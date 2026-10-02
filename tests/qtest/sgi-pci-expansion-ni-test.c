/* SPDX-License-Identifier: BSD-3-Clause */
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

static unsigned card_device = 1;
static unsigned isp_head = 4;

static QTestState *management_start(const char *extra);
static void power_set(QTestState *qts, bool on);

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
    uint64_t config = h + 0x20000 + card_device * 0x1000;

    qtest_writel(qts, h + 0x24,
                 (qtest_readl(qts, h + 0x24) & ~15U) | widget[index]);
    qtest_writel(qts, config + 0x10, 0x01200000);
    qtest_writel(qts, config + 4, 6);
    qtest_writel(qts, h + 0x204 + card_device * 8, 0x81012);
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

static uint64_t card_bar(unsigned index)
{
    return head(0, index) + (card_device == 1 ? 0x400000 : 0x600000);
}

static void dma(QTestState *qts, unsigned card, uint64_t address,
                uint64_t payload, bool write, MemTxResult expected)
{
    uint64_t bar = card_bar(card);

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

#define ISP_RAM_WORD 0x3000
#define ISP_MAILBOX(n) (0x70 + (n) * 2)
#define ISP_COMPLETE 0x4000
#define ISP_INTERFACE_ERROR 0x4002

static QTestState *isp_start(const char *extra)
{
    g_autofree char *options = g_strdup_printf(
        "-global sgi-pci-expansion-ni-fixture.intx-map=1 "
        "-global sgi-pci-expansion-ni-fixture.isp-head=%u "
        "-global sgi-pci-expansion-ni-fixture.card-device=%u %s",
        isp_head, card_device, extra);
    QTestState *qts = strstr(extra, "power=on") ?
                      management_start(options) : start(options);
    uint64_t h = head(0, isp_head);
    uint64_t config = h + 0x20000 + card_device * 0x1000;

    g_assert_cmphex(qtest_readl(qts, config), ==, 0x10201077);
    qtest_writel(qts, h + 0x24,
                 (qtest_readl(qts, h + 0x24) & ~15U) | widget[isp_head]);
    qtest_writel(qts, config + 0x14, 0x01200000);
    qtest_writel(qts, config + 4, 6);
    qtest_writel(qts, h + 0x204 + card_device * 8, 0x1012);
    return qts;
}

static void isp_writew(QTestState *qts, unsigned offset, uint16_t value)
{
    qtest_writew(qts, card_bar(isp_head) + (offset ^ 2), value);
    result(qts, 0, MEMTX_OK);
}

static uint16_t isp_readw(QTestState *qts, unsigned offset)
{
    uint16_t value = qtest_readw(qts,
        card_bar(isp_head) + (offset ^ 2));

    result(qts, 0, MEMTX_OK);
    return value;
}

static void isp_ack(QTestState *qts)
{
    isp_writew(qts, 0xc0, 7 << 12);
    isp_writew(qts, 0x0c, 0);
}

static void isp_word(QTestState *qts, uint16_t address, uint16_t value)
{
    isp_writew(qts, ISP_MAILBOX(0), 4);
    isp_writew(qts, ISP_MAILBOX(1), address);
    isp_writew(qts, ISP_MAILBOX(2), value);
    isp_writew(qts, 0xc0, 5 << 12);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_COMPLETE);
}

static void isp_block(QTestState *qts, uint16_t command, uint16_t address,
                      uint32_t pci_address, uint16_t count, uint16_t expected)
{
    isp_writew(qts, ISP_MAILBOX(0), command);
    isp_writew(qts, ISP_MAILBOX(1), address);
    isp_writew(qts, ISP_MAILBOX(2), pci_address >> 16);
    isp_writew(qts, ISP_MAILBOX(3), pci_address);
    isp_writew(qts, ISP_MAILBOX(4), count);
    isp_writew(qts, ISP_MAILBOX(6), 0);
    isp_writew(qts, ISP_MAILBOX(7), 0);
    isp_writew(qts, 0xc0, 5 << 12);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, expected);
    isp_ack(qts);
}

static void test_isp_dma(void)
{
    QTestState *qts = isp_start("");
    uint64_t h = head(0, isp_head);
    const uint8_t output[2][4] = {
        { 0xab, 0xcd, 0x12, 0x34 }, { 0x34, 0x12, 0xcd, 0xab },
    };
    const uint8_t input[2][4] = {
        { 0x90, 0xef, 0x56, 0x78 }, { 0x78, 0x56, 0xef, 0x90 },
    };
    uint8_t bytes[4];

    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(1)), ==, 0x4953);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==, 0x5020);
    for (unsigned swap = 0; swap < 2; swap++) {
        uint64_t flags = (0xa << 8) | (swap << 5) | 1;

        qtest_writeq(qts, h + 0x10000, NODE(3) | 0x8000 | flags);
        qtest_writeq(qts, h + 0x10008, NODE(3) | 0x9000 | flags);
        isp_word(qts, ISP_RAM_WORD, 0x1234);
        isp_ack(qts);
        isp_word(qts, ISP_RAM_WORD + 1, 0xabcd);
        isp_ack(qts);
        isp_block(qts, 3, ISP_RAM_WORD, 0x40000000, 2, ISP_COMPLETE);
        qtest_memread(qts, RAM + 0x8000, bytes, sizeof(bytes));
        g_assert_cmpmem(bytes, sizeof(bytes), output[swap], sizeof(bytes));
        qtest_memwrite(qts, RAM + 0x9000, input[swap], sizeof(bytes));
        isp_block(qts, 1, ISP_RAM_WORD, 0x40001000, 2, ISP_COMPLETE);
        /* A qualified plain dump prevents matching swaps hiding load errors. */
        qtest_writeq(qts, h + 0x10000, NODE(3) | 0x8021 | (0xa << 8));
        isp_block(qts, 3, ISP_RAM_WORD, 0x40000004, 2, ISP_COMPLETE);
        qtest_memread(qts, RAM + 0x8004, bytes, sizeof(bytes));
        g_assert_cmpmem(bytes, sizeof(bytes), input[1], sizeof(bytes));
    }
    for (unsigned other = 0; other < 6; other++) {
        if (other != isp_head) {
            g_assert_cmphex(qtest_readq(qts, head(0, other) + 0x10000), ==, 0);
        }
    }
    qtest_quit(qts);
}

static void isp_interrupt(bool denied);

static void test_adapter_placement(const void *opaque)
{
    unsigned placement = GPOINTER_TO_UINT(opaque);

    isp_head = placement / 2;
    card_device = placement % 2 + 1;
    test_isp_dma();
    isp_interrupt(false);
    isp_head = 4;
    card_device = 1;
}

static void test_dma_device2(void)
{
    /* Device numbers are synthetic positions, not physical slot labels. */
    QTestState *qts;

    card_device = 2;
    qts = start(
        "-global sgi-pci-expansion-ni-fixture.card-device=2");

    for (unsigned i = 0; i < 6; i++) {
        uint64_t payload = UINT64_C(0xdeadbeef01235000) + i;
        uint64_t address = UINT64_C(0xa080000000000000) + NODE(3) + 0x8000;

        configure_card(qts, i);
        dma(qts, i, address, payload, true, MEMTX_OK);
        g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, payload);
        dma(qts, i, address, payload, false, MEMTX_OK);
    }
    qtest_quit(qts);
    card_device = 1;
}

static void test_isp_dma_denied(void)
{
    QTestState *qts = isp_start("");
    uint64_t h = head(0, isp_head);
    const uint8_t sentinel[] = { 0xa1, 0xb2, 0xc3, 0xd4 };
    const uint8_t output[] = { 0x57, 0x13 };
    uint8_t bytes[4];

    isp_word(qts, ISP_RAM_WORD, 0x1357);
    isp_ack(qts);
    qtest_memwrite(qts, RAM + 0x8000, sentinel, sizeof(sentinel));
    /* An invalid ATE refuses the transfer rather than using global memory. */
    isp_block(qts, 3, ISP_RAM_WORD, 0x40000000, 1, ISP_INTERFACE_ERROR);
    qtest_memread(qts, RAM + 0x8000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), sentinel, sizeof(sentinel));
    /* A valid translation to the RAM-less BIC also refuses the transfer. */
    qtest_writeq(qts, h + 0x10000, NODE(7) | 0x8021 | (0xa << 8));
    isp_block(qts, 3, ISP_RAM_WORD, 0x40000000, 1, ISP_INTERFACE_ERROR);
    qtest_memread(qts, RAM + 0x8000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), sentinel, sizeof(sentinel));
    qtest_writeq(qts, h + 0x10000, NODE(3) | 0x8021 | (0xa << 8));
    qtest_writeq(qts, COMPUTE + 0x400118,
                 ~(UINT64_C(1) << widget[isp_head]));
    isp_block(qts, 3, ISP_RAM_WORD, 0x40000000, 1, ISP_INTERFACE_ERROR);
    qtest_memread(qts, RAM + 0x8000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), sentinel, sizeof(sentinel));
    qtest_writeq(qts, COMPUTE + 0x400118, UINT64_MAX);
    isp_block(qts, 3, ISP_RAM_WORD, 0x40000000, 1, ISP_COMPLETE);
    qtest_memread(qts, RAM + 0x8000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, 2, output, sizeof(output));
    g_assert_cmpmem(bytes + 2, 2, sentinel + 2, 2);
    qtest_quit(qts);
}

static void isp_interrupt(bool denied)
{
    QTestState *qts = isp_start(denied ?
        "-global sgi-pci-expansion-ni-fixture.compute-device-mask="
        "0xfffffffeffffffff" : "");
    uint64_t pending = denied ? 0 : UINT64_C(1) << 16;
    uint64_t count = denied ? 0 : 1;

    set_number(qts, 11);
    configure_irq_pin(qts, isp_head, card_device, 16);
    isp_writew(qts, 0x08, 6);
    isp_word(qts, ISP_RAM_WORD, 0x1357);
    g_assert_cmphex(isp_readw(qts, 0x0a) & 4, ==, 4);
    g_assert_cmphex(isp_readw(qts, 0x0c) & 1, ==, 1);
    intx_state(qts, isp_head, 1U << card_device, pending);
    g_assert_cmphex(qtest_readq(qts, STATUS + 16), ==, count);
    if (!denied) {
        g_assert_cmphex(qtest_readq(qts, STATUS + 24), ==,
                        UINT64_C(0x858001800090));
        g_assert_cmphex(qtest_readq(qts, STATUS + 32), ==, 0x110);
        g_assert_cmphex(qtest_readq(qts, STATUS + 40), ==,
                        0x100 | widget[isp_head]);
    }
    for (unsigned other = 0; other < 6; other++) {
        if (other != isp_head) {
            g_assert_cmphex(qtest_readl(qts, head(0, other) + 0x104) & 0xff,
                            ==, 0);
        }
    }
    isp_ack(qts);
    g_assert_cmphex(isp_readw(qts, 0x0a) & 4, ==, 0);
    g_assert_cmphex(isp_readw(qts, 0x0c), ==, 0);
    intx_state(qts, isp_head, 0, 0);
    g_assert_cmphex(qtest_readq(qts, STATUS + 16), ==, count * 2);
    if (denied) {
        const uint8_t expected[] = { 0x57, 0x13 };
        uint8_t bytes[2];

        /* Known device-1 DMA passes; its unknown-device IRQ remains denied. */
        qtest_writeq(qts, head(0, isp_head) + 0x10000,
                     NODE(11) | 0x8021 | (0xa << 8));
        isp_block(qts, 3, ISP_RAM_WORD, 0x40000000, 1, ISP_COMPLETE);
        qtest_memread(qts, RAM + 0x8000, bytes, sizeof(bytes));
        g_assert_cmpmem(bytes, sizeof(bytes), expected, sizeof(expected));
        intx_state(qts, isp_head, 0, 0);
        g_assert_cmphex(qtest_readq(qts, STATUS + 16), ==, 0);
    } else {
        g_assert_cmphex(qtest_readq(qts, STATUS + 32), ==, 0x10);
    }
    for (unsigned other = 0; other < 6; other++) {
        if (other != isp_head) {
            g_assert_cmphex(qtest_readl(qts, head(0, other) + 0x104) & 0xff,
                            ==, 0);
        }
    }
    qtest_quit(qts);
}

static void test_isp_interrupt(void)
{
    isp_interrupt(false);
}

static void test_isp_interrupt_denied(void)
{
    isp_interrupt(true);
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

#define STORAGE_BASE (REQUESTER(0) + NODE(7) + UINT64_C(0x38000000))
#define STORAGE_SECOND UINT64_C(0x02000000)
#define STORAGE_UNLOCK0 (8 * UINT64_C(0x555))
#define STORAGE_UNLOCK1 (8 * UINT64_C(0x2aa))

static char *storage_image(void)
{
    g_autofree uint8_t *bytes = g_malloc(16 * 1024 * 1024);
    char *path = NULL;
    GError *error = NULL;
    int fd = g_file_open_tmp("ni-storage-XXXXXX", &path, &error);

    g_assert_no_error(error);
    memset(bytes, 0xff, 16 * 1024 * 1024);
    g_assert_cmpint(write(fd, bytes, 16 * 1024 * 1024), ==, 16 * 1024 * 1024);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *storage_start(const char *image, const char *extra)
{
    g_autofree char *options = g_strdup_printf(
        "-global sgi-pci-expansion-ni-fixture.power=on "
        "-global sgi-pci-expansion-ni-fixture.storage=on "
        "-global sgi-pci-expansion-ni-fixture.storage-drive=cfg "
        "-drive if=none,id=cfg,file=%s,format=raw %s", image, extra);

    return management_start(options);
}

static void storage_cycle(QTestState *qts, uint64_t base, uint64_t offset,
                           uint16_t value)
{
    qtest_writeq(qts, base + offset, value);
    result(qts, 0, MEMTX_OK);
}

static void storage_unlock(QTestState *qts, uint64_t base)
{
    storage_cycle(qts, base, STORAGE_UNLOCK0, 0xaa);
    storage_cycle(qts, base, STORAGE_UNLOCK1, 0x55);
}

static void storage_program(QTestState *qts, uint64_t base, uint64_t offset,
                             uint16_t value)
{
    storage_unlock(qts, base);
    storage_cycle(qts, base, STORAGE_UNLOCK0, 0xa0);
    storage_cycle(qts, base, offset * 4, value);
}

#define ABSENCE_OPTIONS "-global sgi-pci-expansion-ni-fixture.power=on " \
                        "-global sgi-pci-expansion-ni-fixture." \
                        "processor-absence=on"
static const uint64_t presence_offsets[] = { 0x40, 0x48, 0x200040, 0x200048 };

static void absence_words(QTestState *qts, unsigned nasid)
{
    for (unsigned requester = 0; requester < 2; requester++) {
        for (unsigned word = 0; word < G_N_ELEMENTS(presence_offsets); word++) {
            uint64_t address = REQUESTER(requester) + IO + NODE(nasid) +
                               0x1800000 + presence_offsets[word];

            g_assert_cmphex(qtest_readq(qts, address), ==, 0);
            result(qts, requester, MEMTX_OK);
        }
    }
}

#define PENDING_OPTIONS ABSENCE_OPTIONS " -global " \
    "sgi-origin300-pci-expansion-module.unconnected-uart-experiment=on " \
    "-global sgi-pci-expansion-ni-fixture.pending-registers=on"
#define PENDING_BASE(n) (REQUESTER(n) + BIC_IO + 0x1800000)

static void pending_write(QTestState *qts, unsigned pi, unsigned offset,
                           uint64_t value)
{
    qtest_writeq(qts, PENDING_BASE(0) + pi * 0x200000 + offset, value);
    result(qts, 0, MEMTX_OK);
}

static uint64_t pending_read(QTestState *qts, unsigned pi, unsigned bank)
{
    uint64_t value = qtest_readq(qts,
        PENDING_BASE(1) + pi * 0x200000 + 0x98 + bank * 8);

    result(qts, 1, MEMTX_OK);
    return value;
}

static void test_pending_registers(gconstpointer opaque)
{
    unsigned count = GPOINTER_TO_UINT(opaque);
    g_autofree char *options = g_strdup_printf(PENDING_OPTIONS
        " -global sgi-pci-expansion-ni-fixture.compute-origins=%u", count);
    QTestState *qts = management_start(options);
    static const unsigned reflected[] = { 54, 55, 62, 63 };
    uint64_t sentinel = UINT64_C(0x123456789abcdef0);

    qtest_writeq(qts, COMPUTE + 0x608000, sentinel);
    for (unsigned pi = 0; pi < 2; pi++) {
        for (unsigned bank = 0; bank < 2; bank++) {
            unsigned bit = 16 + pi + bank * 2;
            uint64_t expected = UINT64_C(1) << bit;

            pending_write(qts, pi, 0x90, 0x100 | bank * 64 | bit);
            g_assert_cmphex(pending_read(qts, pi, bank), ==, expected);
            for (unsigned origin = 0; origin < count; origin++) {
                qtest_writeq(qts, MANAGEMENT + 144, origin);
                g_assert_cmphex(qtest_readq(qts, PENDING_BASE(0) +
                    pi * 0x200000 + 0x98 + bank * 8), ==, expected);
                result(qts, 0, MEMTX_OK);
            }
            qtest_writeq(qts, MANAGEMENT + 144, count);
            qtest_writeq(qts, PENDING_BASE(0) + pi * 0x200000 + 0x90, 0);
            result(qts, 0, MEMTX_ACCESS_ERROR);
            qtest_writeq(qts, MANAGEMENT + 144, 0);
            g_assert_cmphex(pending_read(qts, pi, bank), ==, expected);
            pending_write(qts, pi, 0x180, bank * 64 | bit);
            g_assert_cmphex(pending_read(qts, pi, bank), ==, 0);
        }
        for (unsigned i = 0; i < G_N_ELEMENTS(reflected); i++) {
            pending_write(qts, pi, 0x90, 0x140 | reflected[i]);
            g_assert_cmphex(pending_read(qts, pi, 1), ==, 0);
            pending_write(qts, pi, 0x180, 0x40 | reflected[i]);
            g_assert_cmphex(pending_read(qts, pi, 1), ==, 0);
        }
        pending_write(qts, pi, 0x10, 0x12345678);
        pending_write(qts, pi, 0x90, 0x110 + pi);
    }
    power_set(qts, false);
    qtest_readq(qts, PENDING_BASE(0) + 0x98);
    result(qts, 0, MEMTX_ACCESS_ERROR);
    power_set(qts, true);
    for (unsigned pi = 0; pi < 2; pi++) {
        g_assert_cmphex(pending_read(qts, pi, 0), ==, UINT64_C(1) << (16 + pi));
    }
    qtest_writeq(qts, MANAGEMENT + 176, 0);
    for (unsigned pi = 0; pi < 2; pi++) {
        g_assert_cmphex(pending_read(qts, pi, 0), ==, 0);
        g_assert_cmphex(qtest_readq(qts, PENDING_BASE(0) +
                                  pi * 0x200000 + 0x10), ==, 0x12345678);
        result(qts, 0, MEMTX_OK);
    }
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x608000), ==, sentinel);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, 0);
    qtest_quit(qts);
}

static void test_processor_absence(void)
{
    QTestState *qts = management_start(ABSENCE_OPTIONS);

    absence_words(qts, 7);
    g_assert_cmpuint(qtest_readq(qts, MANAGEMENT + 120), ==, 1);
    qtest_writeq(qts, MANAGEMENT + 120, 1);
    g_assert_cmpuint(qtest_readq(qts, MANAGEMENT + 128), ==, 1);
    for (unsigned word = 0; word < G_N_ELEMENTS(presence_offsets); word++) {
        uint64_t address = REQUESTER(0) + BIC_IO +
                           0x1800000 + presence_offsets[word];

        g_assert_cmphex(qtest_readq(qts, COMPUTE + presence_offsets[word]),
                        ==, word < 2);
        qtest_writeq(qts, address, UINT64_MAX);
        result(qts, 0, MEMTX_OK);
    }
    absence_words(qts, 7);
    for (unsigned owner = 0; owner < 2; owner++) {
        qtest_writeq(qts, MANAGEMENT + 8, owner);
        absence_words(qts, 7);
    }
    qtest_writeq(qts, MANAGEMENT + 48, 1);
    absence_words(qts, 7);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    absence_words(qts, 7);
    qtest_quit(qts);
}

static void absence_transfer(QTestState *qts, uint64_t address,
                             unsigned length, bool write,
                             MemTxResult expected)
{
    qtest_writeq(qts, MANAGEMENT + 88, address);
    qtest_writeq(qts, MANAGEMENT + 96, UINT64_MAX);
    qtest_writeq(qts, MANAGEMENT + 104,
                 length | ((uint64_t)write << 32));
    g_assert_cmpuint(qtest_readq(qts, MANAGEMENT + 112), ==, expected);
}

static void test_pending_refusals(void)
{
    QTestState *qts = management_start(PENDING_OPTIONS);
    static const unsigned widths[] = { 1, 2, 4, 16 };
    static const unsigned offsets[] = { 0x08, 0x18, 0x88, 0xb0, 0x188 };

    pending_write(qts, 0, 0x90, 0x110);
    for (unsigned pi = 0; pi < 2; pi++) {
        uint64_t base = BIC_IO + 0x1800000 + pi * 0x200000;

        for (unsigned i = 0; i < G_N_ELEMENTS(widths); i++) {
            absence_transfer(qts, base + 0x90, widths[i], true, MEMTX_ERROR);
            absence_transfer(qts, base + 0x98, widths[i], false, MEMTX_ERROR);
        }
        absence_transfer(qts, base + 0x91, 8, true, MEMTX_DECODE_ERROR);
        absence_transfer(qts, base + 0x90, 8, false, MEMTX_DECODE_ERROR);
        absence_transfer(qts, base + 0x180, 8, false, MEMTX_DECODE_ERROR);
        absence_transfer(qts, base + 0x98, 8, true, MEMTX_DECODE_ERROR);
        absence_transfer(qts, base + 0xa0, 8, true, MEMTX_DECODE_ERROR);
        for (unsigned i = 0; i < G_N_ELEMENTS(offsets); i++) {
            absence_transfer(qts, base + offsets[i], 8, true,
                             MEMTX_DECODE_ERROR);
        }
        absence_transfer(qts, BIC_HUB + pi * 0x200000 + 0x90, 8, true,
                         pi ? MEMTX_DECODE_ERROR : MEMTX_OK);
        qtest_writeq(qts, MANAGEMENT + 88,
            (UINT64_C(1) << 47) | (UINT64_C(7) << 39) |
            (0x1800090 + pi * 0x200000));
        qtest_writeq(qts, MANAGEMENT + 136, 0);
        g_assert_cmphex(qtest_readq(qts, MANAGEMENT + 112), ==,
                        MEMTX_DECODE_ERROR);
    }
    g_assert_cmphex(pending_read(qts, 0, 0), ==, UINT64_C(1) << 16);
    g_assert_cmphex(pending_read(qts, 1, 0), ==, 0);
    qtest_quit(qts);
    qts = management_start(ABSENCE_OPTIONS);
    absence_transfer(qts, BIC_IO + 0x1800090, 8, true, MEMTX_DECODE_ERROR);
    qtest_quit(qts);
}

static void test_pending_permissions(void)
{
    GError *error = NULL;
    g_autofree char *log = NULL;
    g_autofree char *contents = NULL;
    int fd = g_file_open_tmp("ni-pending-permissions-XXXXXX", &log, &error);
    g_autofree char *options = g_strdup_printf(PENDING_OPTIONS
        " -d guest_errors -D %s", log);
    uint64_t packet = (UINT64_C(1) << 47) | (UINT64_C(7) << 39) | 0x1800090;
    QTestState *qts;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    qts = management_start(options);
    qtest_writeq(qts, MANAGEMENT + 88, packet);
    qtest_writeq(qts, MANAGEMENT + 96, 0x110);
    pending_write(qts, 0, 0x10, 0);
    qtest_writeq(qts, MANAGEMENT + 192, 1);
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT + 112), ==, MEMTX_OK);
    g_assert_cmphex(pending_read(qts, 0, 0), ==, 0);
    pending_write(qts, 0, 0x10, 1);
    qtest_writeq(qts, MANAGEMENT + 192, 1);
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT + 112), ==, MEMTX_OK);
    g_assert_cmphex(pending_read(qts, 0, 0), ==, UINT64_C(1) << 16);
    pending_write(qts, 0, 0x90, 16);
    qtest_writeq(qts, REQUESTER(0) + BIC_HUB + 0x400118, 0);
    result(qts, 0, MEMTX_OK);
    qtest_writeq(qts, MANAGEMENT + 192, 1);
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT + 112), ==, MEMTX_ACCESS_ERROR);
    g_assert_cmphex(pending_read(qts, 0, 0), ==, 0);
    qtest_writeq(qts, MANAGEMENT + 200, 1);
    for (unsigned pi = 0; pi < 2; pi++) {
        g_assert_cmphex(pending_read(qts, pi, 1), ==, UINT64_C(1) << 54);
        pending_write(qts, pi, 0x90, 0x40 | 54);
        g_assert_cmphex(pending_read(qts, pi, 1), ==, UINT64_C(1) << 54);
    }
    qtest_writeq(qts, MANAGEMENT + 200, 0);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(contents, "denied by I/O protect"));
    g_assert_cmpint(unlink(log), ==, 0);
}

static void test_pending_child_compatibility(void)
{
    QTestState *qts = management_start(PENDING_OPTIONS);
    uint64_t ordinary_protect = qtest_readq(qts, COMPUTE + 0x10);

    pending_write(qts, 0, 0x90, 0x110);
    pending_write(qts, 0, 0x180, 0x151);
    pending_write(qts, 0, 0x10, 0x12345678);
    for (unsigned kind = 0; kind < 3; kind++) {
        qtest_writeq(qts, MANAGEMENT + 184, kind);
        g_assert_cmphex(qtest_readq(qts, MANAGEMENT + 112), ==, 2);
        g_assert_cmphex(pending_read(qts, 0, 0), ==, UINT64_C(1) << 16);
        g_assert_cmphex(pending_read(qts, 0, 1), ==, UINT64_C(1) << 17);
    }
    qtest_writeq(qts, MANAGEMENT + 184, 3);
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT + 112), ==, 13);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, UINT64_C(1) << 21);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0xa0), ==, UINT64_C(1) << 22);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x10), ==,
                    ordinary_protect ^ UINT64_C(0xa5a5a5a5));
    qtest_quit(qts);
}

static void test_pending_crossbar(void)
{
    QTestState *qts = management_start(PENDING_OPTIONS);
    uint64_t xbow = REQUESTER(0) + BIC_IO;
    uint64_t packet = (UINT64_C(1) << 47) | (UINT64_C(7) << 39) | 0x1800090;

    pending_write(qts, 0, 0x10, UINT64_MAX);
    pending_write(qts, 1, 0x10, UINT64_MAX);
    qtest_writel(qts, xbow + 0x34, (16U << 24) | (0xaU << 16) |
                 (packet >> 32));
    qtest_writel(qts, xbow + 0x3c, packet);
    qtest_writel(qts, xbow + 0x24, 1U << 5);
    qtest_writeb(qts, xbow + 0x20, 0);
    g_assert_cmphex(pending_read(qts, 0, 0), ==, UINT64_C(1) << 16);
    g_assert_cmphex(pending_read(qts, 1, 0), ==, 0);
    qtest_readl(qts, xbow + 0x54);
    g_assert_cmphex(pending_read(qts, 0, 0), ==, 0);
    qtest_writel(qts, xbow + 0x3c, packet + 0x200000);
    qtest_writeb(qts, xbow + 0x20, 0);
    g_assert_cmphex(pending_read(qts, 1, 0), ==, UINT64_C(1) << 16);
    qtest_readl(qts, xbow + 0x54);
    pending_write(qts, 1, 0x10, 0);
    qtest_writeb(qts, xbow + 0x20, 0);
    g_assert_cmphex(pending_read(qts, 1, 0), ==, 0);
    qtest_quit(qts);
}

static void test_processor_absence_refusals(void)
{
    static const unsigned widths[] = { 1, 2, 4, 16 };
    QTestState *qts = management_start(ABSENCE_OPTIONS);

    for (unsigned word = 0; word < G_N_ELEMENTS(presence_offsets); word++) {
        uint64_t offset = presence_offsets[word];
        uint64_t address = BIC_IO + 0x1800000 + offset;

        for (unsigned width = 0; width < G_N_ELEMENTS(widths); width++) {
            for (unsigned write = 0; write < 2; write++) {
                absence_transfer(qts, address, widths[width], write,
                                 MEMTX_ERROR);
            }
        }
        absence_transfer(qts, address + 1, 8, false, MEMTX_DECODE_ERROR);
        absence_transfer(qts, address + 1, 8, true, MEMTX_DECODE_ERROR);
        absence_transfer(qts, BIC_HUB + offset, 8, false,
                         MEMTX_DECODE_ERROR);
        absence_transfer(qts, BIC_HUB + offset, 8, true,
                         MEMTX_DECODE_ERROR);
    }
    absence_transfer(qts, BIC_IO + 0x1800050, 8, false, MEMTX_DECODE_ERROR);
    absence_transfer(qts, BIC_IO + 0x18000a0, 8, false, MEMTX_DECODE_ERROR);
    absence_transfer(qts, IO + NODE(8) + 0x1800040, 8, false,
                     MEMTX_DECODE_ERROR);
    absence_words(qts, 7);
    power_set(qts, false);
    absence_transfer(qts, BIC_IO + 0x1800040, 8, false, MEMTX_ACCESS_ERROR);
    absence_transfer(qts, BIC_IO + 0x1800040, 8, true, MEMTX_ACCESS_ERROR);
    power_set(qts, true);
    absence_words(qts, 7);
    qtest_quit(qts);

    qts = management_start(ABSENCE_OPTIONS
        " -global sgi-pci-expansion-ni-fixture.connected=off");
    absence_transfer(qts, BIC_IO + 0x1800040, 8, false, MEMTX_DECODE_ERROR);
    qtest_quit(qts);
    qts = management_start("-global sgi-pci-expansion-ni-fixture.power=on");
    absence_transfer(qts, BIC_IO + 0x1800040, 8, false, MEMTX_DECODE_ERROR);
    qtest_writeq(qts, MANAGEMENT + 120, 1);
    g_assert_cmpuint(qtest_readq(qts, MANAGEMENT + 128), ==, 1);
    absence_transfer(qts, BIC_IO + 0x1800040, 8, false, MEMTX_DECODE_ERROR);
    qtest_quit(qts);
}

static void test_processor_absence_remap(void)
{
    QTestState *qts = management_start(ABSENCE_OPTIONS);
    uint64_t address = REQUESTER(0) + BIC_IO + 0x1e00000;
    uint64_t revision = qtest_readq(qts, address);

    result(qts, 0, MEMTX_OK);
    qtest_writeq(qts, address,
                 (revision & ~(UINT64_C(0xff) << 32)) | (UINT64_C(9) << 32));
    result(qts, 0, MEMTX_OK);
    absence_transfer(qts, BIC_IO + 0x1800040, 8, false, MEMTX_DECODE_ERROR);
    absence_words(qts, 9);
    qtest_quit(qts);
}

static void test_pending_migration(gconstpointer opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("ni-pending-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = management_start(kind == 2 ? ABSENCE_OPTIONS :
                                       PENDING_OPTIONS);
    QTestState *to = management_start(kind == 1 ?
        ABSENCE_OPTIONS " -incoming defer" : kind == 3 ?
        PENDING_OPTIONS " -global sgi-pci-expansion-ni-fixture."
        "compute-origins=4 -incoming defer" :
        PENDING_OPTIONS " -incoming defer");

    g_assert_no_error(error);
    if (kind != 2) {
        for (unsigned pi = 0; pi < 2; pi++) {
            pending_write(from, pi, 0x10, 0x76543210);
            pending_write(from, pi, 0x90, 0x110 + pi);
            pending_write(from, pi, 0x180, 0x152 + pi);
        }
    }
    if (kind == 4) {
        power_set(from, false);
    }
    if (kind && kind != 4) {
        qtest_set_expected_status(to, EXIT_FAILURE);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    if (kind && kind != 4) {
        qtest_qmp_assert_success(from,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        qtest_wait_qemu(to);
    } else {
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration_status(to, "completed", 30);
        if (kind == 4) {
            qtest_readq(to, PENDING_BASE(0) + 0x98);
            result(to, 0, MEMTX_ACCESS_ERROR);
            power_set(to, true);
        }
        for (unsigned pi = 0; pi < 2; pi++) {
            g_assert_cmphex(pending_read(to, pi, 0), ==,
                            UINT64_C(1) << (16 + pi));
            g_assert_cmphex(pending_read(to, pi, 1), ==,
                            UINT64_C(1) << (18 + pi));
            g_assert_cmphex(qtest_readq(to, PENDING_BASE(0) +
                                      pi * 0x200000 + 0x10), ==, 0x76543210);
            result(to, 0, MEMTX_OK);
        }
    }
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_rmdir(dir);
}

static void test_processor_absence_migration(const void *data)
{
    unsigned kind = GPOINTER_TO_UINT(data);
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("ni-absence-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = management_start(kind == 2 ?
        "-global sgi-pci-expansion-ni-fixture.power=on" : ABSENCE_OPTIONS);
    QTestState *to = management_start(kind == 1 ?
        "-global sgi-pci-expansion-ni-fixture.power=on -incoming defer" :
        ABSENCE_OPTIONS " -incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    if (kind) {
        qtest_set_expected_status(to, EXIT_FAILURE);
    } else {
        absence_words(from, 7);
        power_set(from, false);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    if (kind) {
        qtest_qmp_assert_success(from,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        qtest_wait_qemu(to);
    } else {
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration_status(to, "completed", 30);
        absence_transfer(to, BIC_IO + 0x1800040, 8, false, MEMTX_ACCESS_ERROR);
        power_set(to, true);
        absence_words(to, 7);
    }
    qtest_quit(from);
    qtest_quit(to);
    if (unlink(socket)) {
        g_assert_cmpint(errno, ==, ENOENT);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_processor_absence_old_stream(const void *data)
{
    bool selected = GPOINTER_TO_UINT(data);
    static const char name[] = "sgi-origin300-pci-expansion-module";
    uint8_t header[1 + sizeof(name) - 1 + 8];
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("ni-absence-v5-XXXXXX", &error);
    g_autofree char *file = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *stream = NULL;
    gsize length, found = 0, payload = 0;
    QTestState *from = management_start(ABSENCE_OPTIONS);
    QTestState *to = management_start(selected ?
        ABSENCE_OPTIONS " -incoming defer" :
        "-global sgi-pci-expansion-ni-fixture.power=on -incoming defer");

    g_assert_no_error(error);
    file = g_build_filename(directory, "migration.bin", NULL);
    uri = g_strdup_printf("file:%s", file);
    sgi_migrate_stopped(from, uri);
    g_assert_true(g_file_get_contents(file, &stream, &length, &error));
    g_assert_no_error(error);
    header[0] = sizeof(name) - 1;
    memcpy(header + 1, name, sizeof(name) - 1);
    stl_be_p(header + sizeof(name), 0);
    stl_be_p(header + sizeof(name) + 4, 8);
    for (gsize i = 0; i + sizeof(header) < length; i++) {
        if (!memcmp(stream + i, header, sizeof(header))) {
            payload = i + sizeof(header);
            found++;
        }
    }
    g_assert_cmpuint(found, ==, 1);
    g_assert_cmpuint((uint8_t)stream[payload], ==, 0);
    g_assert_cmpuint(ldl_be_p(stream + payload + 1), ==, 0);
    g_assert_cmpuint((uint8_t)stream[payload + 5], ==, 0);
    g_assert_cmpuint(ldl_be_p(stream + payload + 6), ==, 2);
    g_assert_cmpuint((uint8_t)stream[payload + 10], ==, 1);
    /* Remove UART selection/count and processor-absence selection for v5. */
    stl_be_p(stream + payload - 4, 5);
    memmove(stream + payload, stream + payload + 11, length - payload - 11);
    g_assert_true(g_file_set_contents(file, stream, length - 11, &error));
    g_assert_no_error(error);
    if (selected) {
        qtest_set_expected_status(to, EXIT_FAILURE);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    if (selected) {
        qtest_wait_qemu(to);
    } else {
        sgi_wait_migration_status(to, "completed", 30);
        absence_transfer(to, BIC_IO + 0x1800040, 8, false,
                         MEMTX_DECODE_ERROR);
    }
    qtest_quit(from);
    qtest_quit(to);
    g_assert_cmpint(unlink(file), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_storage_cpu_carrier(void)
{
    g_autofree char *image = storage_image();
    uint8_t bytes[8];
    uint64_t address = STORAGE_BASE + UINT64_C(0x10000000000) +
                       STORAGE_SECOND + 0x1e0000;
    int fd = open(image, O_WRONLY);
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    stq_be_p(bytes, UINT64_C(0xfedcba9876543210));
    g_assert_cmpint(pwrite(fd, bytes, sizeof(bytes), 0x9e0000),
                    ==, sizeof(bytes));
    g_assert_cmpint(close(fd), ==, 0);
    qts = storage_start(image, "");
    g_assert_cmphex(qtest_readq(qts, address), ==,
                    UINT64_C(0xfedcba9876543210));
    result(qts, 0, MEMTX_OK);
    qtest_readq(qts, REQUESTER(0) + UINT64_C(0x10000000000) +
                NODE(8) + 0x3a1e0000);
    result(qts, 0, MEMTX_DECODE_ERROR);
    power_set(qts, false);
    qtest_readq(qts, address);
    result(qts, 0, MEMTX_ACCESS_ERROR);
    power_set(qts, true);
    g_assert_cmphex(qtest_readq(qts, address), ==,
                    UINT64_C(0xfedcba9876543210));
    result(qts, 0, MEMTX_OK);
    qtest_quit(qts);
    qts = storage_start(image,
        "-global sgi-pci-expansion-ni-fixture.connected=off");
    qtest_readq(qts, address);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_quit(qts);
    g_assert_cmpint(unlink(image), ==, 0);
}

static void test_storage_unsupported_transfer(void)
{
    g_autofree char *image = storage_image();
    QTestState *qts = storage_start(image, "");

    for (unsigned position = 0; position < 2; position++) {
        uint64_t base = STORAGE_BASE + position * STORAGE_SECOND;

        storage_cycle(qts, base, 8 * 0x55, 0x98);
        g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 'Q');
        qtest_writeq(qts, MANAGEMENT + 88,
                     NODE(7) + 0x38000000 + position * STORAGE_SECOND);
        qtest_writeq(qts, MANAGEMENT + 96, 0xf0);
        qtest_writeq(qts, MANAGEMENT + 104, 16 | (UINT64_C(1) << 32));
        g_assert_cmpuint(qtest_readq(qts, MANAGEMENT + 112), ==, MEMTX_ERROR);
        g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 'Q');
        qtest_writeq(qts, MANAGEMENT + 104, 16);
        g_assert_cmpuint(qtest_readq(qts, MANAGEMENT + 112), ==, MEMTX_ERROR);
        g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 'Q');
        storage_cycle(qts, base, 0, 0xf0);
        g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 0xffff);
    }
    qtest_quit(qts);
    g_assert_cmpint(unlink(image), ==, 0);
}

static void test_storage_program_power_reset(void)
{
    g_autofree char *image = storage_image();
    QTestState *qts = storage_start(image, "");

    for (unsigned position = 0; position < 2; position++) {
        uint64_t base = STORAGE_BASE + position * STORAGE_SECOND;
        uint16_t value = position ? 0x5678 : 0x1234;

        storage_cycle(qts, base, STORAGE_UNLOCK0, 0xaa);
        power_set(qts, false);
        qtest_writeq(qts, base + STORAGE_UNLOCK1, 0x55);
        result(qts, 0, MEMTX_ACCESS_ERROR);
        qtest_readw(qts, base);
        result(qts, 0, MEMTX_ACCESS_ERROR);
        power_set(qts, true);
        storage_cycle(qts, base, STORAGE_UNLOCK1, 0x55);
        storage_cycle(qts, base, STORAGE_UNLOCK0, 0xa0);
        storage_cycle(qts, base, 0x100 * 4, value);
        g_assert_cmphex(qtest_readw(qts, base + 0x100), ==, value);
        storage_cycle(qts, base, 8 * 0x55, 0x98);
        g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 'Q');
        qtest_writeq(qts, MANAGEMENT + 8, 1);
        qtest_writeq(qts, MANAGEMENT + 48, 0);
        g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 'Q');
        power_set(qts, false);
        power_set(qts, true);
        g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 'Q');
        qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
        g_assert_cmphex(qtest_readw(qts, base + 0x100), ==, value);
        g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 0xffff);
    }
    qtest_quit(qts);
    qts = storage_start(image, "");
    g_assert_cmphex(qtest_readw(qts, STORAGE_BASE + 0x100), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, STORAGE_BASE + STORAGE_SECOND + 0x100),
                    ==, 0x5678);
    qtest_quit(qts);
    g_assert_cmpint(unlink(image), ==, 0);
}

static void test_storage_erase_while_off(void)
{
    g_autofree char *image = storage_image();
    QTestState *qts = storage_start(image, "");
    uint64_t base = STORAGE_BASE + STORAGE_SECOND;

    /* The fixture starts paused; CFI timers require a running VM clock. */
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    storage_program(qts, base, 0x20000, 0x1234);
    storage_program(qts, STORAGE_BASE, 0x20000, 0x5678);
    storage_unlock(qts, base);
    storage_cycle(qts, base, STORAGE_UNLOCK0, 0x80);
    storage_unlock(qts, base);
    storage_cycle(qts, base, 0x20000 * 4, 0x30);
    power_set(qts, false);
    qtest_clock_step(qts, INT64_C(10000000000));
    qtest_readw(qts, base + 0x20000);
    result(qts, 0, MEMTX_ACCESS_ERROR);
    power_set(qts, true);
    g_assert_cmphex(qtest_readw(qts, base + 0x20000), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, STORAGE_BASE + 0x20000), ==, 0x5678);
    qtest_quit(qts);
    g_assert_cmpint(unlink(image), ==, 0);
}

static void test_storage_migration_off(void)
{
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("ni-storage-migrate-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *source = storage_image();
    g_autofree char *destination = storage_image();
    QTestState *from = storage_start(source, "");
    QTestState *to = storage_start(destination, "-incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    storage_program(from, STORAGE_BASE + STORAGE_SECOND, 0x400, 0x3456);
    storage_cycle(from, STORAGE_BASE + STORAGE_SECOND, 8 * 0x55, 0x98);
    power_set(from, false);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    qtest_readw(to, STORAGE_BASE + STORAGE_SECOND + 0x20);
    result(to, 0, MEMTX_ACCESS_ERROR);
    power_set(to, true);
    g_assert_cmphex(qtest_readw(to, STORAGE_BASE + STORAGE_SECOND + 0x20),
                    ==, 'Q');
    storage_cycle(to, STORAGE_BASE + STORAGE_SECOND, 0, 0xf0);
    g_assert_cmphex(qtest_readw(to, STORAGE_BASE + STORAGE_SECOND + 0x400),
                    ==, 0x3456);
    qtest_quit(from);
    qtest_quit(to);
    to = storage_start(destination, "");
    g_assert_cmphex(qtest_readw(to, STORAGE_BASE + STORAGE_SECOND + 0x400),
                    ==, 0x3456);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(unlink(source), ==, 0);
    g_assert_cmpint(unlink(destination), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_storage_migration_erase_off(void)
{
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("ni-storage-erase-migrate-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *source = storage_image();
    g_autofree char *destination = storage_image();
    QTestState *from = storage_start(source, "");
    QTestState *to = storage_start(destination, "-incoming defer");
    uint64_t upper = STORAGE_BASE + STORAGE_SECOND;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(from, "{'execute':'cont'}");
    storage_program(from, upper, 0x20000, 0x1234);
    storage_program(from, STORAGE_BASE, 0x20000, 0x5678);
    storage_unlock(from, upper);
    storage_cycle(from, upper, STORAGE_UNLOCK0, 0x80);
    storage_unlock(from, upper);
    storage_cycle(from, upper, 0x20000 * 4, 0x30);
    g_assert_cmphex(qtest_readw(from, upper + 0x20000) & 0x80, ==, 0);
    power_set(from, false);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    qtest_readw(to, upper + 0x20000);
    result(to, 0, MEMTX_ACCESS_ERROR);
    power_set(to, true);
    g_assert_cmphex(qtest_readw(to, upper + 0x20000) & 0x80, ==, 0);
    power_set(to, false);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    qtest_clock_step(to, INT64_C(10000000000));
    qtest_readw(to, upper + 0x20000);
    result(to, 0, MEMTX_ACCESS_ERROR);
    power_set(to, true);
    g_assert_cmphex(qtest_readw(to, upper + 0x20000), ==, 0xffff);
    g_assert_cmphex(qtest_readw(to, STORAGE_BASE + 0x20000), ==, 0x5678);
    qtest_quit(from);
    qtest_quit(to);
    to = storage_start(destination, "");
    g_assert_cmphex(qtest_readw(to, upper + 0x20000), ==, 0xffff);
    g_assert_cmphex(qtest_readw(to, STORAGE_BASE + 0x20000), ==, 0x5678);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(unlink(source), ==, 0);
    g_assert_cmpint(unlink(destination), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static bool storage_backend_attached(QTestState *qts)
{
    QDict *reply = qtest_qmp(qts, "{'execute':'query-block'}");
    const QListEntry *entry;
    bool attached = false;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(reply, "return"), entry) {
        QDict *block = qobject_to(QDict, qlist_entry_obj(entry));

        if (!strcmp(qdict_get_str(block, "device"), "cfg")) {
            attached = qdict_haskey(block, "qdev");
        }
    }
    qobject_unref(reply);
    return attached;
}

static void test_storage_late_binding(void)
{
    g_autofree char *image = storage_image();
    QTestState *qts = storage_start(image,
        "-global sgi-pci-expansion-ni-fixture.storage-late=on");

    g_assert_cmpuint(qtest_readq(qts, MANAGEMENT + 80), ==, 1);
    g_assert_false(storage_backend_attached(qts));
    qtest_readq(qts, STORAGE_BASE);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_quit(qts);
    g_assert_cmpint(unlink(image), ==, 0);
}

static void test_storage_teardown(void)
{
    g_autofree char *image = storage_image();
    QTestState *qts = storage_start(image, "");

    g_assert_true(storage_backend_attached(qts));
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'realized','value':false}}", PATH "/chassis");
    g_assert_false(storage_backend_attached(qts));
    qtest_readq(qts, STORAGE_BASE);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_quit(qts);
    g_assert_cmpint(unlink(image), ==, 0);
}

static void test_storage_migration_mismatch(const void *data)
{
    unsigned kind = GPOINTER_TO_UINT(data);
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("ni-storage-mismatch-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *source = storage_image();
    g_autofree char *destination = storage_image();
    const char *owner = kind == 0 ?
        "-global sgi-pci-expansion-ni-fixture.storage-module-id="
        "fixture-otherx " :
        kind == 1 ? "-global sgi-pci-expansion-ni-fixture."
                    "storage-controller-id=fixture-controllER " : "";
    g_autofree char *extra = g_strconcat(owner, "-incoming defer", NULL);
    QTestState *from = kind == 3 ?
        management_start("-global sgi-pci-expansion-ni-fixture.power=on") :
        storage_start(source, "");
    QTestState *to = kind == 2 ?
        management_start("-global sgi-pci-expansion-ni-fixture.power=on "
                         "-incoming defer") : storage_start(destination, extra);

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
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
    g_assert_cmpint(unlink(source), ==, 0);
    g_assert_cmpint(unlink(destination), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_storage_read(void)
{
    g_autofree uint8_t *bytes = g_malloc0(16 * 1024 * 1024);
    g_autofree char *path = NULL;
    g_autofree char *options = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    int fd = g_file_open_tmp("ni-configuration-storage-XXXXXX", &path, &error);
    uint64_t base = REQUESTER(0) + NODE(7) + 0x38000000;

    g_assert_no_error(error);
    memset(bytes, 0xff, 16 * 1024 * 1024);
    stq_be_p(bytes + 0x1e0000, UINT64_C(0x0123456789abcdef));
    stq_be_p(bytes + 0x9e0000, UINT64_C(0xfedcba9876543210));
    g_assert_cmpint(write(fd, bytes, 16 * 1024 * 1024), ==, 16 * 1024 * 1024);
    g_assert_cmpint(close(fd), ==, 0);
    options = g_strdup_printf(
        "-global sgi-pci-expansion-ni-fixture.power=on "
        "-global sgi-pci-expansion-ni-fixture.storage=on "
        "-global sgi-pci-expansion-ni-fixture.storage-drive=cfg "
        "-drive if=none,id=cfg,file=%s,format=raw", path);
    qts = management_start(options);
    g_assert_cmphex(qtest_readq(qts, base + 0x1e0000), ==,
                    UINT64_C(0x0123456789abcdef));
    result(qts, 0, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, base + 0x21e0000), ==,
                    UINT64_C(0xfedcba9876543210));
    result(qts, 0, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, base + 0x1e0000), ==, 0x01);
    result(qts, 0, MEMTX_OK);
    g_assert_cmphex(qtest_readw(qts, base + 0x1e0000), ==, 0x0123);
    result(qts, 0, MEMTX_OK);
    g_assert_cmphex(qtest_readl(qts, base + 0x1e0000), ==, 0x01234567);
    result(qts, 0, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, base + 0x7fffff), ==, 0xff);
    result(qts, 0, MEMTX_OK);
    qtest_readq(qts, base + 0x800000);
    result(qts, 0, MEMTX_ERROR);
    g_assert_cmphex(qtest_readb(qts, base + 0x2000000), ==, 0xff);
    result(qts, 0, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, base + 0x27fffff), ==, 0xff);
    result(qts, 0, MEMTX_OK);
    qtest_readq(qts, base + 0x8000000);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_writel(qts, base + STORAGE_UNLOCK0, 0xaa);
    result(qts, 0, MEMTX_ERROR);
    qtest_writeq(qts, base + STORAGE_UNLOCK0, UINT64_C(0x100aa));
    result(qts, 0, MEMTX_ERROR);
    qtest_quit(qts);
    g_unlink(path);
}

static void test_power_pio(void)
{
    QTestState *qts = management_start(
        "-global sgi-pci-expansion-ni-fixture.power=on");

    for (unsigned i = 0; i < 6; i++) {
        g_assert_cmphex(qtest_readl(qts, head(0, i) + 4), ==, 0x2d002049);
        result(qts, 0, MEMTX_OK);
    }
    /* Explicit fixture request to the owning controller, not guest MMIO. */
    qtest_writeq(qts, 0x31002000 + 16, 0);
    for (unsigned i = 0; i < 6; i++) {
        qtest_readl(qts, head(0, i) + 4);
        result(qts, 0, MEMTX_ACCESS_ERROR);
    }
    management_query(qts, 0, 0x4ffff006, 19);
    management_identity(qts, 0, 19, 1);
    qtest_writeq(qts, 0x31002000 + 16, 1);
    for (unsigned i = 0; i < 6; i++) {
        g_assert_cmphex(qtest_readl(qts, head(0, i) + 4), ==, 0x2d002049);
        result(qts, 0, MEMTX_OK);
    }
    qtest_quit(qts);
}

#define POWER_OPTIONS "-global sgi-pci-expansion-ni-fixture.power=on"

static void power_set(QTestState *qts, bool on)
{
    qtest_writeq(qts, MANAGEMENT + 16, on);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 64), ==, 1);
}

static void test_power_owner(void)
{
    QTestState *qts = management_start(POWER_OPTIONS);

    qtest_writeq(qts, MANAGEMENT + 24, 0);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 64), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 48), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 56), ==, 1);
    qtest_writeq(qts, MANAGEMENT + 32, 1);
    qtest_writeq(qts, MANAGEMENT + 16, 0);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 64), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 48), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 56), ==, 1);
    g_assert_cmphex(qtest_readl(qts, head(0, 0) + 4), ==, 0x2d002049);
    g_assert_cmpint((int32_t)qtest_readq(qts, STATUS + 88), ==, -EBUSY);
    qtest_writeq(qts, MANAGEMENT + 32, 0);
    power_set(qts, false);
    qtest_writeq(qts, MANAGEMENT + 24, 1);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 48), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 56), ==, 0);
    qtest_writeq(qts, MANAGEMENT + 8, 1);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 56), ==, 0);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmpuint(qtest_readq(qts, STATUS + 48), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 56), ==, 1);
    qtest_quit(qts);
}

static void test_power_dma_irq(void)
{
    QTestState *qts = management_start(POWER_OPTIONS
        " -global sgi-pci-expansion-ni-fixture.intx-map=1");
    uint64_t sentinel = UINT64_C(0x0123456789abcdef);
    uint64_t pending = UINT64_C(1) << 40;

    qtest_writeq(qts, COMPUTE + 0x90, 0x100 | 40);
    for (unsigned i = 0; i < 6; i++) {
        uint64_t h = head(0, i);

        configure_card(qts, i);
        configure_irq_pin(qts, i, 1, 16 + i);
        qtest_writeq(qts, h + 0x10000, NODE(3) | 0x8021 | (0xa << 8));
        qtest_writel(qts, h + 0x84, (0xa << 20) | 12);
    }
    qtest_writel(qts, head(0, 5) + 0x10c, 0);
    power_set(qts, false);
    qtest_writeq(qts, RAM + 0x8000, sentinel);
    for (unsigned i = 0; i < 6; i++) {
        uint64_t control = MANAGEMENT + 0x100 + i * 0x40;
        const uint64_t addresses[] = {
            UINT64_C(0xa080000000000000) + NODE(3) + 0x8000,
            0x40000000,
            0x80008000,
        };

        for (unsigned mode = 0; mode < G_N_ELEMENTS(addresses); mode++) {
            qtest_writeq(qts, control + 8, addresses[mode]);
            qtest_writeq(qts, control + 16, sentinel ^ 1);
            qtest_writeq(qts, control + 24, 1);
            g_assert_cmphex(qtest_readq(qts, control + 24), ==,
                            MEMTX_ACCESS_ERROR);
            qtest_writeq(qts, control + 24, 2);
            g_assert_cmphex(qtest_readq(qts, control + 24), ==,
                            MEMTX_ACCESS_ERROR);
            g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, sentinel);
        }
        qtest_writeq(qts, control + 32, 1);
        g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 0);
    }
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, pending);
    power_set(qts, true);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 5);
    for (unsigned i = 0; i < 5; i++) {
        pending |= UINT64_C(1) << (16 + i);
    }
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, pending);
    power_set(qts, true);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 5);
    qtest_writel(qts, head(0, 5) + 0x10c, 2);
    power_set(qts, false);
    power_set(qts, true);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 11);
    pending |= UINT64_C(1) << 21;
    power_set(qts, false);
    for (unsigned i = 0; i < 6; i++) {
        qtest_writeq(qts, MANAGEMENT + 0x100 + i * 0x40 + 32, 0);
    }
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 11);
    power_set(qts, true);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 11);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, pending);
    for (unsigned i = 0; i < 6; i++) {
        dma(qts, i, 0x40000000, sentinel + i, true, MEMTX_OK);
        g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, sentinel + i);
    }
    qtest_quit(qts);
}

static void test_power_bte_completion(void)
{
    QTestState *qts = management_start(POWER_OPTIONS);
    uint64_t bte = REQUESTER(0) + BIC_HUB + 0x410000;
    uint64_t sentinel = UINT64_C(0x0123456789abcdef);

    /* The BIC has no local DMA RAM; its existing failure still queues IRQ. */
    qtest_writeq(qts, RAM + 0x8000, sentinel);
    qtest_writeq(qts, bte + 0x28, (25U << 16) | (3U << 1));
    qtest_writeq(qts, bte, 1 | (1U << 20));
    qtest_writeq(qts, MANAGEMENT + 56, 0);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 96), ==, 1);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==,
                    UINT64_C(1) << 25);
    qtest_writeq(qts, COMPUTE + 0x90, 25);
    qtest_writeq(qts, bte, 1 | (1U << 20));
    qtest_writeq(qts, MANAGEMENT + 56, 1);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 96), ==, 1);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, 0);
    g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, sentinel);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 0);
    power_set(qts, true);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, 0);
    g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, sentinel);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 0);
    qtest_quit(qts);
}

static void test_power_errors(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("ni-power-errors-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *qts = management_start(POWER_OPTIONS);
    QTestState *to = management_start(POWER_OPTIONS " -incoming defer");
    uint64_t h = head(0, 0);
    uint64_t xbow = UINT64_C(0x60000000);
    uint64_t address = (UINT64_C(1) << 47) |
                       (UINT64_C(3) << 39) | 0x1800090;

    g_assert_no_error(error);
    qtest_sysbus_mmio_map(qts, PATH "/chassis", 11, xbow);
    qtest_writel(qts, h + 0x24,
                 (qtest_readl(qts, h + 0x24) & ~15U) | widget[0]);
    qtest_writel(qts, h + 0x34, (0xa << 16) | (address >> 32));
    qtest_writel(qts, h + 0x3c, address);
    qtest_writel(qts, h + 0x12c, 26);
    qtest_writel(qts, h + 0x7c, 0);
    qtest_writel(qts, xbow + 0x34,
                 (27U << 24) | (0xa << 16) | (address >> 32));
    qtest_writel(qts, xbow + 0x3c, address);
    qtest_readl(qts, xbow + 0xd4);
    g_assert_cmphex(qtest_readl(qts, h + 0x104) & (1U << 24), !=, 0);
    g_assert_cmphex(qtest_readl(qts, xbow + 0xc) & (1U << 5), !=, 0);
    power_set(qts, false);
    power_set(qts, true);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 0);
    qtest_writel(qts, h + 0x10c, 1U << 24);
    qtest_writel(qts, xbow + 0x24, 1U << 5);
    g_assert_cmphex(qtest_readl(qts, h + 0x10c), ==, 1U << 24);
    g_assert_cmphex(qtest_readl(qts, xbow + 0x24) & (1U << 5), !=, 0);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 0);
    power_set(qts, false);
    power_set(qts, true);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 1);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==,
                    (UINT64_C(1) << 26) | (UINT64_C(1) << 27));
    qtest_writeq(qts, COMPUTE + 0x90, 26);
    qtest_writeq(qts, COMPUTE + 0x90, 27);
    power_set(qts, false);
    qtest_writeq(qts, MANAGEMENT + 64, 0);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 1);
    power_set(qts, true);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 2);
    qtest_writeq(qts, COMPUTE + 0x90, 26);
    qtest_writeq(qts, COMPUTE + 0x90, 27);
    power_set(qts, true);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 2);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, 0);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(qts, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmpuint(qtest_readq(to, STATUS + 16), ==, 0);
    g_assert_cmphex(qtest_readq(to, COMPUTE + 0x98), ==, 0);
    power_set(to, true);
    g_assert_cmpuint(qtest_readq(to, STATUS + 16), ==, 0);
    g_assert_cmphex(qtest_readq(to, COMPUTE + 0x98), ==, 0);
    power_set(to, false);
    power_set(to, true);
    g_assert_cmpuint(qtest_readq(to, STATUS + 16), ==, 1);
    g_assert_cmphex(qtest_readq(to, COMPUTE + 0x98), ==,
                    (UINT64_C(1) << 26) | (UINT64_C(1) << 27));
    qtest_quit(qts);
    qtest_quit(to);
    unlink(socket);
    g_rmdir(dir);
}

static void test_power_error_permissions(void)
{
    const uint64_t denied = UINT64_C(0xfffeffffffffffff);
    uint64_t address = (UINT64_C(1) << 47) |
                       (UINT64_C(3) << 39) | 0x1800090;

    for (unsigned target = 0; target < 2; target++) {
        const char *options = target ?
            " -global sgi-pci-expansion-ni-fixture.compute-device-mask="
            "0xfffeffffffffffff" :
            " -global sgi-bedrock.device-mask-reset=0xfffeffffffffffff";
        g_autofree char *extra = g_strdup_printf(POWER_OPTIONS "%s", options);
        QTestState *qts = management_start(extra);
        uint64_t h = head(0, 0);
        uint64_t mask = target ? COMPUTE + 0x400120 :
                               REQUESTER(0) + BIC_HUB + 0x400120;

        g_assert_cmphex(qtest_readq(qts, mask), ==, denied);
        qtest_writel(qts, h + 0x24,
                     (qtest_readl(qts, h + 0x24) & ~15U) | 0xe);
        qtest_writel(qts, h + 0x34, (0xa << 16) | (address >> 32));
        qtest_writel(qts, h + 0x3c, address);
        qtest_writel(qts, h + 0x12c, 26);
        qtest_writel(qts, h + 0x10c, 1U << 24);
        qtest_writel(qts, h + 0x7c, 0);
        g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 0);
        g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, 0);
        power_set(qts, false);
        power_set(qts, true);
        g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 0);
        g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, 0);
        /* The programmed source ID selects F's permitted row, not E's. */
        qtest_writel(qts, h + 0x24,
                     (qtest_readl(qts, h + 0x24) & ~15U) | 0xf);
        power_set(qts, false);
        power_set(qts, true);
        g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 1);
        g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==,
                        UINT64_C(1) << 26);
        g_assert_cmphex(qtest_readq(qts, STATUS + 40), ==, 0x10f);
        qtest_writeq(qts, COMPUTE + 0x90, 26);
        qtest_writeq(qts, mask, UINT64_MAX);
        qtest_writel(qts, h + 0x24,
                     (qtest_readl(qts, h + 0x24) & ~15U) | 0xe);
        power_set(qts, false);
        power_set(qts, true);
        g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 2);
        g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==,
                        UINT64_C(1) << 26);
        g_assert_cmphex(qtest_readq(qts, STATUS + 40), ==, 0x10e);
        qtest_quit(qts);
    }
}

static void test_power_vector(void)
{
    QTestState *qts = management_start(POWER_OPTIONS);

    qtest_readq(qts, COMPUTE + 0x60c038);
    qtest_writeq(qts, COMPUTE + 0x60c008, 0);
    qtest_writeq(qts, COMPUTE + 0x60c000, 0x600000);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x60c020) & 7, ==, 0);
    power_set(qts, false);
    qtest_readq(qts, COMPUTE + 0x60c038);
    qtest_writeq(qts, COMPUTE + 0x60c008, 0);
    qtest_writeq(qts, COMPUTE + 0x60c000, 0x600000);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x60c020) & 7, !=, 0);
    qtest_readq(qts, COMPUTE + 0x60c038);
    qtest_writeq(qts, COMPUTE + 0x60c010, UINT64_C(9) << 32);
    qtest_writeq(qts, COMPUTE + 0x60c000, 0x600001);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x60c020) & 7, !=, 1);
    power_set(qts, true);
    g_assert_cmphex((qtest_readq(qts, REQUESTER(0) + BIC_HUB + 0x600000)
                     >> 32) & 0xff, ==, 7);
    qtest_quit(qts);
}

static void power_command(QTestState *qts, unsigned owner,
                           const char *text, int expected)
{
    uint8_t request[128] = { 17, 0x0f, 0xff, 0xf0, 3, 0, 0, 1, 1 };
    uint8_t reply[256];
    size_t length = strlen(text) + 1;

    memcpy(request + 9, text, length);
    sgi_l1_test_send(qts, L1_UART(owner), request, 9 + length, false);
    g_assert_cmpuint(sgi_l1_test_receive(qts, L1_UART(owner), reply,
                                        sizeof(reply)), >=, 6);
    g_assert_cmphex(reply[0], ==, 0x20 | 17);
    g_assert_cmpint((int32_t)ldl_be_p(reply + 1), ==, expected);
    management_empty(qts, 1 - owner);
}

static bool reject_power_lifecycle_event(QTestState *qts, const char *name,
                                         QDict *event, void *opaque)
{
    g_assert_cmpstr(name, !=, "RESET");
    g_assert_cmpstr(name, !=, "SHUTDOWN");
    g_assert_cmpstr(name, !=, "STOP");
    g_assert_cmpstr(name, !=, "RESUME");
    return false;
}

static void test_power_commands(void)
{
    QTestState *qts = management_start(POWER_OPTIONS);

    qtest_qmp_set_event_callback(qts, reject_power_lifecycle_event, NULL);

    power_command(qts, 1, "pwr d", 0);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 56), ==, 0);
    power_command(qts, 0, "ctc pwr u", 0);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 56), ==, 1);
    qtest_writeq(qts, MANAGEMENT + 40, 0);
    power_command(qts, 0, "ctc pwr d", -1);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 56), ==, 1);
    qtest_writeq(qts, MANAGEMENT + 40, 1);
    qtest_writeq(qts, MANAGEMENT + 32, 1);
    power_command(qts, 0, "ctc pwr d", -1);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 48), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 56), ==, 1);
    power_command(qts, 0, "ctc ctc pwr d", -1);
    qtest_quit(qts);
}

static void test_power_views(void)
{
    QTestState *qts = management_start(POWER_OPTIONS);
    uint64_t identity = REQUESTER(0) + NODE(7) + 0x34000000;
    uint64_t latch = REQUESTER(0) + NODE(7) + 0x35000000;

    g_assert_cmphex(qtest_readq(qts, identity), ==, 0xc000);
    result(qts, 0, MEMTX_OK);
    g_assert_cmphex(qtest_readw(qts, latch), ==, 0x1234);
    result(qts, 0, MEMTX_OK);
    power_set(qts, false);
    for (unsigned view = 0; view < 12; view++) {
        qtest_writeq(qts, MANAGEMENT + 0x400 + view * 8, view == 0 ?
                     0x600000 : view >= 3 && view < 9 ? 4 : 0);
        g_assert_cmphex(qtest_readq(qts, STATUS + 72), ==, MEMTX_ACCESS_ERROR);
    }
    qtest_readq(qts, identity);
    result(qts, 0, MEMTX_ACCESS_ERROR);
    qtest_readw(qts, latch);
    result(qts, 0, MEMTX_ACCESS_ERROR);
    qtest_writeq(qts, MANAGEMENT + 48, 1);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 56), ==, 0);
    power_set(qts, true);
    g_assert_cmphex(qtest_readq(qts, identity), ==, 0xc000);
    g_assert_cmphex(qtest_readw(qts, latch), ==, 0x1234);
    for (unsigned head = 0; head < 6; head++) {
        qtest_writeq(qts, MANAGEMENT + 0x400 + (3 + head) * 8, 4);
        g_assert_cmphex(qtest_readq(qts, STATUS + 72), ==, MEMTX_OK);
        g_assert_cmphex(qtest_readq(qts, STATUS + 80), ==, 0x2d002049);
    }
    qtest_quit(qts);
}

static void test_power_initial_off(void)
{
    QTestState *qts = management_start(POWER_OPTIONS
        " -global sgi-pci-expansion-ni-fixture.initial-power=off");

    qtest_readl(qts, head(0, 0) + 4);
    result(qts, 0, MEMTX_ACCESS_ERROR);
    management_query(qts, 0, 0x4ffff006, 19);
    management_identity(qts, 0, 19, 1);
    power_set(qts, true);
    g_assert_cmphex(qtest_readl(qts, head(0, 0) + 4), ==, 0x2d002049);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmpuint(qtest_readq(qts, STATUS + 48), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 56), ==, 0);
    qtest_readl(qts, head(0, 0) + 4);
    result(qts, 0, MEMTX_ACCESS_ERROR);
    qtest_quit(qts);
}

static void test_power_migration(void)
{
    for (unsigned mode = 0; mode < 3; mode++) {
        GError *error = NULL;
        g_autofree char *dir =
            g_dir_make_tmp("ni-power-migrate-XXXXXX", &error);
        g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
        g_autofree char *uri = g_strdup_printf("unix:%s", socket);
        QTestState *from = management_start(POWER_OPTIONS
            " -global sgi-pci-expansion-ni-fixture.intx-map=1");
        QTestState *to = management_start(POWER_OPTIONS
            " -global sgi-pci-expansion-ni-fixture.intx-map=1 -incoming defer");

        g_assert_no_error(error);
        configure_card(from, 0);
        configure_irq_pin(from, 0, 1, 16);
        card_pin(from, 0, 1, 0, true);
        if (mode == 1) {
            power_set(from, false);
        } else if (mode == 2) {
            qtest_writeq(from, MANAGEMENT + 32, 1);
            qtest_writeq(from, MANAGEMENT + 16, 0);
        }
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration_status(to, "completed", 30);
        g_assert_cmpuint(qtest_readq(to, STATUS + 48), ==, mode == 0);
        g_assert_cmpuint(qtest_readq(to, STATUS + 56), ==, mode != 1);
        g_assert_cmpuint(qtest_readq(to, STATUS + 16), ==, 0);
        power_set(to, mode != 1);
        g_assert_cmpuint(qtest_readq(to, STATUS + 16), ==, 0);
        if (mode == 1) {
            power_set(to, true);
            g_assert_cmpuint(qtest_readq(to, STATUS + 16), ==, 1);
        }
        qtest_quit(from);
        qtest_quit(to);
        unlink(socket);
        g_rmdir(dir);
    }
}

static void test_power_migration_mismatch(const void *data)
{
    unsigned kind = GPOINTER_TO_UINT(data);
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("ni-power-mismatch-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = management_start(kind == 2 ? "" : POWER_OPTIONS);
    QTestState *to = management_start(kind == 1 ? "-incoming defer" :
        kind == 2 ? POWER_OPTIONS " -incoming defer" : POWER_OPTIONS
        " -global sgi-pci-expansion-ni-fixture.initial-power=off -incoming defer");

    g_assert_no_error(error);
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

static void adapter_work_write(QTestState *qts, unsigned offset, uint16_t value)
{
    qtest_writeq(qts, MANAGEMENT + 0x600 + offset * 8, value);
}

static void test_power_adapter(void)
{
    QTestState *qts = isp_start(POWER_OPTIONS);
    uint64_t h = head(0, isp_head);
    uint64_t sentinel = UINT64_C(0x0123456789abcdef);

    qtest_writeq(qts, h + 0x10000, NODE(3) | 0x8021 | (0xa << 8));
    configure_irq_pin(qts, isp_head, card_device, 16);
    isp_writew(qts, 0x08, 6);
    isp_word(qts, ISP_RAM_WORD, 0x1357);
    isp_ack(qts);
    qtest_writeq(qts, RAM + 0x8000, sentinel);
    power_set(qts, false);
    adapter_work_write(qts, ISP_MAILBOX(0), 3);
    adapter_work_write(qts, ISP_MAILBOX(1), ISP_RAM_WORD);
    adapter_work_write(qts, ISP_MAILBOX(2), 0x4000);
    adapter_work_write(qts, ISP_MAILBOX(3), 0);
    adapter_work_write(qts, ISP_MAILBOX(4), 1);
    adapter_work_write(qts, ISP_MAILBOX(6), 0);
    adapter_work_write(qts, ISP_MAILBOX(7), 0);
    adapter_work_write(qts, 0xc0, 5 << 12);
    g_assert_cmphex(qtest_readq(qts,
        MANAGEMENT + 0x600 + ISP_MAILBOX(0) * 8), ==, ISP_INTERFACE_ERROR);
    g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), ==, sentinel);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 2);
    power_set(qts, true);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 3);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x98), ==, UINT64_C(1) << 16);
    isp_ack(qts);
    isp_block(qts, 3, ISP_RAM_WORD, 0x40000000, 1, ISP_COMPLETE);
    g_assert_cmphex(qtest_readq(qts, RAM + 0x8000), !=, sentinel);
    qtest_quit(qts);
}

#define UART_OPTIONS POWER_OPTIONS " -global " \
    "sgi-origin300-pci-expansion-module.unconnected-uart-experiment=on"
#define UART_BASE(n) (REQUESTER(n) + BIC_HUB + 0x80)

static void uart_write(QTestState *qts, unsigned reg, uint64_t value)
{
    qtest_writeq(qts, UART_BASE(0) + reg * 8, value);
    result(qts, 0, MEMTX_OK);
}

static uint64_t uart_read(QTestState *qts, unsigned reg)
{
    uint64_t value = qtest_readq(qts, UART_BASE(1) + reg * 8);

    result(qts, 1, MEMTX_OK);
    return value;
}

static void test_uart_registers(void)
{
    QTestState *qts = management_start(UART_OPTIONS);
    QDict *error;

    error = qtest_qmp_assert_failure_ref(qts,
        "{'execute':'qom-set','arguments':{'path':'" PATH "/chassis',"
        "'property':'unconnected-uart-experiment','value':false}}");
    qobject_unref(error);
    error = qtest_qmp_assert_failure_ref(qts,
        "{'execute':'qom-set','arguments':{'path':'" PATH "/chassis',"
        "'property':'uart-origin-count','value':4}}");
    qobject_unref(error);
    error = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'" PATH "/chassis/bic',"
        "'property':'processor-count'}}");
    g_assert_cmpint(qdict_get_int(error, "return"), ==, 0);
    qobject_unref(error);
    error = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'" PATH "/chassis/bic',"
        "'property':'installed-bank-mask'}}");
    g_assert_cmpint(qdict_get_int(error, "return"), ==, 0);
    qobject_unref(error);
    g_assert_cmphex(uart_read(qts, 6), ==, 0xb0);
    g_assert_cmphex(uart_read(qts, 5), ==, 0x60);
    uart_write(qts, 3, 0x80);
    uart_write(qts, 0, UINT64_C(0xfedcba9876543234));
    uart_write(qts, 1, UINT64_C(0x0123456789abcd12));
    g_assert_cmphex(uart_read(qts, 0), ==, 0x34);
    g_assert_cmphex(uart_read(qts, 1), ==, 0x12);
    uart_write(qts, 3, 3);
    uart_write(qts, 7, UINT64_C(0x123456789abcdefa));
    g_assert_cmphex(uart_read(qts, 7), ==, 0xfa);
    uart_write(qts, 2, 0x41);
    uart_write(qts, 4, 0x18);
    uart_write(qts, 0, UINT64_C(0x1122334455667742));
    uart_write(qts, 0, 0x43);
    g_assert_cmphex(uart_read(qts, 0), ==, 0x42);
    g_assert_cmphex(uart_read(qts, 0), ==, 0x43);
    g_assert_cmphex(uart_read(qts, 5) & 1, ==, 0);
    uart_write(qts, 4, 8);
    uart_write(qts, 0, 0x45);
    g_assert_cmphex(uart_read(qts, 5), ==, 0x60);
    management_empty(qts, 0);
    management_empty(qts, 1);
    g_assert_cmpuint(qtest_readq(qts, STATUS + 16), ==, 0);
    qtest_quit(qts);
}

static void test_uart_refusals(void)
{
    QTestState *qts = management_start(POWER_OPTIONS);
    const uint64_t refused[] = { BIC_HUB + 0x78, BIC_HUB + 0xc0,
                                NODE(7) + UINT64_C(0x10010000080) };

    qtest_readq(qts, UART_BASE(0));
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_quit(qts);
    qts = management_start(UART_OPTIONS);
    uart_write(qts, 2, 0x41);
    uart_write(qts, 4, 0x18);
    uart_write(qts, 0, 0x42);
    for (unsigned size = 1; size <= 4; size *= 2) {
        uint8_t bytes[4];

        qtest_memread(qts, UART_BASE(0), bytes, size);
        result(qts, 0, MEMTX_ERROR);
        qtest_memwrite(qts, UART_BASE(0) + 0x38, bytes, size);
        result(qts, 0, MEMTX_ERROR);
    }
    qtest_writeq(qts, MANAGEMENT + 88, BIC_HUB + 0x81);
    qtest_writeq(qts, MANAGEMENT + 104, 8);
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT + 112), ==, MEMTX_ERROR);
    for (unsigned origin = 2; origin < 4; origin++) {
        qtest_writeq(qts, MANAGEMENT + 144, origin);
        qtest_readq(qts, UART_BASE(0));
        result(qts, 0, MEMTX_ACCESS_ERROR);
    }
    qtest_writeq(qts, MANAGEMENT + 144, 0);
    management_cable(qts, true);
    qtest_readq(qts, UART_BASE(0));
    result(qts, 0, MEMTX_DECODE_ERROR);
    management_cable(qts, false);
    g_assert_cmphex(uart_read(qts, 0), ==, 0x42);
    for (unsigned i = 0; i < G_N_ELEMENTS(refused); i++) {
        qtest_readq(qts, REQUESTER(0) + refused[i]);
        result(qts, 0, MEMTX_DECODE_ERROR);
    }
    qtest_readq(qts, REQUESTER(0) + HUB + NODE(8) + 0x80);
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_writeq(qts, MANAGEMENT + 88,
                 (UINT64_C(1) << 47) | (UINT64_C(7) << 39) | 0x30000080);
    for (unsigned source = 0; source < 4; source++) {
        qtest_writeq(qts, MANAGEMENT + 136, source);
        g_assert_cmphex(qtest_readq(qts, MANAGEMENT + 112), !=, MEMTX_OK);
    }
    qtest_writeq(qts, REQUESTER(0) + BIC_HUB + 0x400118, 0);
    result(qts, 0, MEMTX_OK);
    qtest_writeq(qts, MANAGEMENT + 136, 0);
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT + 112), !=, MEMTX_OK);
    uart_write(qts, 7, 0x99);
    power_set(qts, false);
    qtest_writeq(qts, UART_BASE(0) + 0x38, 0x55);
    result(qts, 0, MEMTX_ACCESS_ERROR);
    power_set(qts, true);
    g_assert_cmphex(uart_read(qts, 7), ==, 0);
    qtest_quit(qts);
    qts = management_start(UART_OPTIONS
        " -global sgi-pci-expansion-ni-fixture.connected=off");
    qtest_readq(qts, UART_BASE(0));
    result(qts, 0, MEMTX_DECODE_ERROR);
    qtest_quit(qts);
}

static void test_uart_lifecycle(void)
{
    QTestState *qts = management_start(UART_OPTIONS);
    uint64_t sentinel = UINT64_C(0x123456789abcdef0);

    qtest_writeq(qts, COMPUTE + 0x608000, sentinel);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x608000), ==, sentinel);
    management_query(qts, 0, 0x4ffff006, 19);
    management_query(qts, 1, management_address(0), 7);
    qtest_writeq(qts, MANAGEMENT, 0);
    uart_write(qts, 7, 0x88);
    uart_write(qts, 3, 0x80);
    uart_write(qts, 0, 1);
    uart_write(qts, 3, 3);
    uart_write(qts, 2, 0x41);
    uart_write(qts, 4, 0x18);
    uart_write(qts, 0, 0x42);
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT + 160) & 1, ==, 1);
    qtest_writeq(qts, MANAGEMENT + 152, 0);
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT + 160), ==, 0);
    g_assert_cmphex(uart_read(qts, 7), ==, 0);
    g_assert_cmphex(uart_read(qts, 5), ==, 0x60);
    /* Generic serial reset clears FIFO contents without disabling FIFO mode. */
    uart_write(qts, 3, 3);
    uart_write(qts, 4, 0x18);
    uart_write(qts, 0, 0x42);
    uart_write(qts, 0, 0x43);
    g_assert_cmphex(uart_read(qts, 0), ==, 0x42);
    g_assert_cmphex(uart_read(qts, 5) & 1, ==, 1);
    g_assert_cmphex(uart_read(qts, 0), ==, 0x43);
    g_assert_cmphex(uart_read(qts, 5) & 1, ==, 0);
    uart_write(qts, 3, 0x80);
    g_assert_cmphex(uart_read(qts, 0), ==, 12);
    uart_write(qts, 3, 3);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x608000), ==, sentinel);
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT), ==, 0);
    management_identity(qts, 0, 19, 1);
    uart_write(qts, 4, 0x18);
    uart_write(qts, 0, 0x43);
    power_set(qts, false);
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT + 160), ==, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    qtest_clock_step(qts, INT64_C(10000000000));
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT + 160), ==, 0);
    power_set(qts, true);
    g_assert_cmphex(uart_read(qts, 5), ==, 0x60);
    uart_write(qts, 3, 3);
    uart_write(qts, 4, 0x18);
    uart_write(qts, 0, 0x42);
    uart_write(qts, 0, 0x43);
    g_assert_cmphex(uart_read(qts, 0), ==, 0x42);
    g_assert_cmphex(uart_read(qts, 0), ==, 0x43);
    g_assert_cmphex(uart_read(qts, 5) & 1, ==, 0);
    g_assert_cmphex(qtest_readq(qts, COMPUTE + 0x608000), ==, sentinel);
    g_assert_cmphex(qtest_readq(qts, MANAGEMENT), ==, 0);
    management_identity(qts, 1, 7, 0);
    qtest_quit(qts);
}

static void test_uart_migration(gconstpointer opaque)
{
    bool off = GPOINTER_TO_UINT(opaque);
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("ni-uart-migrate-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = management_start(UART_OPTIONS);
    QTestState *to = management_start(UART_OPTIONS " -incoming defer");

    g_assert_no_error(error);
    uart_write(from, 3, 0x80);
    uart_write(from, 0, 0x34);
    uart_write(from, 1, 0x12);
    uart_write(from, 3, 3);
    uart_write(from, 7, 0xaa);
    uart_write(from, 2, 0x41);
    uart_write(from, 1, 1);
    uart_write(from, 4, 0x18);
    uart_write(from, 0, 0x42);
    g_assert_cmphex(qtest_readq(from, MANAGEMENT + 160), ==, 1);
    if (off) {
        power_set(from, false);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    if (off) {
        g_assert_cmphex(qtest_readq(to, MANAGEMENT + 160), ==, 0);
        power_set(to, true);
        g_assert_cmphex(uart_read(to, 7), ==, 0);
        g_assert_cmphex(uart_read(to, 5), ==, 0x60);
    } else {
        g_assert_cmphex(uart_read(to, 7), ==, 0xaa);
        g_assert_cmphex(uart_read(to, 3), ==, 3);
        g_assert_cmphex(uart_read(to, 4), ==, 0x18);
        g_assert_cmphex(qtest_readq(to, MANAGEMENT + 160), ==, 1);
        uart_write(to, 3, 0x80);
        g_assert_cmphex(uart_read(to, 0), ==, 0x34);
        g_assert_cmphex(uart_read(to, 1), ==, 0x12);
        uart_write(to, 3, 3);
        qtest_qmp_assert_success(to, "{'execute':'cont'}");
        qtest_clock_step(to, INT64_C(10000000000));
        g_assert_cmphex(uart_read(to, 2) & 0xf, ==, 0xc);
        g_assert_cmphex(uart_read(to, 0), ==, 0x42);
        g_assert_cmphex(uart_read(to, 5) & 1, ==, 0);
    }
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_rmdir(dir);
}

static void test_uart_origins(gconstpointer opaque)
{
    unsigned count = GPOINTER_TO_UINT(opaque);
    g_autofree char *options = g_strdup_printf(UART_OPTIONS
        " -global sgi-pci-expansion-ni-fixture.compute-origins=%u", count);
    QTestState *qts = management_start(options);

    uart_write(qts, 7, 0xaa);
    qtest_writeq(qts, MANAGEMENT + 144, count - 1);
    g_assert_cmphex(qtest_readq(qts, UART_BASE(0) + 0x38), ==, 0xaa);
    result(qts, 0, MEMTX_OK);
    qtest_writeq(qts, MANAGEMENT + 144, count);
    qtest_writeq(qts, UART_BASE(0) + 0x38, 0x55);
    result(qts, 0, MEMTX_ACCESS_ERROR);
    g_assert_cmphex(uart_read(qts, 7), ==, 0xaa);
    qtest_quit(qts);
}

static void test_uart_invalid_origins(gconstpointer opaque)
{
    unsigned count = GPOINTER_TO_UINT(opaque);

    if (g_test_subprocess()) {
        g_autofree char *options = g_strdup_printf(UART_OPTIONS
            " -global sgi-pci-expansion-ni-fixture.uart-origins=%u", count);
        QTestState *qts = management_start(options);

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*two or four compute origins*");
}

static void test_uart_migration_mismatch(gconstpointer opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);
    static const char *const expected[] = {
        "*device 'sgi-origin300-pci-expansion-module': 0 != 1*",
        "*device 'sgi-origin300-pci-expansion-module': 1 != 0*",
        "*device 'sgi-origin300-pci-expansion-module': 4 != 2*",
        "*device 'sgi-origin300-pci-expansion-module': 2 != 4*",
    };

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 30 * G_TIME_SPAN_SECOND, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr(expected[kind]);
        return;
    }
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("ni-uart-mismatch-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = management_start(kind == 1 ? POWER_OPTIONS :
        kind == 3 ? UART_OPTIONS
            " -global sgi-pci-expansion-ni-fixture.compute-origins=4" :
        UART_OPTIONS);
    QTestState *to = management_start(kind == 0 ?
        POWER_OPTIONS " -incoming defer" : kind == 2 ? UART_OPTIONS
        " -global sgi-pci-expansion-ni-fixture.compute-origins=4 "
        "-incoming defer" :
        UART_OPTIONS " -incoming defer");

    g_assert_no_error(error);
    qtest_set_expected_status(to, EXIT_FAILURE);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    qtest_wait_qemu(to);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_rmdir(dir);
}

static void test_uart_old_stream(void)
{
    if (g_test_subprocess()) {
        static const char name[] = "sgi-origin300-pci-expansion-module";
        uint8_t header[1 + sizeof(name) - 1 + 8];
        GError *error = NULL;
        g_autofree char *dir = g_dir_make_tmp("ni-uart-v6-XXXXXX", &error);
        g_autofree char *file = g_build_filename(dir, "migration.bin", NULL);
        g_autofree char *uri = g_strdup_printf("file:%s", file);
        g_autofree char *stream = NULL;
        gsize length, found = 0, payload = 0;
        QTestState *from = management_start(POWER_OPTIONS);
        QTestState *to = management_start(UART_OPTIONS
            " -d guest_errors -incoming defer");

        g_assert_no_error(error);
        sgi_migrate_stopped(from, uri);
        g_assert_true(g_file_get_contents(file, &stream, &length, &error));
        g_assert_no_error(error);
        header[0] = sizeof(name) - 1;
        memcpy(header + 1, name, sizeof(name) - 1);
        stl_be_p(header + sizeof(name), 0);
        stl_be_p(header + sizeof(name) + 4, 8);
        for (gsize i = 0; i + sizeof(header) < length; i++) {
            if (!memcmp(stream + i, header, sizeof(header))) {
                payload = i + sizeof(header);
                found++;
            }
        }
        g_assert_cmpuint(found, ==, 1);
        g_assert_cmpuint((uint8_t)stream[payload], ==, 0);
        g_assert_cmpuint(ldl_be_p(stream + payload + 1), ==, 0);
        g_assert_cmpuint((uint8_t)stream[payload + 5], ==, 0);
        g_assert_cmpuint(ldl_be_p(stream + payload + 6), ==, 2);
        stl_be_p(stream + payload - 4, 6);
        memmove(stream + payload, stream + payload + 10, length - payload - 10);
        g_assert_true(g_file_set_contents(file, stream, length - 10, &error));
        g_assert_no_error(error);
        qtest_set_expected_status(to, EXIT_FAILURE);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        qtest_wait_qemu(to);
        qtest_quit(from);
        qtest_quit(to);
        g_assert_cmpint(g_unlink(file), ==, 0);
        g_assert_cmpint(g_rmdir(dir), ==, 0);
        return;
    }
    g_test_trap_subprocess(NULL, 30 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_passed();
    g_test_trap_assert_stderr("*synthetic UART requires migration version 7*");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/pci-expansion-ni/uart/registers", test_uart_registers);
    qtest_add_func("/sgi/pci-expansion-ni/uart/old-stream",
                   test_uart_old_stream);
    qtest_add_data_func("/sgi/pci-expansion-ni/uart/origins/two",
                        GUINT_TO_POINTER(2), test_uart_origins);
    qtest_add_data_func("/sgi/pci-expansion-ni/uart/origins/four",
                        GUINT_TO_POINTER(4), test_uart_origins);
    qtest_add_data_func("/sgi/pci-expansion-ni/uart/origins/zero",
                        GUINT_TO_POINTER(0), test_uart_invalid_origins);
    qtest_add_data_func("/sgi/pci-expansion-ni/uart/origins/three",
                        GUINT_TO_POINTER(3), test_uart_invalid_origins);
    qtest_add_data_func("/sgi/pci-expansion-ni/uart/origins/five",
                        GUINT_TO_POINTER(5), test_uart_invalid_origins);
    qtest_add_data_func("/sgi/pci-expansion-ni/uart/migration-count-two-four",
                        GUINT_TO_POINTER(2), test_uart_migration_mismatch);
    qtest_add_data_func("/sgi/pci-expansion-ni/uart/migration-count-four-two",
                        GUINT_TO_POINTER(3), test_uart_migration_mismatch);
    qtest_add_func("/sgi/pci-expansion-ni/uart/refusals", test_uart_refusals);
    qtest_add_func("/sgi/pci-expansion-ni/uart/lifecycle", test_uart_lifecycle);
    qtest_add_data_func(
        "/sgi/pci-expansion-ni/uart/migration-selected-mismatch",
                        GUINT_TO_POINTER(0), test_uart_migration_mismatch);
    qtest_add_data_func(
        "/sgi/pci-expansion-ni/uart/migration-unselected-mismatch",
                        GUINT_TO_POINTER(1), test_uart_migration_mismatch);
    qtest_add_data_func("/sgi/pci-expansion-ni/uart/migration",
                        GUINT_TO_POINTER(0), test_uart_migration);
    qtest_add_data_func("/sgi/pci-expansion-ni/uart/migration-off",
                        GUINT_TO_POINTER(1), test_uart_migration);
    qtest_add_data_func("/sgi/pci-expansion-ni/pending-registers/two-cpus",
                        GUINT_TO_POINTER(2), test_pending_registers);
    qtest_add_data_func("/sgi/pci-expansion-ni/pending-registers/four-cpus",
                        GUINT_TO_POINTER(4), test_pending_registers);
    qtest_add_data_func("/sgi/pci-expansion-ni/pending-registers/migration",
                        GUINT_TO_POINTER(0), test_pending_migration);
    qtest_add_data_func(
        "/sgi/pci-expansion-ni/pending-registers/selected-mismatch",
                        GUINT_TO_POINTER(1), test_pending_migration);
    qtest_add_data_func(
        "/sgi/pci-expansion-ni/pending-registers/unselected-mismatch",
                        GUINT_TO_POINTER(2), test_pending_migration);
    qtest_add_data_func(
        "/sgi/pci-expansion-ni/pending-registers/count-mismatch",
                        GUINT_TO_POINTER(3), test_pending_migration);
    qtest_add_data_func("/sgi/pci-expansion-ni/pending-registers/migration-off",
                        GUINT_TO_POINTER(4), test_pending_migration);
    qtest_add_func("/sgi/pci-expansion-ni/pending-registers/refusals",
                   test_pending_refusals);
    qtest_add_func("/sgi/pci-expansion-ni/pending-registers/permissions",
                   test_pending_permissions);
    qtest_add_func(
        "/sgi/pci-expansion-ni/pending-registers/child-compatibility",
                   test_pending_child_compatibility);
    qtest_add_func("/sgi/pci-expansion-ni/pending-registers/crossbar",
                   test_pending_crossbar);
    qtest_add_func("/sgi/pci-expansion-ni/processor-absence/read",
                   test_processor_absence);
    qtest_add_func("/sgi/pci-expansion-ni/processor-absence/refusals",
                   test_processor_absence_refusals);
    qtest_add_data_func(
        "/sgi/pci-expansion-ni/processor-absence/old-stream-selected",
                        GUINT_TO_POINTER(1), test_processor_absence_old_stream);
    qtest_add_data_func(
        "/sgi/pci-expansion-ni/processor-absence/old-stream-unselected",
                        GUINT_TO_POINTER(0), test_processor_absence_old_stream);
    qtest_add_func("/sgi/pci-expansion-ni/processor-absence/remap",
                   test_processor_absence_remap);
    qtest_add_data_func("/sgi/pci-expansion-ni/processor-absence/migration",
                        GUINT_TO_POINTER(0), test_processor_absence_migration);
    qtest_add_data_func(
        "/sgi/pci-expansion-ni/processor-absence/migration-selected-mismatch",
                        GUINT_TO_POINTER(1), test_processor_absence_migration);
    qtest_add_data_func(
        "/sgi/pci-expansion-ni/processor-absence/migration-unselected-mismatch",
                        GUINT_TO_POINTER(2), test_processor_absence_migration);
    qtest_add_func("/sgi/pci-expansion-ni/storage/read", test_storage_read);
    qtest_add_func("/sgi/pci-expansion-ni/storage/cpu-carrier",
                   test_storage_cpu_carrier);
    qtest_add_func("/sgi/pci-expansion-ni/storage/unsupported-transfer",
                   test_storage_unsupported_transfer);
    qtest_add_func("/sgi/pci-expansion-ni/storage/late-binding",
                   test_storage_late_binding);
    qtest_add_func("/sgi/pci-expansion-ni/storage/teardown",
                   test_storage_teardown);
    qtest_add_data_func(
        "/sgi/pci-expansion-ni/storage/migration-module-mismatch",
                        GUINT_TO_POINTER(0), test_storage_migration_mismatch);
    qtest_add_data_func(
        "/sgi/pci-expansion-ni/storage/migration-controller-mismatch",
                        GUINT_TO_POINTER(1), test_storage_migration_mismatch);
    qtest_add_data_func(
        "/sgi/pci-expansion-ni/storage/migration-selected-mismatch",
                        GUINT_TO_POINTER(2), test_storage_migration_mismatch);
    qtest_add_data_func(
        "/sgi/pci-expansion-ni/storage/migration-unselected-mismatch",
                        GUINT_TO_POINTER(3), test_storage_migration_mismatch);
    qtest_add_func("/sgi/pci-expansion-ni/storage/program-power-reset",
                   test_storage_program_power_reset);
    qtest_add_func("/sgi/pci-expansion-ni/storage/erase-while-off",
                   test_storage_erase_while_off);
    qtest_add_func("/sgi/pci-expansion-ni/storage/migration-erase-off",
                   test_storage_migration_erase_off);
    qtest_add_func("/sgi/pci-expansion-ni/storage/migration-off",
                   test_storage_migration_off);
    qtest_add_func("/sgi/pci-expansion-ni/power-adapter", test_power_adapter);
    qtest_add_func("/sgi/pci-expansion-ni/power-pio", test_power_pio);
    qtest_add_func("/sgi/pci-expansion-ni/power-owner", test_power_owner);
    qtest_add_func("/sgi/pci-expansion-ni/power-dma-irq", test_power_dma_irq);
    qtest_add_func("/sgi/pci-expansion-ni/power-vector", test_power_vector);
    qtest_add_func("/sgi/pci-expansion-ni/power-bte-completion",
                   test_power_bte_completion);
    qtest_add_func("/sgi/pci-expansion-ni/power-errors", test_power_errors);
    qtest_add_func("/sgi/pci-expansion-ni/power-error-permissions",
                   test_power_error_permissions);
    qtest_add_func("/sgi/pci-expansion-ni/power-commands", test_power_commands);
    qtest_add_func("/sgi/pci-expansion-ni/power-views", test_power_views);
    qtest_add_func("/sgi/pci-expansion-ni/power-initial-off",
                   test_power_initial_off);
    qtest_add_func("/sgi/pci-expansion-ni/power-migration",
                   test_power_migration);
    qtest_add_data_func("/sgi/pci-expansion-ni/power-migration-mismatch",
                        NULL, test_power_migration_mismatch);
    qtest_add_data_func("/sgi/pci-expansion-ni/"
                        "power-migration-selected-to-absent",
                        GUINT_TO_POINTER(1), test_power_migration_mismatch);
    qtest_add_data_func("/sgi/pci-expansion-ni/"
                        "power-migration-absent-to-selected",
                        GUINT_TO_POINTER(2), test_power_migration_mismatch);
    for (unsigned placement = 0; placement < 12; placement++) {
        g_autofree char *name = g_strdup_printf(
            "/sgi/pci-expansion-ni/adapter-placement/head%u-device%u",
            placement / 2, placement % 2 + 1);

        qtest_add_data_func(name, GUINT_TO_POINTER(placement),
                            test_adapter_placement);
    }
    qtest_add_func("/sgi/pci-expansion-ni/dma-device2", test_dma_device2);
    qtest_add_func("/sgi/pci-expansion-ni/isp-dma", test_isp_dma);
    qtest_add_func("/sgi/pci-expansion-ni/isp-dma-denied", test_isp_dma_denied);
    qtest_add_func("/sgi/pci-expansion-ni/isp-interrupt", test_isp_interrupt);
    qtest_add_func("/sgi/pci-expansion-ni/isp-interrupt-denied",
                   test_isp_interrupt_denied);
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
