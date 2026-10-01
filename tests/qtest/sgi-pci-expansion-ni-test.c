/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Direct NI transport; no firmware or physical slot wiring. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include <glib/gstdio.h>
#include "system/memory.h"
#include "sgi-migrate.h"
#include "sgi-l1-test.h"

#define FIXTURE "-M none -cpu R10000 -S " \
                "-device sgi-pci-expansion-ni-fixture,id=ni"
#define PATH "/machine/peripheral/ni"
#define REQUESTER(n) (UINT64_C(0x4000000000000) + ((uint64_t)(n) << 48))
#define STATUS UINT64_C(0x10000000)
#define RAM UINT64_C(0x20000000)
#define COMPUTE UINT64_C(0x30000000)
#define NODE(n) ((uint64_t)(n) << 33)
#define HUB UINT64_C(0x10030000000)
#define IO UINT64_C(0x20000000000)
#define BIC_HUB (HUB + NODE(7))
#define BIC_IO (IO + NODE(7))
#define COMPUTE_HUB (HUB + NODE(3))
static const unsigned widget[] = { 0xe, 0xf, 8, 9, 0xc, 0xd };

static QTestState *start(const char *extra)
{
    QTestState *qts = qtest_initf(FIXTURE " %s", extra);

    qtest_sysbus_mmio_map(qts, PATH, 0, REQUESTER(0));
    qtest_sysbus_mmio_map(qts, PATH, 1, REQUESTER(1));
    qtest_sysbus_mmio_map(qts, PATH, 2, STATUS);
    qtest_sysbus_mmio_map(qts, PATH, 3, RAM);
    qtest_sysbus_mmio_map(qts, PATH, 4, COMPUTE);
    return qts;
}

static void result(QTestState *qts, unsigned requester, MemTxResult expected)
{
    g_assert_cmphex(qtest_readl(qts, STATUS + requester * 8), ==, expected);
}

static uint64_t head(unsigned requester, unsigned index)
{
    return REQUESTER(requester) + BIC_IO + ((uint64_t)widget[index] << 24);
}

static void configure_card(QTestState *qts, unsigned index)
{
    uint64_t h = head(0, index);

    qtest_writel(qts, h + 0x24,
                 (qtest_readl(qts, h + 0x24) & ~15U) | widget[index]);
    qtest_writel(qts, h + 0x21010, 0x01200000);
    qtest_writel(qts, h + 0x21004, 6);
    qtest_writel(qts, h + 0x20c, 0x81012);
    qtest_writel(qts, h + 0x84, 0xa << 20);
}

static void test_pio(void)
{
    QTestState *qts = start("");
    unsigned i;

    uint64_t bic_revision = qtest_readq(qts,
        REQUESTER(0) + BIC_HUB + 0x600000);

    result(qts, 0, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts,
        REQUESTER(1) + BIC_IO + 0x1800000 + 0x600000), ==, bic_revision);
    result(qts, 1, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts,
        REQUESTER(0) + IO + NODE(3) + 0x1800000 + 0x600000), ==,
        qtest_readq(qts, COMPUTE + 0x600000));
    result(qts, 0, MEMTX_OK);

    for (i = 0; i < 6; i++) {
        uint64_t first = head(0, i), second = head(1, i);

        g_assert_cmphex(qtest_readl(qts, first + 4), ==, 0x2d002049);
        result(qts, 0, MEMTX_OK);
        qtest_writel(qts, first + 0x2c, 0x45000 + i);
        result(qts, 0, MEMTX_OK);
        g_assert_cmphex(qtest_readl(qts, second + 0x2c), ==, 0x45000 + i);
        result(qts, 1, MEMTX_OK);
        configure_card(qts, i);
        g_autofree char *path = g_strdup_printf(PATH "/card%u", i);
        QDict *reply;
        uint8_t bytes[4];
        const uint8_t expected[] = { 0xab, 0xc0, 0, i };

        g_assert_cmphex(qtest_readl(qts, second + 0x400000), ==,
                        0xabc00000 + i);
        result(qts, 1, MEMTX_OK);
        qtest_memread(qts, second + 0x400000, bytes, sizeof(bytes));
        g_assert_cmpmem(bytes, sizeof(bytes), expected, sizeof(expected));
        reply = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
            "{'path':%s,'property':'signature'}}", path);
        g_assert_cmphex(qnum_get_uint(qobject_to(QNum,
            qdict_get(reply, "return"))), ==, 0xabc00000 + i);
        qobject_unref(reply);
    }
    qtest_readl(qts, REQUESTER(0) + BIC_IO + (0xbULL << 24) + 4);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_readq(qts, REQUESTER(0) + NODE(7) + 0x8000);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_readq(qts, REQUESTER(0) + BIC_HUB + 0x90);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_readq(qts, REQUESTER(0) + BIC_HUB + 0x200090);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_readq(qts, REQUESTER(0) + BIC_IO + 0x1800090);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_readq(qts, REQUESTER(0) + BIC_IO + 0x1a00090);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_quit(qts);
}

static void dma(QTestState *qts, unsigned card, uint64_t address,
                uint64_t payload, bool write, MemTxResult expected)
{
    uint64_t bar = head(0, card) + 0x400000;

    g_autofree char *path = g_strdup_printf(PATH "/card%u", card);
    QDict *reply;

    qtest_writel(qts, bar + 8, address);
    qtest_writel(qts, bar + 12, address >> 32);
    qtest_writel(qts, bar + 16, payload);
    qtest_writel(qts, bar + 20, payload >> 32);
    reply = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
        "{'path':%s,'property':'dma-address'}}", path);
    g_assert_cmphex(qnum_get_uint(qobject_to(QNum,
            qdict_get(reply, "return"))), ==, address);
    qobject_unref(reply);
    reply = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
        "{'path':%s,'property':'dma-payload'}}", path);
    g_assert_cmphex(qnum_get_uint(qobject_to(QNum,
            qdict_get(reply, "return"))), ==, payload);
    qobject_unref(reply);
    qtest_writel(qts, bar + 24, write ? 1 : 2);
    g_assert_cmphex(qtest_readl(qts, bar + 24), ==, expected);
    if (!write && expected == MEMTX_OK) {
        uint64_t readback = qtest_readl(qts, bar + 16) |
            (uint64_t)qtest_readl(qts, bar + 20) << 32;

        g_assert_cmphex(readback, ==, payload);
    }
}

static void test_dma(void)
{
    QTestState *qts = start("");
    unsigned i;

    /* PCI64 destination widget a and full physical memory NASID. */
    for (i = 0; i < 6; i++) {
        uint64_t payload = UINT64_C(0xdeadbeef01234000) + i;
        uint64_t address = UINT64_C(0xa080000000000000) + NODE(3) + 0x8000;

        configure_card(qts, i);
        dma(qts, i, address, payload, true, MEMTX_OK);
        g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, payload);
        dma(qts, i, address, payload, false, MEMTX_OK);
        dma(qts, i, UINT64_C(0xa080000000000000) + NODE(7) + 0x8000,
            payload ^ 1, true, MEMTX_DECODE_ERROR);
        g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, payload);
        dma(qts, i, UINT64_C(0xa080000000000000) + NODE(3) + 0x200000,
            payload ^ 1, true, MEMTX_DECODE_ERROR);
        g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, payload);
    }
    qtest_quit(qts);
}

static void set_number(QTestState *qts, unsigned number)
{
    uint64_t revision = qtest_readq(qts, COMPUTE + 0x600000);

    qtest_writeq(qts, COMPUTE + 0x600000,
        (revision & ~(UINT64_C(0xff) << 32)) | (uint64_t)number << 32);
}

static void test_live_number(void)
{
    QTestState *qts = start("");
    uint64_t old = REQUESTER(0) + NODE(3) + 0x8000;
    uint64_t new = REQUESTER(1) + NODE(11) + 0x8000;

    qtest_writeq(qts, old, 0xabcdef);
    result(qts, 0, MEMTX_OK);
    set_number(qts, 131);
    qtest_writeq(qts, old, 0xbad);
    result(qts, 0, MEMTX_DECODE_ERROR);
    set_number(qts, 3);
    g_assert_cmphex(qtest_readq(qts, old), ==, 0xabcdef);
    result(qts, 0, MEMTX_OK);
    set_number(qts, 11);
    qtest_writeq(qts, old, 0xbad);
    result(qts, 0, MEMTX_DECODE_ERROR);
    g_assert_cmphex(qtest_readq(qts, new), ==, 0xabcdef);
    result(qts, 1, MEMTX_OK);
    set_number(qts, 7);
    qtest_writel(qts, head(0, 0) + 0x2c, 0xbad);
    result(qts, 0, MEMTX_DECODE_ERROR);
    set_number(qts, 3);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'" PATH "/cable',"
        "'property':'down','value':true}}");
    qtest_writel(qts, head(0, 0) + 0x2c, 0xbad);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'" PATH "/cable',"
        "'property':'down','value':false}}");
    g_assert_cmphex(qtest_readl(qts, head(1, 0) + 4), ==, 0x2d002049);
    result(qts, 1, MEMTX_OK);
    qtest_quit(qts);
}

static void test_denied(void)
{
    QTestState *qts = start("");
    uint64_t sentinel = UINT64_C(0xabcdeffedcba0123);
    uint64_t address = UINT64_C(0xa080000000000000) + NODE(3) + 0x8000;

    configure_card(qts, 0);
    qtest_writeq(qts, RAM + 0x8000, sentinel);
    qtest_writeq(qts, COMPUTE + 0x400118, ~(UINT64_C(1) << 0xe));
    dma(qts, 0, address, sentinel ^ 1, true, MEMTX_ACCESS_ERROR);
    g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, sentinel);
    qtest_writeq(qts, COMPUTE + 0x400118, UINT64_MAX);
    qtest_writeq(qts, REQUESTER(0) + BIC_HUB + 0x400118,
                 ~(UINT64_C(1) << 0xe));
    result(qts, 0, MEMTX_OK);
    dma(qts, 0, address, sentinel ^ 2, true, MEMTX_ACCESS_ERROR);
    g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, sentinel);
    qtest_quit(qts);
}

static void test_translations(void)
{
    QTestState *qts = start("");

    for (unsigned i = 0; i < 6; i++) {
        uint64_t h = head(0, i);
        uint64_t payload = UINT64_C(0x1020304050607080) + i;

        configure_card(qts, i);
        qtest_writel(qts, h + 0x84, (0xa << 20) | 12);
        dma(qts, i, 0x80008000, payload, true, MEMTX_OK);
        g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, payload);
        dma(qts, i, 0x80008000, payload, false, MEMTX_OK);
        qtest_writeq(qts, h + 0x10000, NODE(3) | 0x8021 | (0xa << 8));
        dma(qts, i, 0x40000000, payload ^ 0xff, true, MEMTX_OK);
        g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, payload ^ 0xff);
        dma(qts, i, 0x40000000, payload ^ 0xff, false, MEMTX_OK);
        qtest_writeq(qts, h + 0x10000, 0);
        dma(qts, i, 0x40000000, payload, true, MEMTX_DECODE_ERROR);
        g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, payload ^ 0xff);
    }
    qtest_quit(qts);
}

static void configure_irq_pin(QTestState *qts, unsigned index, unsigned pin,
                              unsigned number)
{
    unsigned nasid = (qtest_readq(qts, COMPUTE + 0x600000) >> 32) & 0xff;
    uint64_t address = (UINT64_C(1) << 47) |
                       ((uint64_t)nasid << 39) | 0x1800090;
    uint64_t h = head(0, index);

    qtest_writel(qts, h + 0x24,
                 (qtest_readl(qts, h + 0x24) & ~15U) | widget[index]);
    qtest_writel(qts, h + 0x34, (0xa << 16) | (address >> 32));
    qtest_writel(qts, h + 0x3c, address);
    qtest_writel(qts, h + 0x134 + pin * 8,
                 ((address >> 38) << 8) | number);
    qtest_writel(qts, h + 0x10c,
                 qtest_readl(qts, h + 0x10c) | (1U << pin));
    qtest_writel(qts, h + 0x11c,
                 qtest_readl(qts, h + 0x11c) | (1U << pin));
}

static void configure_irq(QTestState *qts, unsigned index, unsigned number)
{
    configure_irq_pin(qts, index, 3, number);
}

static void pin(QTestState *qts, unsigned index, bool level)
{
    g_autofree char *path = g_strdup_printf(PATH "/chassis/u%u/head-%c",
        index / 2, index % 2 ? 'f' : 'e');

    qtest_set_irq_in(qts, path, NULL, 3, level);
}

static void test_interrupts(void)
{
    QTestState *qts = start("");

    for (unsigned i = 0; i < 6; i++) {
        unsigned level = 16 + i;

        configure_irq(qts, i, level);
        pin(qts, i, true);
        g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==,
                        UINT64_C(1) << level);
        pin(qts, i, false);
        g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, 0);
    }
    qtest_writeq(qts, COMPUTE + 0x400118, ~(UINT64_C(1) << 0xe));
    pin(qts, 0, true);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, 0);
    pin(qts, 0, false);
    qtest_quit(qts);

    qts = start("-global sgi-pci-expansion-ni-fixture.compute-device-mask="
                "0xfffeffffffffffff");
    configure_irq(qts, 0, 16);
    pin(qts, 0, true);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, 0);
    pin(qts, 0, false);
    configure_card(qts, 0);
    dma(qts, 0, UINT64_C(0xa080000000000000) + NODE(3) + 0x8000,
        0x12345678, true, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, 0x12345678);
    qtest_quit(qts);
}

static void test_interrupt_protection(void)
{
    const uint64_t source = UINT64_C(0x1000000000000000);
    const uint64_t status = UINT64_C(0x60000000);
    const uint64_t address = (UINT64_C(1) << 47) |
        (UINT64_C(3) << 39) | 0x1800090;
    QTestState *qts = start("-d guest_errors,unimp "
        "-device sgi-xio-test-target,id=packet,"
        "target-port=" PATH "/chassis/bic/io,source-widget=14");

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/packet", 1, source);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/packet", 2, status);
    qtest_writeq(qts, COMPUTE + 0x10, 0);
    qtest_writeq(qts, source + address, 0x110);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, 0);
    qtest_writeq(qts, COMPUTE + 0x10, UINT64_MAX);
    qtest_writeq(qts, source + address, 0x110);
    g_assert_cmphex(qtest_readq(qts, status), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, UINT64_C(1) << 16);
    qtest_writeq(qts, source + address, 0x10);
    g_assert_cmphex(qtest_readq(qts, status), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, 0);
    qtest_quit(qts);
}

static void test_unknown_sources(void)
{
    const char *extra = "-device sgi-xio-test-target,id=unknown,"
        "target-port=" PATH "/chassis/bic/io "
        "-device sgi-xio-test-target,id=head,"
        "target-port=" PATH "/chassis/bic/io,source-widget=14";
    uint64_t source = UINT64_C(0x1000000000000000);
    uint64_t status = UINT64_C(0x60000000);
    QTestState *qts = start(extra);
    uint64_t sentinel = UINT64_C(0xfedcba0123456789);

    qtest_writeq(qts, RAM + 0x8000, sentinel);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/unknown", 1, source);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/unknown", 2, status);
    qtest_writeq(qts, source + NODE(3) + 0x8000, sentinel ^ 1);
    g_assert_cmphex(qtest_readq(qts, status), ==, MEMTX_ACCESS_ERROR);
    g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, sentinel);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/head", 1,
                          source + (UINT64_C(1) << 53));
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/head", 2, status + 0x1000);
    qtest_writeq(qts, source + (UINT64_C(1) << 53) + NODE(3) + 0x8000,
                 sentinel ^ 2);
    g_assert_cmphex(qtest_readq(qts, status + 0x1000), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, sentinel ^ 2);
    qtest_quit(qts);

    qts = start("-global sgi-pci-expansion-ni-fixture.compute-device-mask="
        "0xfffdffffffffffff "
        "-device sgi-xio-test-target,id=head,"
        "target-port=" PATH "/chassis/bic/io,source-widget=14");
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/head", 1, source);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/head", 2, status);
    qtest_writeq(qts, RAM + 0x8000, sentinel);
    qtest_writeq(qts, source + NODE(3) + 0x8000, sentinel ^ 3);
    g_assert_cmphex(qtest_readq(qts, status), ==, MEMTX_ACCESS_ERROR);
    g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, sentinel);
    qtest_quit(qts);
}

static void test_disconnected(void)
{
    QTestState *qts = start(
        "-global sgi-pci-expansion-ni-fixture.connected=off");

    qtest_readl(qts, head(0, 0) + 4);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_writeq(qts, REQUESTER(1) + NODE(3) + 0x8000, 0x12345);
    result(qts, 1, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, 0x12345);
    qtest_quit(qts);
}

static void test_discovery(void)
{
    QTestState *qts = start("");
    uint64_t revision;

    set_number(qts, 7);
    qtest_readl(qts, head(0, 0) + 4);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_readq(qts, COMPUTE + 0x60c038);
    qtest_writeq(qts, COMPUTE + 0x60c008, 0);
    qtest_writeq(qts, COMPUTE + 0x60c000, 0x600000);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x60c020) &
                    ((UINT64_C(1) << 63) | 7), ==, UINT64_C(1) << 63);
    revision = qtest_readq(qts, COMPUTE + 0x60c030);
    qtest_readq(qts, COMPUTE + 0x60c038);
    g_assert_cmphex((revision >> 32) & 0xff, ==, 7);
    qtest_writeq(qts, COMPUTE + 0x60c008, 0);
    qtest_writeq(qts, COMPUTE + 0x60c010,
                 (revision & ~(UINT64_C(0xff) << 32)) | (UINT64_C(9) << 32));
    qtest_writeq(qts, COMPUTE + 0x60c000, 0x600001);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x60c020) & 7, ==, 1);
    g_assert_cmphex(qtest_readl(qts,
        REQUESTER(0) + IO + NODE(9) + (0xeULL << 24) + 4), ==, 0x2d002049);
    result(qts, 0, MEMTX_OK);
    qtest_quit(qts);
}

static void test_reset(void)
{
    QTestState *qts = start("");

    configure_irq(qts, 0, 16);
    pin(qts, 0, true);
    set_number(qts, 11);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex((qtest_readq(qts, COMPUTE + 0x600000) >> 32) & 0xff, ==, 3);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, 0);
    g_assert_cmphex(qtest_readl(qts, head(1, 0) + 4), ==, 0x2d002049);
    result(qts, 1, MEMTX_OK);
    qtest_quit(qts);
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("pci-expansion-ni-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = start("");
    QTestState *to = start("-incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_writeq(from, RAM + 0x8000, UINT64_C(0x123456789abcdef0));
    qtest_writel(from, head(0, 2) + 0x2c, 0x12345);
    configure_irq(from, 0, 16);
    pin(from, 0, true);
    set_number(from, 11);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, RAM + 0x8000), ==,
                    UINT64_C(0x123456789abcdef0));
    g_assert_cmphex(qtest_readq(to, REQUESTER(1) + NODE(11) + 0x8000), ==,
                    UINT64_C(0x123456789abcdef0));
    result(to, 1, MEMTX_OK);
    qtest_readq(to, REQUESTER(0) + NODE(3) + 0x8000);
    result(to, 0, MEMTX_DECODE_ERROR);
    g_assert_cmphex(qtest_readl(to, head(1, 2) + 0x2c), ==, 0x12345);
    g_assert_cmphex(qtest_readq(to, COMPUTE + 0x98), ==, UINT64_C(1) << 16);
    configure_irq(to, 0, 16);
    pin(to, 0, false);
    g_assert_cmphex(qtest_readq(to, COMPUTE + 0x98), ==, 0);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_rmdir(dir);
}

static void configure_intx_card(QTestState *qts, unsigned index,
                                unsigned device, unsigned pin)
{
    uint64_t h = head(0, index);
    uint64_t config = h + 0x20000 + device * 0x1000 + pin * 0x100;

    qtest_writel(qts, h + 0x24,
                 (qtest_readl(qts, h + 0x24) & ~15U) | widget[index]);
    qtest_writel(qts, config + 0x10,
                 0x01000000 + device * 0x200000 + pin * 0x1000);
    qtest_writel(qts, config + 4, 6);
    qtest_writel(qts, h + 0x204 + device * 8,
                 0x81000 | (0x10 + device * 2));
}

static uint64_t intx_card(unsigned index, unsigned device, unsigned pin)
{
    static const unsigned window[] = { 0, 0x400000, 0x600000, 0x700000 };

    return head(0, index) + window[device] + pin * 0x1000;
}

static void card_pin(QTestState *qts, unsigned index, unsigned device,
                     unsigned pin, bool level)
{
    uint64_t bar = intx_card(index, device, pin);

    qtest_writel(qts, bar + 32, level);
    g_assert_cmphex(qtest_readl(qts, bar + 32), ==, level);
}

static void intx_state(QTestState *qts, unsigned index, uint8_t inputs,
                       uint64_t pending)
{
    g_assert_cmphex(qtest_readl(qts, head(0, index) + 0x104) & 0xff, ==,
                    inputs);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, pending);
}

static void test_intx(void)
{
    static const unsigned inputs[2][4] = { { 1, 5, 1, 5 }, { 2, 6, 2, 6 } };
    QTestState *qts = start("-global sgi-pci-expansion-ni-fixture.intx-map=1");

    set_number(qts, 11);
    for (unsigned index = 0; index < 6; index++) {
        for (unsigned device = 1; device <= 2; device++) {
            for (unsigned pin = 0; pin < 4; pin++) {
                unsigned input = inputs[device - 1][pin];
                uint64_t count = qtest_readq(qts, STATUS + 16);

                configure_intx_card(qts, index, device, pin);
                configure_irq_pin(qts, index, input, 16);
                card_pin(qts, index, device, pin, true);
                intx_state(qts, index, 1U << input, UINT64_C(1) << 16);
                g_assert_cmphex(qtest_readq(qts, STATUS + 16), ==, count + 1);
                g_assert_cmphex(qtest_readq(qts, STATUS + 24), ==,
                    UINT64_C(0x858001800090));
                g_assert_cmphex(qtest_readq(qts, STATUS + 32), ==, 0x110);
                g_assert_cmphex(qtest_readq(qts, STATUS + 40), ==,
                                0x100 | widget[index]);
                for (unsigned other = 0; other < 6; other++) {
                    if (other != index) {
                        g_assert_cmphex(qtest_readl(qts,
                            head(0, other) + 0x104) & 0xff, ==, 0);
                    }
                }
                card_pin(qts, index, device, pin, false);
                intx_state(qts, index, 0, 0);
                g_assert_cmphex(qtest_readq(qts, STATUS + 16), ==, count + 2);
                g_assert_cmphex(qtest_readq(qts, STATUS + 32), ==, 0x10);
            }
        }
    }
    qtest_quit(qts);
}

static void test_intx_or(void)
{
    QTestState *qts = start("-global sgi-pci-expansion-ni-fixture.intx-map=1");

    for (unsigned device = 1; device <= 2; device++) {
        for (unsigned pair = 0; pair < 2; pair++) {
            unsigned input = device + pair * 4;
            uint64_t count = qtest_readq(qts, STATUS + 16);

            configure_intx_card(qts, 0, device, pair);
            configure_intx_card(qts, 0, device, pair + 2);
            configure_irq_pin(qts, 0, input, 16);
            card_pin(qts, 0, device, pair, true);
            card_pin(qts, 0, device, pair + 2, true);
            intx_state(qts, 0, 1U << input, UINT64_C(1) << 16);
            g_assert_cmphex(qtest_readq(qts, STATUS + 16), ==, count + 1);
            card_pin(qts, 0, device, pair, false);
            intx_state(qts, 0, 1U << input, UINT64_C(1) << 16);
            g_assert_cmphex(qtest_readq(qts, STATUS + 16), ==, count + 1);
            card_pin(qts, 0, device, pair + 2, false);
            intx_state(qts, 0, 0, 0);
            g_assert_cmphex(qtest_readq(qts, STATUS + 16), ==, count + 2);
        }
    }
    /* Both device indices stay independently asserted on one head. */
    configure_irq_pin(qts, 0, 1, 16);
    configure_irq_pin(qts, 0, 2, 17);
    card_pin(qts, 0, 1, 0, true);
    card_pin(qts, 0, 2, 0, true);
    intx_state(qts, 0, 6, UINT64_C(3) << 16);
    card_pin(qts, 0, 1, 0, false);
    intx_state(qts, 0, 4, UINT64_C(1) << 17);
    card_pin(qts, 0, 2, 0, false);
    intx_state(qts, 0, 0, 0);
    qtest_quit(qts);
}

static void test_intx_masked(void)
{
    QTestState *qts = start("-global sgi-pci-expansion-ni-fixture.intx-map=1");
    uint64_t h = head(0, 0);

    configure_intx_card(qts, 0, 1, 0);
    configure_irq_pin(qts, 0, 1, 16);
    qtest_writel(qts, h + 0x10c, 0);
    card_pin(qts, 0, 1, 0, true);
    intx_state(qts, 0, 2, 0);
    g_assert_cmphex(qtest_readq(qts, STATUS + 16), ==, 0);
    qtest_writel(qts, h + 0x10c, 2);
    intx_state(qts, 0, 2, 0);
    g_assert_cmphex(qtest_readq(qts, STATUS + 16), ==, 0);
    /* Unmasking alone emits no packet; the existing head force does. */
    qtest_writel(qts, h + 0x1cc, 1);
    intx_state(qts, 0, 2, UINT64_C(1) << 16);
    card_pin(qts, 0, 1, 0, false);
    intx_state(qts, 0, 0, 0);
    qtest_writel(qts, h + 0x11c, 0);
    card_pin(qts, 0, 1, 0, true);
    card_pin(qts, 0, 1, 0, false);
    intx_state(qts, 0, 0, UINT64_C(1) << 16);
    qtest_writeq(qts, COMPUTE + 0x90, 16);
    intx_state(qts, 0, 0, 0);
    qtest_quit(qts);
}

static void test_intx_denied(void)
{
    QTestState *qts = start("-global sgi-pci-expansion-ni-fixture.intx-map=1 "
        "-global sgi-pci-expansion-ni-fixture.compute-device-mask="
        "0xfffeffffffffffff");

    configure_intx_card(qts, 0, 1, 0);
    configure_irq_pin(qts, 0, 1, 16);
    card_pin(qts, 0, 1, 0, true);
    intx_state(qts, 0, 2, 0);
    g_assert_cmphex(qtest_readq(qts, STATUS + 16), ==, 0);
    card_pin(qts, 0, 1, 0, false);
    /* Device-1 DMA remains permitted despite unknown-device IRQ denial. */
    configure_card(qts, 0);
    dma(qts, 0, UINT64_C(0xa080000000000000) + NODE(3) + 0x8000,
        0x12345678, true, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, 0x12345678);
    qtest_quit(qts);
}

static void test_intx_alternative(void)
{
    QTestState *qts = start("-global sgi-pci-expansion-ni-fixture.intx-map=2");

    configure_intx_card(qts, 0, 1, 0);
    configure_irq_pin(qts, 0, 1, 16);
    configure_irq_pin(qts, 0, 3, 17);
    card_pin(qts, 0, 1, 0, true);
    intx_state(qts, 0, 8, UINT64_C(1) << 17);
    card_pin(qts, 0, 1, 0, false);
    intx_state(qts, 0, 0, 0);
    qtest_quit(qts);
}

static void test_intx_reset(void)
{
    QTestState *qts = start("-global sgi-pci-expansion-ni-fixture.intx-map=1");

    configure_intx_card(qts, 0, 1, 0);
    configure_intx_card(qts, 0, 1, 2);
    configure_irq_pin(qts, 0, 1, 16);
    card_pin(qts, 0, 1, 0, true);
    card_pin(qts, 0, 1, 2, true);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    intx_state(qts, 0, 0, 0);
    configure_intx_card(qts, 0, 1, 0);
    configure_intx_card(qts, 0, 1, 2);
    configure_irq_pin(qts, 0, 1, 16);
    g_assert_cmphex(qtest_readl(qts, intx_card(0, 1, 0) + 32), ==, 0);
    g_assert_cmphex(qtest_readl(qts, intx_card(0, 1, 2) + 32), ==, 0);
    card_pin(qts, 0, 1, 0, true);
    intx_state(qts, 0, 2, UINT64_C(1) << 16);
    card_pin(qts, 0, 1, 0, false);
    intx_state(qts, 0, 0, 0);
    qtest_quit(qts);
}

static void test_intx_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("pci-expansion-intx-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = start("-global sgi-pci-expansion-ni-fixture.intx-map=1");
    QTestState *to = start("-global sgi-pci-expansion-ni-fixture.intx-map=1 "
                          "-incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    configure_intx_card(from, 0, 1, 0);
    configure_intx_card(from, 0, 1, 2);
    configure_irq_pin(from, 0, 1, 16);
    card_pin(from, 0, 1, 0, true);
    card_pin(from, 0, 1, 2, true);
    g_assert_cmphex(qtest_readq(from, STATUS + 16), ==, 1);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    intx_state(to, 0, 2, UINT64_C(1) << 16);
    /* Destination-only observation catches even idempotent repeated packets. */
    g_assert_cmphex(qtest_readq(to, STATUS + 16), ==, 0);
    card_pin(to, 0, 1, 0, false);
    intx_state(to, 0, 2, UINT64_C(1) << 16);
    g_assert_cmphex(qtest_readq(to, STATUS + 16), ==, 0);
    card_pin(to, 0, 1, 2, false);
    intx_state(to, 0, 0, 0);
    g_assert_cmphex(qtest_readq(to, STATUS + 16), ==, 1);
    card_pin(to, 0, 1, 0, true);
    intx_state(to, 0, 2, UINT64_C(1) << 16);
    g_assert_cmphex(qtest_readq(to, STATUS + 16), ==, 2);
    card_pin(to, 0, 1, 0, false);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_rmdir(dir);
}

static void test_intx_unconnected(void)
{
    QTestState *qts = start("-d unimp");

    configure_card(qts, 0);
    configure_irq_pin(qts, 0, 1, 16);
    card_pin(qts, 0, 1, 0, true);
    intx_state(qts, 0, 0, 0);
    g_assert_cmphex(qtest_readq(qts, STATUS + 16), ==, 0);
    card_pin(qts, 0, 1, 0, false);
    qtest_quit(qts);
}

static void test_intx_unsupported(void)
{
    QTestState *qts = start("-d unimp "
        "-global sgi-pci-expansion-ni-fixture.intx-map=3");

    configure_intx_card(qts, 0, 3, 0);
    configure_irq_pin(qts, 0, 3, 16);
    card_pin(qts, 0, 3, 0, true);
    intx_state(qts, 0, 0, 0);
    g_assert_cmphex(qtest_readq(qts, STATUS + 16), ==, 0);
    card_pin(qts, 0, 3, 0, false);
    /* A supported card still drives its independent board input. */
    configure_intx_card(qts, 0, 1, 0);
    configure_irq_pin(qts, 0, 1, 16);
    card_pin(qts, 0, 1, 0, true);
    intx_state(qts, 0, 2, UINT64_C(1) << 16);
    card_pin(qts, 0, 1, 0, false);
    qtest_quit(qts);
}

static void test_intx_migration_mismatch(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("pci-intx-map-mismatch-XXXXXX",
                                        &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = start("-global sgi-pci-expansion-ni-fixture.intx-map=1");
    QTestState *to = start("-global sgi-pci-expansion-ni-fixture.intx-map=2 "
                          "-incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_set_expected_status(to, EXIT_FAILURE);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    qtest_wait_qemu(to);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket);
    g_rmdir(dir);
}

#define MANAGEMENT UINT64_C(0x31002000)
#define L1_UART(n) (UINT64_C(0x31000000) + (n) * 0x1000)

static QTestState *management_start(const char *extra)
{
    g_autofree char *options = g_strdup_printf(
        "-global sgi-pci-expansion-ni-fixture.management=on %s", extra);
    QTestState *qts = start(options);

    qtest_sysbus_mmio_map(qts, PATH, 5, L1_UART(0));
    qtest_sysbus_mmio_map(qts, PATH, 6, L1_UART(1));
    qtest_sysbus_mmio_map(qts, PATH, 7, MANAGEMENT);
    return qts;
}

static void management_query(QTestState *qts, unsigned issuer,
                             uint32_t address, uint8_t channel)
{
    uint8_t request[] = { channel, address >> 24, address >> 16,
                          address >> 8, address, 0, 3, 0 };

    sgi_l1_test_send(qts, L1_UART(issuer), request, sizeof(request), false);
}

static void management_identity(QTestState *qts, unsigned issuer,
                                uint8_t channel, unsigned serving)
{
    uint8_t reply[131];

    g_assert_cmpuint(sgi_l1_test_receive(qts, L1_UART(issuer), reply,
                                        sizeof(reply)), ==, 16);
    g_assert_cmphex(reply[0], ==, 0x20 | channel);
    g_assert_cmphex(reply[5], ==, 2);
    g_assert_cmphex(reply[10], ==, 0x41 + serving);
    g_assert_cmphex(reply[15], ==, serving ? 0x25 : 0x34);

}

static void management_empty(QTestState *qts, unsigned issuer)
{
    g_assert_cmphex(qtest_readq(qts, L1_UART(issuer) + 0x28), ==, 0x60);
}

static uint32_t management_address(unsigned owner)
{
    return (1u << 18) | ((owner + 1) << 12) | 6;
}

static void management_refusal(QTestState *qts, unsigned issuer,
                               uint8_t channel)
{
    uint8_t reply[131];

    g_assert_cmpuint(sgi_l1_test_receive(qts, L1_UART(issuer), reply,
                                        sizeof(reply)), ==, 9);
    g_assert_cmphex(reply[0], ==, 0x20 | channel);
    g_assert_cmphex((uint32_t)ldl_be_p(reply + 1), ==, UINT32_MAX);
    g_assert_cmphex(ldl_be_p(reply + 5), ==, 8);
}

static void management_cable(QTestState *qts, bool down)
{
    if (down) {
        qtest_qmp_assert_success(qts,
            "{'execute':'qom-set','arguments':{'path':'" PATH "/cable',"
            "'property':'down','value':true}}");
    } else {
        qtest_qmp_assert_success(qts,
            "{'execute':'qom-set','arguments':{'path':'" PATH "/cable',"
            "'property':'down','value':false}}");
    }
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x680000) & 2, ==,
                    down ? 0 : 2);
}

static void test_management_isolation(void)
{
    QTestState *qts = management_start("");

    qtest_writel(qts, head(0, 0) + 0x2c, 0x12345);
    result(qts, 0, MEMTX_OK);
    g_assert_cmphex(qtest_readl(qts, head(0, 0) + 0x2c), ==, 0x12345);
    management_cable(qts, true);
    qtest_writel(qts, head(0, 0) + 0x2c, 0xbad);
    result(qts, 0, MEMTX_DECODE_ERROR);
    management_query(qts, 0, 0x4ffff006, 19);
    management_identity(qts, 0, 19, 1);
    management_empty(qts, 1);
    management_query(qts, 1, management_address(0), 7);
    management_identity(qts, 1, 7, 0);
    management_empty(qts, 0);
    qtest_writeq(qts, MANAGEMENT, 0);
    management_query(qts, 0, 0x4ffff006, 13);
    management_refusal(qts, 0, 13);
    management_query(qts, 1, management_address(0), 11);
    management_refusal(qts, 1, 11);
    qtest_writeq(qts, MANAGEMENT, 3);
    management_query(qts, 0, management_address(1), 23);
    management_identity(qts, 0, 23, 1);
    qtest_readl(qts, head(0, 0) + 0x2c);
    result(qts, 0, MEMTX_DECODE_ERROR);
    management_cable(qts, false);
    g_assert_cmphex(qtest_readl(qts, head(0, 0) + 0x2c), ==, 0x12345);
    result(qts, 0, MEMTX_OK);
    qtest_quit(qts);
}

static void test_management_reset(void)
{
    QTestState *qts = management_start("");

    qtest_writeq(qts, MANAGEMENT, 0);
    qtest_writeq(qts, MANAGEMENT + 8, 1);
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT), ==, 0);
    management_query(qts, 0, 0x4ffff006, 17);
    management_refusal(qts, 0, 17);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT), ==, 3);
    management_query(qts, 0, 0x4ffff006, 5);
    management_identity(qts, 0, 5, 1);
    qtest_quit(qts);
}

static void test_management_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("ni-l1-migration-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = management_start("");
    QTestState *to = management_start("-incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    management_query(from, 0, 0x4ffff006, 19);
    management_query(from, 1, management_address(0), 7);
    qtest_writeq(from, MANAGEMENT, 0);
    management_cable(from, true);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    management_identity(to, 0, 19, 1);
    management_identity(to, 1, 7, 0);
    management_empty(to, 0);
    management_empty(to, 1);
    g_assert_cmphex(qtest_readq(to, MANAGEMENT), ==, 0);
    g_assert_cmphex(qtest_readq(to, COMPUTE + 0x680000) & 2, ==, 0);
    qtest_readl(to, head(0, 0) + 4);
    result(to, 0, MEMTX_DECODE_ERROR);
    management_query(to, 0, 0x4ffff006, 3);
    management_refusal(to, 0, 3);
    qtest_writeq(to, MANAGEMENT, 3);
    management_query(to, 0, 0x4ffff006, 29);
    management_identity(to, 0, 29, 1);
    management_cable(to, false);
    g_assert_cmphex(qtest_readl(to, head(0, 0) + 4), ==, 0x2d002049);
    result(to, 0, MEMTX_OK);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_rmdir(dir);
}

static void management_mismatch(const char *extra)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("ni-l1-mismatch-XXXXXX", &error);
    g_autofree char *options = g_strdup_printf("-incoming defer %s", extra);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = management_start("");
    QTestState *to = management_start(options);

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_set_expected_status(to, EXIT_FAILURE);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    qtest_wait_qemu(to);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_rmdir(dir);
}

static void test_management_owner_mismatch(void)
{
    management_mismatch("-global sgi-pci-expansion-ni-fixture.identity1=103");
}

static void test_management_attachment_mismatch(void)
{
    management_mismatch(
        "-global sgi-pci-expansion-ni-fixture.attached-issuer=1 "
        "-global sgi-pci-expansion-ni-fixture.attached-owner=0");
}

static void test_management_records(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("ni-l1-records-XXXXXX", &error);
    g_autofree char *options = NULL;
    char *paths[2];
    uint8_t images[2][64] = { { 0 } };
    uint8_t request[28] = { 23, 0x4f, 0xff, 0xf0, 6, 0, 6, 4 };
    uint8_t reply[131];
    QTestState *qts;

    g_assert_no_error(error);
    for (unsigned i = 0; i < 2; i++) {
        paths[i] = g_strdup_printf("%s/owner%u.bin", dir, i);
        images[i][0] = 1;
        images[i][1] = 2;
        images[i][7] = 0xfd;
        images[i][18] = 4;
        images[i][20] = i + 1;
        images[i][31] = -(4 + i + 1);
        g_assert_true(g_file_set_contents(paths[i], (char *)images[i], 64,
                                          &error));
        g_assert_no_error(error);
    }
    options = g_strdup_printf(
        "-global sgi-pci-expansion-ni-fixture.record0=%s "
        "-global sgi-pci-expansion-ni-fixture.record1=%s", paths[0], paths[1]);
    qts = management_start(options);
    /* Four typed integers: record 0, area 3, offset 0, length 16. */
    for (unsigned i = 0; i < 4; i++) {
        request[8 + i * 5] = 0;
        stl_be_p(request + 9 + i * 5, i == 1 ? 3 : i == 3 ? 16 : 0);
    }
    for (unsigned issuer = 0; issuer < 2; issuer++) {
        unsigned serving = 1 - issuer;

        request[0] = issuer ? 11 : 23;
        stl_be_p(request + 1, issuer ? management_address(0) : 0x4ffff006);
        sgi_l1_test_send(qts, L1_UART(issuer), request, sizeof(request), false);
        g_assert_cmpuint(sgi_l1_test_receive(qts, L1_UART(issuer), reply,
                                            sizeof(reply)), ==, 28);
        g_assert_cmphex(reply[0], ==, 0x20 | request[0]);
        g_assert_cmpmem(reply + 12, 16, images[serving] + 16, 16);
        management_empty(qts, serving);
    }
    qtest_quit(qts);
    for (unsigned i = 0; i < 2; i++) {
        unlink(paths[i]);
        g_free(paths[i]);
    }
    g_rmdir(dir);
}

static void test_management_data(void)
{
    QTestState *qts = management_start("");

    g_assert_cmphex(qtest_readl(qts, head(0, 0) + 4), ==, 0x2d002049);
    result(qts, 0, MEMTX_OK);
    management_query(qts, 0, 0x4ffff006, 19);
    management_identity(qts, 0, 19, 1);
    management_empty(qts, 1);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/pci-expansion-ni/management-isolation",
                   test_management_isolation);
    qtest_add_func("/sgi/pci-expansion-ni/management-reset",
                   test_management_reset);
    qtest_add_func("/sgi/pci-expansion-ni/management-records",
                   test_management_records);
    qtest_add_func("/sgi/pci-expansion-ni/management-migration",
                   test_management_migration);
    qtest_add_func("/sgi/pci-expansion-ni/management-owner-mismatch",
                   test_management_owner_mismatch);
    qtest_add_func("/sgi/pci-expansion-ni/management-attachment-mismatch",
                   test_management_attachment_mismatch);
    qtest_add_func("/sgi/pci-expansion-ni/management-data",
                   test_management_data);
    qtest_add_func("/sgi/pci-expansion-ni/intx", test_intx);
    qtest_add_func("/sgi/pci-expansion-ni/intx-or", test_intx_or);
    qtest_add_func("/sgi/pci-expansion-ni/intx-masked", test_intx_masked);
    qtest_add_func("/sgi/pci-expansion-ni/intx-denied", test_intx_denied);
    qtest_add_func("/sgi/pci-expansion-ni/intx-alternative",
                   test_intx_alternative);
    qtest_add_func("/sgi/pci-expansion-ni/intx-reset", test_intx_reset);
    qtest_add_func("/sgi/pci-expansion-ni/intx-migration", test_intx_migration);
    qtest_add_func("/sgi/pci-expansion-ni/intx-unsupported",
                   test_intx_unsupported);
    qtest_add_func("/sgi/pci-expansion-ni/intx-unconnected",
                   test_intx_unconnected);
    qtest_add_func("/sgi/pci-expansion-ni/intx-migration-mismatch",
                   test_intx_migration_mismatch);
    qtest_add_func("/sgi/pci-expansion-ni/pio", test_pio);
    qtest_add_func("/sgi/pci-expansion-ni/dma", test_dma);
    qtest_add_func("/sgi/pci-expansion-ni/live-number", test_live_number);
    qtest_add_func("/sgi/pci-expansion-ni/denied", test_denied);
    qtest_add_func("/sgi/pci-expansion-ni/translations", test_translations);
    qtest_add_func("/sgi/pci-expansion-ni/interrupts", test_interrupts);
    qtest_add_func("/sgi/pci-expansion-ni/interrupt-protection",
                   test_interrupt_protection);
    qtest_add_func("/sgi/pci-expansion-ni/unknown-sources",
                   test_unknown_sources);
    qtest_add_func("/sgi/pci-expansion-ni/disconnected", test_disconnected);
    qtest_add_func("/sgi/pci-expansion-ni/discovery", test_discovery);
    qtest_add_func("/sgi/pci-expansion-ni/reset", test_reset);
    qtest_add_func("/sgi/pci-expansion-ni/migration", test_migration);
    return g_test_run();
}
