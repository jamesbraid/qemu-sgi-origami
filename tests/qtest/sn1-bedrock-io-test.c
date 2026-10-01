/* SPDX-License-Identifier: BSD-3-Clause */
/* Isolated Bedrock I/O register, PIO, inbound and DMA tests. */
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

/*
 * A widget reached through the PIO window can answer synchronously, and its
 * answer arrives back at this device's inbound region: an ordinary DMA write,
 * or an interrupt packet aimed at the node's registers. Both land while the
 * PIO callback is still running. Linking widget 8 to this device's own
 * inbound region stands in for that round trip; the crossbar and the device
 * behind it add nothing the re-entrancy guard can see, because the flag it
 * tests belongs to whichever device's callback is running, not to the region
 * named in its warning.
 */
#define MACHINE_LOOPBACK_BASE \
    "-M none -cpu R10000 -m 256M " \
    "-object memory-backend-ram,id=dma,size=32M " \
    "-object memory-backend-ram,id=hub,size=8M " \
    "-device sgi-bedrock-io,id=io," \
    "dma-memory=/objects/dma/dma[0]," \
    "widget-8=" IO_PATH "/sgi-bedrock-io-inbound[0],"
#define MACHINE_LOOPBACK \
    MACHINE_LOOPBACK_BASE "hub-registers=/objects/hub/hub[0] -S"
/* The node's register window pointed back at inbound, to re-enter it. */
#define MACHINE_LOOPBACK_HUB \
    MACHINE_LOOPBACK_BASE \
    "hub-registers=" IO_PATH "/sgi-bedrock-io-inbound[0] -S"

/*
 * Big window 0 carries the XIO register form, big window 1 plain
 * memory, once the translation entries below are programmed. Both reach the
 * widget, which is this device's inbound region.
 */
#define PIO_REGISTER_FORM(offset) (PIO_BASE + 0x20000000 + (offset))
#define PIO_MEMORY_FORM(offset) (PIO_BASE + 0x40000000 + (offset))

/* XIO address terms of the interrupt-send form. */
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
#define ERROR_CLEAR    0x1f8
#define LINK_LOG       0x130
#define SCRATCH0       0x150
#define MISC_ERROR_MASK 0x1e8
#define ITTE0          0x160
#define BTE0           0x10000
#define BTE1           0x20000

/*
 * The default inbound-widget-mask names the two widget numbers an XBridge
 * presents to its host port (hw/pci-host/sgi-xbridge.c CHIP_HEAD_E/F), the
 * only real inbound sources a Bedrock+XBridge machine has. Widget 8, the
 * device's old fixed default, names nothing any such machine ever wires up.
 */
#define IN_WIDGET_E (UINT64_C(1) << 0xe)
#define IN_WIDGET_F (UINT64_C(1) << 0xf)
#define IN_WIDGET_LEGACY_UNUSED (UINT64_C(1) << 8)

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
 * What the model refuses or leaves unimplemented is part of its contract, and
 * the only trace of it is the log. These run QEMU with -d unimp into a file
 * and hand its contents back once the machine has exited.
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
    g_autofree char *with_log = g_strdup_printf("-d guest_errors,unimp -D %s "
                                                "%s", log, extra);

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

static void test_dropped_writes_logged(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *contents = NULL;
    QTestState *s = io_start_logged(log, "");

    qtest_writeq(s, REG_BASE, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE), ==, UINT64_C(0x1c11006c));
    qtest_writeq(s, REG_BASE + ERROR_CLEAR, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + ERROR_CLEAR), ==, 0);
    qtest_quit(s);
    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents, "widget identification"));
    g_assert_nonnull(strstr(contents, "error clear"));
}

/*
 * The unit's DMA memory is a separate backend, not the machine's RAM. The
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
 * A widget's view of the node's register space: XIO bit 47 set, the
 * node in bits 46:39, then the Bedrock's own small window in its remote form.
 * The linked hub-registers RAM is reachable only this way.
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
     * The DMA line at the same 39-bit address shows the register form never
     * reaches memory, and the memory form never reaches a register.
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

    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX & ~(IN_WIDGET_E |
                                                          IN_WIDGET_F));
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
                 "0xfefeffffffffffff");
    xio_writeq(s, HUB_REMOTE | 0x90, 0x123);
    qtest_writeq(s, REG_BASE + DEVICE_MASK, UINT64_C(1) << 48);
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
    uint64_t link;

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

    qtest_writeq(s, REG_BASE, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE), ==, UINT64_C(0x1c11006c));
    qtest_writeq(s, REG_BASE + SCRATCH0 + 8, UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s, REG_BASE + SCRATCH0 + 8), ==,
                    UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s, REG_BASE + SCRATCH0), ==,
                    UINT64_C(0x0123456789abcdef));
    link = qtest_readq(s, REG_BASE + LINK_STATUS);
    qtest_writeq(s, REG_BASE + LINK_STATUS, link & ~(UINT64_C(1) << 10));
    g_assert_false(qtest_readq(s, REG_BASE + LINK_STATUS) & (1 << 10));
    g_assert_true(qtest_readq(s, REG_BASE + LINK_STATUS) & (1 << 13));
    qtest_writeq(s, REG_BASE + LINK_STATUS, link | (1 << 10));
    g_assert_true(qtest_readq(s, REG_BASE + LINK_STATUS) & (1 << 10));
    qtest_writeq(s, REG_BASE + LINK_LOG, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + LINK_LOG), ==, 0);
    qtest_writeq(s, REG_BASE + BTE0 + 0x28, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 0x28), ==, 0x7f01ff);
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
        { 0x1e8, 0x1000, 0x1111 },
        { 0x1f0, UINT64_MAX, UINT64_C(0x0fffffff03ffffff) },
        { 0x200, UINT64_MAX, 0x10f },
        { 0x3a8, UINT64_MAX, UINT64_C(0x13ffffffff30f3) },
        { 0x3b0, UINT64_MAX, 0x1ff0f },
        { 0x3b8, UINT64_MAX, 0xff },
        { 0x3c0, UINT64_MAX, 0xffffff },
    };
    const uint8_t bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    for (size_t i = 0; i < ARRAY_SIZE(registers); i++) {
        qtest_writeq(s, REG_BASE + registers[i].offset, registers[i].value);
        g_assert_cmphex(qtest_readq(s, REG_BASE + registers[i].offset), ==,
                        registers[i].expected);
    }
    /*
     * Model readback mask combines the stored header fields. Only bits 13
     * and 10 have established meanings; identity-registers checks them.
     */
    qtest_writeq(s, REG_BASE + LINK_STATUS, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + LINK_STATUS), ==,
                    UINT64_C(0x3ff0fff7d3f));
    qtest_writeq(s, REG_BASE + 0x138, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + 0x138), ==, 0x101ff7f);
    /*
     * Model policy: registers without known field maps, including the
     * graphics maps among them, are 64-bit retained storage in the model.
     * Their hardware field widths are not established.
     */
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
    /*
     * Model policy: nine PIO request buffers retain all 64 bits. Known
     * fields do not limit storage; this does not assert a hardware field map.
     */
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
    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX & ~(IN_WIDGET_E |
                                                          IN_WIDGET_F));
    dma_writeq(s, 0x190000, UINT64_C(0xfedcba9876543210));
    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX);
    g_assert_cmphex(dma_readq(s, 0x190000), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_writel(s, IN_BASE + 0x190008, 0xa55a3cc3);
    g_assert_cmphex(qtest_readl(s, IN_BASE + 0x190008), ==, 0xa55a3cc3);
    assert_ram_filled(s, 0x190000, 0x77, 16);
    qtest_quit(s);

    /* Device-0 bits for heads 0xe and 0xf occupy bits 48 and 56. */
    s = io_start("-global sgi-bedrock-io.device-mask-reset="
                 "0xfefeffffffffffff");
    dma_writeq(s, 0x190000, UINT64_MAX);
    qtest_writeq(s, REG_BASE + DEVICE_MASK, UINT64_C(1) << 48);
    dma_writeq(s, 0x190000, UINT64_C(0x1111111111111111));
    qtest_writeq(s, REG_BASE + DEVICE_MASK, UINT64_C(1) << 56);
    g_assert_cmphex(dma_readq(s, 0x190000), ==, 0);
    dma_writeq(s, 0x190000, UINT64_C(0x2222222222222222));
    g_assert_cmphex(dma_readq(s, 0x190000), ==,
                    UINT64_C(0x2222222222222222));
    qtest_quit(s);

    s = io_start("-global sgi-bedrock-io.device-mask-reset="
                 "0xfefeffffffffffff");
    qtest_writeq(s, REG_BASE + DEVICE_MASK, UINT64_C(1) << 56);
    dma_writeq(s, 0x190000, UINT64_C(0x3333333333333333));
    qtest_writeq(s, REG_BASE + DEVICE_MASK, UINT64_C(1) << 48);
    g_assert_cmphex(dma_readq(s, 0x190000), ==, 0);
    qtest_quit(s);
}

/*
 * The shared window cannot identify a head; a split permission must refuse.
 */
static void test_inbound_source_widgets(void)
{
    QTestState *s = io_start("");

    dma_writeq(s, 0x191000, UINT64_C(0x1111111111111111));
    g_assert_cmphex(dma_readq(s, 0x191000), ==,
                    UINT64_C(0x1111111111111111));

    /* Widget 8 was the old fixed default; it names no real source now. */
    qtest_writeq(s, REG_BASE + IN_ACCESS,
                UINT64_MAX & ~IN_WIDGET_LEGACY_UNUSED);
    dma_writeq(s, 0x191000, UINT64_C(0x2222222222222222));
    g_assert_cmphex(dma_readq(s, 0x191000), ==,
                    UINT64_C(0x2222222222222222));

    /* A denied head must not borrow the other head's permission. */
    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX & ~IN_WIDGET_E);
    dma_writeq(s, 0x191000, UINT64_C(0x3333333333333333));
    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX & ~IN_WIDGET_F);
    dma_writeq(s, 0x191000, UINT64_C(0x4444444444444444));

    /*
     * Refusing both heads also refuses the read back. Restore access before
     * checking that none of the refused writes landed.
     */
    qtest_writeq(s, REG_BASE + IN_ACCESS,
                UINT64_MAX & ~(IN_WIDGET_E | IN_WIDGET_F));
    dma_writeq(s, 0x191000, UINT64_C(0x5555555555555555));
    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX);
    g_assert_cmphex(dma_readq(s, 0x191000), ==,
                    UINT64_C(0x2222222222222222));
    dma_writeq(s, 0x191000, UINT64_C(0x6666666666666666));
    g_assert_cmphex(dma_readq(s, 0x191000), ==,
                    UINT64_C(0x6666666666666666));
    qtest_quit(s);
}

/* A denied candidate on the shared window is never silent. */
static void test_inbound_refusal_is_logged(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *contents = NULL;
    QTestState *s = io_start_logged(log, "");

    qtest_writeq(s, REG_BASE + IN_ACCESS,
                UINT64_MAX & ~(IN_WIDGET_E | IN_WIDGET_F));
    dma_writeq(s, 0x192000, UINT64_C(0x6666666666666666));
    qtest_quit(s);
    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents, "sgi-bedrock-io: inbound write"));
    g_assert_nonnull(strstr(contents, "refused for widget mask"));
}

static QTestState *io_start_loopback(const char *machine, const char *extra)
{
    QTestState *s = qtest_initf("%s %s", machine, extra);

    /*
     * Registers and the PIO window only. Inbound is deliberately left
     * unmapped: an address space built over a region that is also a subregion
     * of the system space renders it at that system address, so mapping
     * inbound here would move it out of reach of the XIO addresses the
     * PIO window forms.
     */
    qtest_sysbus_mmio_map(s, IO_PATH, 0, REG_BASE);
    qtest_sysbus_mmio_map(s, IO_PATH, 1, PIO_BASE);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    return s;
}

static void program_loopback_entries(QTestState *s)
{
    qtest_writeq(s, REG_BASE + ITTE0, 0x1800);     /* widget 8, I/O space */
    qtest_writeq(s, REG_BASE + ITTE0 + 8, 0x800);  /* widget 8, memory */
}

static void test_pio_target_answers_through_inbound(void)
{
    QTestState *s = io_start_loopback(MACHINE_LOOPBACK, "");

    program_loopback_entries(s);

    /*
     * An interrupt packet: the widget writes the node's register window in
     * its checked remote form while our PIO callback is still on the stack.
     * The same door reads it back out of the node's registers.
     */
    qtest_writeq(s, PIO_REGISTER_FORM(HUB_REMOTE + 0x90),
                 UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, PIO_REGISTER_FORM(HUB_REMOTE + 0x90)), ==,
                    UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, PIO_REGISTER_FORM(HUB_REMOTE + 0x98)), ==,
                    0);

    /* The same nesting for an ordinary inbound DMA write to memory. */
    qtest_writeq(s, PIO_MEMORY_FORM(0x2000), UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s, PIO_MEMORY_FORM(0x2000)), ==,
                    UINT64_C(0xfedcba9876543210));

    /* Neither path escapes the inbound access check. */
    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX & ~(IN_WIDGET_E |
                                                          IN_WIDGET_F));
    qtest_writeq(s, PIO_MEMORY_FORM(0x2008), UINT64_MAX);
    qtest_writeq(s, PIO_REGISTER_FORM(HUB_REMOTE + 0xa0), UINT64_MAX);
    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PIO_MEMORY_FORM(0x2008)), ==, 0);
    g_assert_cmphex(qtest_readq(s, PIO_REGISTER_FORM(HUB_REMOTE + 0xa0)), ==,
                    0);
    qtest_quit(s);

    /* Nor the device mask.  It is write-one-to-set, so start it clear. */
    s = io_start_loopback(MACHINE_LOOPBACK,
                          "-global sgi-bedrock-io.device-mask-reset=0");
    program_loopback_entries(s);
    qtest_writeq(s, PIO_MEMORY_FORM(0x2000), UINT64_MAX);
    qtest_writeq(s, REG_BASE + DEVICE_MASK, 1);
    g_assert_cmphex(qtest_readq(s, PIO_MEMORY_FORM(0x2000)), ==, 0);
    qtest_quit(s);
}

static void test_inbound_keeps_its_guard(void)
{
    QTestState *s = io_start_loopback(MACHINE_LOOPBACK_HUB, "");

    program_loopback_entries(s);

    /* One level of nesting reaches memory: inbound answers a PIO. */
    qtest_writeq(s, PIO_MEMORY_FORM(0x90), UINT64_C(0x1111111111111111));
    g_assert_cmphex(qtest_readq(s, PIO_MEMORY_FORM(0x90)), ==,
                    UINT64_C(0x1111111111111111));

    /*
     * Two levels must not. Here the node's register window is inbound itself,
     * so forwarding re-enters that region while its own callback runs, and
     * its guard refuses the write; memory keeps the earlier value. Exempting
     * inbound as well would let this through.
     */
    qtest_writeq(s, PIO_REGISTER_FORM(HUB_REMOTE + 0x90),
                 UINT64_C(0x2222222222222222));
    g_assert_cmphex(qtest_readq(s, PIO_MEMORY_FORM(0x90)), ==,
                    UINT64_C(0x1111111111111111));
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
    /* Data and notification are in place with no virtual time passed. */
    dma_read(s, 0x110000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
    g_assert_cmphex(dma_readq(s, 0x120000), ==, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 0);
    /* The model's choice: both addresses end one line past the last moved. */
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 8), ==, 0x100100);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 0x10), ==, 0x110100);
    assert_ram_filled(s, 0x110000, 0xee, sizeof(source));
    assert_ram_filled(s, 0x120000, 0xee, 8);

    /* A finished engine takes new programming. */
    qtest_writeq(s, REG_BASE + BTE0, 7 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 8, 0x130000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, 0x140000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x20, 0x150000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x28, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 7 | (1 << 20));
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 8), ==, 0x130000);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 0x10), ==, 0x140000);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 0x20), ==, 0x150000);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 0x28), ==, 0x7f01ff);

    dma_set(s, 0x130000, 0xcc, sizeof(zero));
    qtest_writeq(s, REG_BASE + BTE1, 1 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE1 + 0x10, 0x130000);
    qtest_writeq(s, REG_BASE + BTE1 + 0x20, 0x140000);
    qtest_writeq(s, REG_BASE + BTE1 + 0x18, 1 | (1 << 4));
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
    /*
     * A control read terminates a transfer in progress, but the transfer
     * finished inside its starting write: nothing is left to stop.
     */
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 0x18), ==, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 0);
    dma_read(s, 0x160000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
    dma_write(s, 0x160000, unchanged, sizeof(unchanged));

    /*
     * Interrupt mode: a notification address is programmed but notify is
     * clear, so completion leaves that line alone and raises the completion
     * interrupt through the node register window instead. Delivery is
     * deferred to a bottom half, so it lands by the next access rather than
     * re-entering this interface from inside the launching write.
     */
    dma_writeq(s, 0x1e0000, UINT64_MAX);
    qtest_writeq(s, REG_BASE + BTE1, 2 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE1 + 8, 0x150000);
    qtest_writeq(s, REG_BASE + BTE1 + 0x10, 0x170000);
    qtest_writeq(s, REG_BASE + BTE1 + 0x20, 0x1e0000);
    qtest_writeq(s, REG_BASE + BTE1 + 0x18, 0);
    dma_read(s, 0x160000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), unchanged, sizeof(unchanged));
    dma_read(s, 0x170000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
    g_assert_cmphex(dma_readq(s, 0x1e0000), ==, UINT64_MAX);
    /*
     * The deferred completion interrupt reached the node's pending-modify
     * register through the hub window: a set (bit 8) of level zero, the
     * engine's unprogrammed interrupt-address default. Reading it back the
     * XIO way also confirms the write was not dropped as re-entrant.
     */
    g_assert_cmphex(xio_readq(s, HUB_REMOTE + 0x90), ==, UINT64_C(1) << 8);

    qtest_writeq(s, REG_BASE + BTE0, 2 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 8, UINT64_C(0x80000000));
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, 0x160000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x20, 0x180000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 1 << 4);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 2 | (1 << 16));
    g_assert_cmphex(dma_readq(s, 0x180000), ==, 2 | (1 << 16));

    qtest_writeq(s, REG_BASE + BTE0, 1 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 8, 0x150000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, UINT64_C(0x80000000));
    qtest_writeq(s, REG_BASE + BTE0 + 0x20, 0x180080);
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 1 << 4);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 1 | (1 << 16));
    g_assert_cmphex(dma_readq(s, 0x180080), ==, 1 | (1 << 16));

    dma_set(s, 0x160000, 0, 128);
    qtest_writeq(s, REG_BASE + BTE0, 1 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 8, 0x150000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, 0x160000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x20, UINT64_C(0x80000000));
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 1 << 4);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 0);
    dma_read(s, 0x160000, actual, 128);
    g_assert_cmpmem(actual, 128, source, 128);

    qtest_writeq(s, REG_BASE + BTE0, 1 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 1 << 8);
    qtest_clock_step(s, 2);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 1 | (1 << 20));
    /*
     * The control read on an engine that was armed but never started is only
     * cleanup here. What it does to that engine's busy bit is the model's
     * extrapolation, so it is not asserted.
     */
    qtest_readq(s, REG_BASE + BTE0 + 0x18);
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

static void test_itanium_probe_sequence(void)
{
    QTestState *s = io_start("");
    uint64_t saved, value;

    /* Per-hub pass, register by register, in the consumer's order. */
    qtest_writeq(s, REG_BASE + 0x3a8, 0xf << 4);
    qtest_writeq(s, REG_BASE + 0x200, 8);
    qtest_writeq(s, REG_BASE + 0x3c0, 0x1000);
    qtest_writeq(s, REG_BASE + 0x3b8, 0xff);
    for (unsigned i = 0; i < 9; i++) {
        uint64_t offset = i ? 0x1a0 + (i - 1) * 8 : 0x198;

        value = qtest_readq(s, REG_BASE + offset);
        value &= ~(UINT64_C(0x1f) << 43 | UINT64_C(1) << 42 |
                   UINT64_C(0x3fff) << 24 | UINT64_C(0x3fff) << 8 | 0xff);
        value |= UINT64_C(1) << 43 | 3;
        qtest_writeq(s, REG_BASE + offset, value);
        g_assert_cmphex(qtest_readq(s, REG_BASE + offset), ==,
                        (UINT64_C(1) << 43) | 3);
    }
    qtest_writeq(s, REG_BASE + 0x1f8, UINT64_MAX);
    value = qtest_readq(s, REG_BASE + 0x138);
    qtest_writeq(s, REG_BASE + 0x138, value & ~(UINT64_C(1) << 28));
    qtest_writeq(s, REG_BASE + 0x138, (1 << 24) | 102);

    /*
     * Widget-probe pass. The consumer's six disable writes name widget 3 but
     * all land on entry 0, the window number spilling into bit 12 and above.
     */
    for (unsigned window = 0; window < 6; window++) {
        qtest_writeq(s, REG_BASE + ITTE0, ((uint64_t)window << 12) | 0x300);
    }
    saved = qtest_readq(s, REG_BASE + OUT_ACCESS);
    qtest_writeq(s, REG_BASE + OUT_ACCESS, 0);
    qtest_writeq(s, REG_BASE + 0x198,
                 (qtest_readq(s, REG_BASE + 0x198) & ~UINT64_C(0xff)) | 1);
    for (unsigned w = 8; w < 16; w++) {
        uint64_t offset = 0x1a0 + (w - 8) * 8;

        qtest_writeq(s, REG_BASE + offset,
                     (qtest_readq(s, REG_BASE + offset) & ~UINT64_C(0xff)) | 3);
    }
    qtest_writeq(s, REG_BASE + OUT_ACCESS, saved);

    g_assert_cmphex(qtest_readq(s, REG_BASE + 0x3a8), ==, 0xf0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + 0x200), ==, 8);
    g_assert_cmphex(qtest_readq(s, REG_BASE + 0x3c0), ==, 0x1000);
    g_assert_cmphex(qtest_readq(s, REG_BASE + 0x3b8), ==, 0xff);
    g_assert_cmphex(qtest_readq(s, REG_BASE + 0x1f8), ==, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + 0x138), ==, (1 << 24) | 102);
    g_assert_cmphex(qtest_readq(s, REG_BASE + 0x198), ==,
                    (UINT64_C(1) << 43) | 1);
    for (unsigned w = 8; w < 16; w++) {
        g_assert_cmphex(qtest_readq(s, REG_BASE + 0x1a0 + (w - 8) * 8), ==,
                        (UINT64_C(1) << 43) | 3);
    }
    g_assert_cmphex(qtest_readq(s, REG_BASE + ITTE0), ==, 0x1300);
    for (unsigned entry = 1; entry < 6; entry++) {
        g_assert_cmphex(qtest_readq(s, REG_BASE + ITTE0 + entry * 8), ==, 0);
    }
    g_assert_cmphex(qtest_readq(s, REG_BASE + OUT_ACCESS), ==, UINT64_MAX);
    qtest_quit(s);
}

/*
 * The PROM's hub BTE check starts engine 0 and then engine 1 on equal copies,
 * waits for engine 0's completion interrupt alone, and compares both copies.
 * On hardware the two starting stores issue back to back, so engine 1 is
 * taken to finish close behind engine 0 (inferred, not measured). Under TCG
 * the host can take milliseconds between the stores, and without instruction
 * counting that host time is virtual time: the step below stands for it, long
 * enough for engine 0 to finish and raise its interrupt. Engine 1's copy must
 * still be whole when it is compared.
 */
static void test_bte_back_to_back(void)
{
    QTestState *s = io_start("");
    enum { LINES = 64, SIZE = LINES * 128 };
    g_autofree uint8_t *first = g_malloc(SIZE);
    g_autofree uint8_t *second = g_malloc(SIZE);
    g_autofree uint8_t *actual = g_malloc(SIZE);

    fill_pattern(first, SIZE, 0xc3);
    fill_pattern(second, SIZE, 0xa5);
    dma_write(s, 0x100000, first, SIZE);
    dma_write(s, 0x110000, second, SIZE);
    dma_set(s, 0x120000, 0xde, SIZE);
    dma_set(s, 0x130000, 0xde, SIZE);
    /* Interrupt mode, node 0, interface 0: levels 0x3f and 0x3e. */
    qtest_writeq(s, REG_BASE + BTE0, LINES | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 8, 0x100000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, 0x120000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x28, 0x3f << 16);
    qtest_writeq(s, REG_BASE + BTE1, LINES | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE1 + 8, 0x110000);
    qtest_writeq(s, REG_BASE + BTE1 + 0x10, 0x130000);
    qtest_writeq(s, REG_BASE + BTE1 + 0x28, 0x3e << 16);

    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 0);
    qtest_clock_step(s, 10 * LINES);
    g_assert_cmphex(xio_readq(s, HUB_REMOTE + 0x90), ==, (1 << 8) | 0x3f);
    qtest_writeq(s, REG_BASE + BTE1 + 0x18, 0);
    dma_read(s, 0x120000, actual, SIZE);
    g_assert_cmpmem(actual, SIZE, first, SIZE);
    dma_read(s, 0x130000, actual, SIZE);
    g_assert_cmpmem(actual, SIZE, second, SIZE);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE1), ==, 0);
    qtest_quit(s);
}

static void test_bte_error_after_progress(void)
{
    QTestState *s = io_start("");
    uint8_t source[128], actual[256], unchanged[256];

    /* Source line 1 is the DMA region's last line; line 2 lies beyond it. */
    fill_pattern(source, sizeof(source), 0x71);
    memset(unchanged, 0x19, sizeof(unchanged));
    dma_write(s, 0x2000000 - 128, source, sizeof(source));
    dma_write(s, 0x130000, unchanged, sizeof(unchanged));
    dma_writeq(s, 0x140000, UINT64_MAX);
    qtest_writeq(s, REG_BASE + BTE0, 3 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 8, 0x2000000 - 128);
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, 0x130000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x20, 0x140000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 1 << 4);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 2 | (1 << 16));
    g_assert_cmphex(dma_readq(s, 0x140000), ==, 2 | (1 << 16));
    /* The model's choice: both addresses name the failed line. */
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 8), ==, 0x2000000);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 0x10), ==, 0x130080);
    dma_read(s, 0x130000, actual, sizeof(actual));
    g_assert_cmpmem(actual, 128, source, 128);
    g_assert_cmpmem(actual + 128, 128, unchanged + 128, 128);
    qtest_quit(s);
}

static void test_write_one_to_set_masks(void)
{
    QTestState *s = io_start("-global sgi-bedrock-io.device-mask-reset=0");

    qtest_writeq(s, REG_BASE + DEVICE_MASK, 0x10);
    g_assert_cmphex(qtest_readq(s, REG_BASE + DEVICE_MASK), ==, 0x10);
    qtest_writeq(s, REG_BASE + DEVICE_MASK, 0x100);
    g_assert_cmphex(qtest_readq(s, REG_BASE + DEVICE_MASK), ==, 0x110);
    qtest_writeq(s, REG_BASE + DEVICE_MASK, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + DEVICE_MASK), ==, 0x110);
    /* The enable bits start set, so writing zero cannot clear them. */
    qtest_writeq(s, REG_BASE + MISC_ERROR_MASK, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + MISC_ERROR_MASK), ==, 0x111);
    qtest_writeq(s, REG_BASE + MISC_ERROR_MASK, 0x1000);
    g_assert_cmphex(qtest_readq(s, REG_BASE + MISC_ERROR_MASK), ==, 0x1111);
    qtest_writeq(s, REG_BASE + MISC_ERROR_MASK, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + MISC_ERROR_MASK), ==, 0x1111);
    qtest_quit(s);
}

static void test_pio_unlinked_widget(void)
{
    QTestState *s = io_start("");

    qtest_writel(s, 0x100, 0x0badcafe);
    dma_writeq(s, 0x100, UINT64_C(0xd00d5eedd00d5eed));
    qtest_writeq(s, REG_BASE + ITTE0, 0xa00);
    qtest_writel(s, PIO_BASE + 0x20000100, 0x12345678);
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x20000100), ==, 0);
    qtest_writel(s, PIO_BASE + 0xa000100, 0x12345678);
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0xa000100), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x100), ==, 0x0badcafe);
    g_assert_cmphex(dma_readq(s, 0x100), ==, UINT64_C(0xd00d5eedd00d5eed));
    qtest_writeq(s, REG_BASE + ITTE0, 0x800);
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x20000100), ==, 0);
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x8000100), ==, 0);
    qtest_quit(s);
}

static void test_reset(void)
{
    QTestState *s = io_start("");

    qtest_writeq(s, REG_BASE + SCRATCH0, UINT64_MAX);
    qtest_writeq(s, REG_BASE + ITTE0, 0x1802);
    qtest_writeq(s, REG_BASE + BTE0, 3 | (1 << 20));
    qtest_writeq(s, REG_BASE + MISC_ERROR_MASK, 0x1000);
    qtest_system_reset(s);
    /*
     * Local access protection resets to all ones. The miscellaneous error
     * mask comes out of reset with widget 0 and both block transfer engines
     * enabled: the hub clears those bits only to shut a source down after an
     * error, and the vendor's system-level diagnostic fails a freshly booted
     * node whose engine bits are clear. The zeros are the model's choices for
     * registers whose hardware reset value is unknown.
     */
    g_assert_cmphex(qtest_readq(s, REG_BASE + SCRATCH0), ==, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + ITTE0), ==, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + LOCAL_PROTECT), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + MISC_ERROR_MASK), ==, 0x111);
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

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-io-migration-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    uint8_t source[256], actual[256];

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = io_start("");
    to = io_start("-incoming defer");
    fill_pattern(source, sizeof(source), 0xc7);
    /*
     * The DMA backend is not migrated RAM. Seed the destination's copy so an
     * engine programmed before migration and started after it has a source to
     * move.
     */
    dma_write(to, 0x1a0000, source, sizeof(source));
    dma_set(to, 0x1b0000, 0, sizeof(source));
    dma_writeq(to, 0x1c0000, UINT64_MAX);
    qtest_writeq(from, REG_BASE + SCRATCH0, UINT64_C(0x13579bdf2468ace0));
    qtest_writeq(from, REG_BASE + ITTE0, 0x1802);
    qtest_writeq(from, REG_BASE + BTE0, 2 | (1 << 20));
    qtest_writeq(from, REG_BASE + BTE0 + 8, 0x1a0000);
    qtest_writeq(from, REG_BASE + BTE0 + 0x10, 0x1b0000);
    qtest_writeq(from, REG_BASE + BTE0 + 0x20, 0x1c0000);
    qtest_writeq(from, REG_BASE + BTE0 + 0x28, 0x3f << 16);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    migrate_wait(from);
    /* The source completes when its state is sent, not when it is loaded. */
    migrate_wait(to);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    g_assert_cmphex(qtest_readq(to, REG_BASE + SCRATCH0), ==,
                    UINT64_C(0x13579bdf2468ace0));
    g_assert_cmphex(qtest_readq(to, REG_BASE + ITTE0), ==, 0x1802);
    g_assert_cmphex(qtest_readq(to, REG_BASE + BTE0), ==, 2 | (1 << 20));
    g_assert_cmphex(qtest_readq(to, REG_BASE + BTE0 + 8), ==, 0x1a0000);
    g_assert_cmphex(qtest_readq(to, REG_BASE + BTE0 + 0x10), ==, 0x1b0000);
    g_assert_cmphex(qtest_readq(to, REG_BASE + BTE0 + 0x20), ==, 0x1c0000);
    g_assert_cmphex(qtest_readq(to, REG_BASE + BTE0 + 0x28), ==, 0x3f << 16);
    qtest_writeq(to, REG_BASE + BTE0 + 0x18, 1 << 4);
    g_assert_cmphex(qtest_readq(to, REG_BASE + BTE0), ==, 0);
    g_assert_cmphex(dma_readq(to, 0x1c0000), ==, 0);
    dma_read(to, 0x1b0000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_explicit_source_context(void)
{
    const uint64_t source = UINT64_C(0x40000000000);
    const uint64_t status = UINT64_C(0x60000000);
    QTestState *s = io_start(
        "-global sgi-bedrock-io.device-mask-reset=0xfffdffffffffffff "
        "-device sgi-xio-test-target,id=source,"
        "target-port=io,source-widget=14,source-device=1");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 1, source);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 2, status);
    qtest_writeq(s, source + 0x1000, 0x12345678);
    g_assert_cmpuint(qtest_readq(s, status), !=, 0);
    g_assert_cmphex(qtest_readq(s, IN_BASE + 0x1000), ==, 0);
    /* Device-mask writes reenable sources; they cannot clear a bit. */
    qtest_writeq(s, REG_BASE + DEVICE_MASK, UINT64_C(1) << 49);
    qtest_writeq(s, source + 0x1000, 0x12345678);
    g_assert_cmpuint(qtest_readq(s, status), ==, 0);
    g_assert_cmphex(qtest_readq(s, IN_BASE + 0x1000), ==, 0x12345678);
    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX & ~IN_WIDGET_E);
    qtest_writeq(s, source + 0x1000, 0x87654321);
    g_assert_cmpuint(qtest_readq(s, status), !=, 0);
    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, IN_BASE + 0x1000), ==, 0x12345678);
    qtest_quit(s);
}

static void test_partial_source_context(void)
{
    const uint64_t source = UINT64_C(0x40000000000);
    const uint64_t status = UINT64_C(0x60000000);
    QTestState *s = io_start(
        "-global sgi-bedrock-io.device-mask-reset=0xffdfffffffffffff "
        "-device sgi-xio-test-target,id=source,"
        "target-port=io,source-widget=14");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 1, source);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 2, status);
    qtest_writeq(s, source + 0x1000, 0x12345678);
    g_assert_cmpuint(qtest_readq(s, status), !=, 0);
    g_assert_cmphex(qtest_readq(s, IN_BASE + 0x1000), ==, 0);
    qtest_writeq(s, REG_BASE + DEVICE_MASK, UINT64_C(1) << 53);
    qtest_writeq(s, source + 0x1000, 0x12345678);
    g_assert_cmpuint(qtest_readq(s, status), ==, 0);
    g_assert_cmphex(qtest_readq(s, IN_BASE + 0x1000), ==, 0x12345678);
    qtest_quit(s);
}

static void test_unknown_source_context(void)
{
    const uint64_t source = UINT64_C(0x40000000000);
    const uint64_t status = UINT64_C(0x60000000);
    QTestState *s = io_start(
        "-device sgi-xio-test-target,id=source,target-port=io");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 1, source);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 2, status);
    qtest_writeq(s, source + 0x1000, 0x12345678);
    g_assert_cmpuint(qtest_readq(s, status), !=, 0);
    g_assert_cmphex(qtest_readq(s, IN_BASE + 0x1000), ==, 0);
    qtest_quit(s);
}

static void test_remote_source_permission(void)
{
    const uint64_t source = UINT64_C(0x1000000000000000);
    const uint64_t status = UINT64_C(0x60000000);
    const uint64_t peer_regs = UINT64_C(0x70000000);
    uint64_t remote = source + XIO_REGISTER + XIO_NODE(1) + HUB_REMOTE + 0x1000;
    QTestState *s = io_start(
        "-device sgi-bedrock-io,id=peer,node-number=1,"
        "device-mask-reset=0xfffdffffffffffff,"
        "dma-memory=/objects/dma/dma[0],hub-registers=/objects/hub/hub[0] "
        "-device sgi-xio-test-target,id=source,target-port=io,"
        "target-peer=peer,source-widget=14,source-device=1");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 1, source);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 2, status);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/peer", 0, peer_regs);
    qtest_writeq(s, remote, 0x12345678);
    g_assert_cmpuint(qtest_readq(s, status), !=, 0);
    g_assert_cmphex(qtest_readq(s, IN_BASE + XIO_REGISTER +
                               HUB_REMOTE + 0x1000), ==, 0);
    qtest_writeq(s, peer_regs + DEVICE_MASK, UINT64_C(1) << 49);
    qtest_writeq(s, remote, 0x12345678);
    g_assert_cmpuint(qtest_readq(s, status), ==, 0);
    g_assert_cmphex(qtest_readq(s, IN_BASE + XIO_REGISTER +
                               HUB_REMOTE + 0x1000), ==, 0x12345678);
    qtest_quit(s);
}

static void test_source_context_keeps_guard(void)
{
    const uint64_t source = UINT64_C(0x1000000000000000);
    const uint64_t status = UINT64_C(0x60000000);
    QTestState *s = io_start_loopback(MACHINE_LOOPBACK_HUB,
        "-device sgi-xio-test-target,id=source,target-port=io,"
        "source-widget=14,source-device=1");

    program_loopback_entries(s);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 1, source);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 2, status);
    qtest_writeq(s, source + XIO_REGISTER + HUB_REMOTE + 0x1000, 0x12345678);
    g_assert_cmpuint(qtest_readq(s, status), !=, 0);
    g_assert_cmphex(qtest_readq(s, PIO_MEMORY_FORM(0x1000)), ==, 0);
    qtest_quit(s);
}

static void test_source_context_address_bounds(void)
{
    const uint64_t source = UINT64_C(0x1000000000000000);
    const uint64_t status = UINT64_C(0x60000000);
    uint64_t address = source + XIO_REGISTER + HUB_REMOTE + 0x1000;
    QTestState *s = io_start(
        "-device sgi-xio-test-target,id=source,target-port=io,"
        "source-widget=14,source-device=1");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 1, source);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 2, status);
    qtest_writeq(s, address, 0x12345678);
    g_assert_cmpuint(qtest_readq(s, status), ==, 0);
    qtest_writeq(s, address + (UINT64_C(1) << 48), 0x87654321);
    g_assert_cmpuint(qtest_readq(s, status), !=, 0);
    g_assert_cmphex(qtest_readq(s, address), ==, 0x12345678);
    qtest_quit(s);
}

static void test_source_destination_callback(void)
{
    const uint64_t source = UINT64_C(0x1000000000000000);
    const uint64_t status = UINT64_C(0x60000000);
    const uint64_t address = XIO_REGISTER | HUB_REMOTE | 0x1000;
    QTestState *s = io_start(
        "-device sgi-xio-test-target,id=source,target-port=io,"
        "source-widget=14,source-device=1,destination-callback=on");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 1, source);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/source", 2, status);
    qtest_writeq(s, REG_BASE + 0x118, 0);
    qtest_writeq(s, source + address, 0x12345678);
    g_assert_cmpuint(qtest_readq(s, status + 8), ==, 0);
    qtest_writeq(s, REG_BASE + 0x118, UINT64_MAX);
    qtest_writeq(s, source + address, 0x12345678);
    g_assert_cmpuint(qtest_readq(s, status), !=, 0);
    g_assert_cmpuint(qtest_readq(s, status + 8), ==, 1);
    g_assert_cmphex(qtest_readq(s, status + 16), ==, address);
    g_assert_cmpuint(qtest_readq(s, status + 24), ==, 14);
    g_assert_cmpuint(qtest_readq(s, status + 32), ==, 1);
    g_assert_cmpuint(qtest_readq(s, status + 40), ==, 1);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn1/bedrock-io/source-destination-callback",
                   test_source_destination_callback);
    qtest_add_func("sn1/bedrock-io/dma-link-required",
                   test_dma_link_required);
    qtest_add_func("sn1/bedrock-io/identity-registers",
                   test_identity_and_registers);
    qtest_add_func("sn1/bedrock-io/widget-status",
                   test_widget_status);
    qtest_add_func("sn1/bedrock-io/dropped-writes",
                   test_dropped_writes_logged);
    qtest_add_func("sn1/bedrock-io/probe-retained",
                   test_probe_and_retained_registers);
    qtest_add_func("sn1/bedrock-io/pio-translation",
                   test_pio_translation_and_protection);
    qtest_add_func("sn1/bedrock-io/source-context-address-bounds",
                   test_source_context_address_bounds);
    qtest_add_func("sn1/bedrock-io/source-context-keeps-guard",
                   test_source_context_keeps_guard);
    qtest_add_func("sn1/bedrock-io/remote-source-permission",
                   test_remote_source_permission);
    qtest_add_func("sn1/bedrock-io/partial-source-context",
                   test_partial_source_context);
    qtest_add_func("sn1/bedrock-io/unknown-source-context",
                   test_unknown_source_context);
    qtest_add_func("sn1/bedrock-io/explicit-source-context",
                   test_explicit_source_context);
    qtest_add_func("sn1/bedrock-io/inbound-protection",
                   test_inbound_protection);
    qtest_add_func("sn1/bedrock-io/inbound-source-widgets",
                   test_inbound_source_widgets);
    qtest_add_func("sn1/bedrock-io/inbound-refusal-is-logged",
                   test_inbound_refusal_is_logged);
    qtest_add_func("sn1/bedrock-io/inbound-register-access",
                   test_inbound_register_access);
    qtest_add_func("sn1/bedrock-io/pio-target-answers-through-inbound",
                   test_pio_target_answers_through_inbound);
    qtest_add_func("sn1/bedrock-io/inbound-keeps-its-guard",
                   test_inbound_keeps_its_guard);
    qtest_add_func("sn1/bedrock-io/bte", test_bte_copy_and_zero);
    qtest_add_func("sn1/bedrock-io/bte-error-termination",
                   test_bte_error_and_termination);
    qtest_add_func("sn1/bedrock-io/bte-programming-edges",
                   test_bte_programming_edges);
    qtest_add_func("sn1/bedrock-io/itanium-probe",
                   test_itanium_probe_sequence);
    qtest_add_func("sn1/bedrock-io/bte-back-to-back", test_bte_back_to_back);
    qtest_add_func("sn1/bedrock-io/bte-error-after-progress",
                   test_bte_error_after_progress);
    qtest_add_func("sn1/bedrock-io/write-one-to-set",
                   test_write_one_to_set_masks);
    qtest_add_func("sn1/bedrock-io/pio-unlinked-widget",
                   test_pio_unlinked_widget);
    qtest_add_func("sn1/bedrock-io/reset", test_reset);
    qtest_add_func("sn1/bedrock-io/migration", test_migration);
    return g_test_run();
}
