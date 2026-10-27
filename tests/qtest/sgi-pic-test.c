/* SPDX-License-Identifier: BSD-3-Clause */
/* PIC revision 3 compatibility rules, with synthetic PCI fixture identities. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
#include "sgi-prom-container.h"
#include "qemu/bswap.h"
#include "hw/pci/pci_regs.h"
#include "system/memory.h"
#include <poll.h>
#include <sys/socket.h>

#define PIC "/machine/peripheral/pic"
#define PIC_CHILD "/machine/peripheral/fabric/pic"
#define BASE 0x100000000000ULL
#define CPU_BASE (BASE + 0x100000000ULL)
#define HEAD(n) (BASE + (n) * 0x800000ULL)
#define CPU_HEAD(n) (CPU_BASE + (n) * 0x800000ULL)
#define PIC_HEAD_OFFSET 0x800000ULL
#define TARGET 0x20000000ULL
#define TARGET_NODE1 0x30000000ULL
#define II_REGISTER (1ULL << 47)
#define XIO_WIDGET(n) ((uint64_t)(n) << 48)
#define II_NODE(n) ((uint64_t)(n) << 39)
#define II_HUB_REMOTE 0x1800000ULL
#define INGRESS_REGS 0x50000000ULL
#define SERVING_REGS 0x51000000ULL
#define CARD(n) (BASE + (n ? 0xc0000000ULL : 0x40000000ULL) + 0x01000000)
#define IOC4_CONFIG (HEAD(0) + 0x21000)
#define IOC4_CONFIG2 (HEAD(0) + 0x22000)
#define IOC4_BAR 0x01000000ULL
#define IOC4_BAR2 0x02000000ULL
#define IOC4_MEMORY (HEAD(0) + 0x40000000ULL + IOC4_BAR)
#define IOC4_MEMORY2 (HEAD(0) + 0x40000000ULL + IOC4_BAR2)
#define IOC4_ATA_READBACK 0x148
#define IOC4_UART0 (IOC4_MEMORY + 0x380)
#define IOC4_DMA_BASE 0x300
#define IOC4_DMA_CONTROL 0x310
#define IOC4_DMA_TX_PRODUCE 0x314
#define IOC4_DMA_RX_TIMER 0x324
#define IOC4_DMA_RX_PRODUCE 0x31c
#define IOC4_DMA_RX_CONSUME 0x320
#define IOC4_DMA_ENABLE (1U << 28)
#define IOC4_DMA_DRAIN (1U << 27)
/*
 * A received slot's status: data and modem state valid, with the socket
 * backend's CTS and DCD asserted.
 */
#define IOC4_RX_FROM_LINE 0xf0
#define IOC4_UART_PASS 0x40
#define IOC4_UART_DATA 0x41

static uint32_t pic_ioc4_read_reg(QTestState *qts, unsigned offset);
static QTestState *pic_bedrock_parent_start(bool map_io_registers,
                                            bool runtime_profile);
#define IOC4_IRQ_DESTINATION \
    ((UINT64_C(0x1234) << 48) | II_REGISTER | II_NODE(1) | \
     II_HUB_REMOTE | 0x1000)
#define PIC_SHORT_CFG(n, slot) (HEAD(n) + 0x20000 + (slot) * 0x1000)
#define PIC_SHORT_WIN(n, w) (HEAD(n) + (w == 0 ? 0x200000 : \
    w == 1 ? 0x400000 : w == 2 ? 0x600000 : 0x700000))
#define PIC_SHORT_GUARD (BASE + 0x1000000)
#define PIC_SHORT_DEVICE(n, w) (HEAD(n) + 0x200 + (w) * 8)
#define PIC_SHORT_MEMORY_BASE 0x02000000U

static QTestState *pic_short_start(bool incoming)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S %s"
        "-d unimp,guest_errors "
        "-device sgi-xio-test-target,id=fabric,size=65536 "
        "-device sgi-pic,id=pic,test-fixture=on,"
        "target-0=/machine/peripheral/fabric/fabric.ram[0] "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1 "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=2 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=2",
        incoming ? "-incoming defer " : "");

    qtest_sysbus_mmio_map(qts, PIC, 0, BASE);
    return qts;
}

static void pic_short_enable_card(QTestState *qts, unsigned head,
                                  unsigned slot,
                                  uint32_t bar0, uint32_t bar1,
                                  uint32_t bar2, uint16_t command)
{
    uint64_t cfg = PIC_SHORT_CFG(head, slot);

    qtest_writel(qts, cfg + 4, command);
    qtest_writel(qts, cfg + 0x10, bar0);
    qtest_writel(qts, cfg + 0x14, bar1);
    qtest_writel(qts, cfg + 0x18, bar2);
    g_assert_cmphex(qtest_readl(qts, cfg + 4) & 0xffff, ==, command);
    g_assert_cmphex(qtest_readl(qts, cfg + 0x10), ==, bar0);
    g_assert_cmphex(qtest_readl(qts, cfg + 0x14), ==, bar1);
    g_assert_cmphex(qtest_readl(qts, cfg + 0x18), ==, bar2);
}

static void pic_short_program(QTestState *qts, unsigned head, unsigned window,
                              uint32_t base, bool memory)
{
    qtest_writeq(qts, PIC_SHORT_DEVICE(head, window),
                 base | (memory ? 1U << 12 : 0));
}

static void test_short_device_io_windows(void)
{
    QTestState *qts = pic_short_start(false);
    unsigned head, window;

    for (head = 0; head < 2; head++) {
        pic_short_enable_card(qts, head, 1, PIC_SHORT_MEMORY_BASE,
                              0x04000000, 0x3001,
                              PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        pic_short_enable_card(qts, head, 2, 0x03000000,
                              0x05000000, 0x3009,
                              PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        for (window = 0; window < 4; window++) {
            uint64_t short_base = PIC_SHORT_WIN(head, window);
            unsigned slot = window == 1 || window == 3 ? 2 : 1;
            uint32_t base = slot == 1 ? 0x40 : 0x50;
            uint64_t pci_base = (uint64_t)base << 20;
            uint64_t offset = 8;
            uint64_t value = UINT64_C(0x123456789abcdef0) ^
                             ((uint64_t)head << 40) ^ window;

            pic_short_program(qts, head, window, base, true);
            qtest_writeq(qts, short_base + offset, value);
            g_assert_cmphex(qtest_readq(qts, BASE + (head ? 0xc0000000ULL :
                                    0x40000000ULL) + pci_base + offset), ==,
                            value);
            g_assert_cmphex(qtest_readq(qts, short_base + offset), ==, value);
        }

        /* A window follows its programmed base to a different PCI function. */
        pic_short_program(qts, head, 0, 0x40, true);
        qtest_writeq(qts, PIC_SHORT_WIN(head, 0) + 8, 0x13579bdf2468ace0ULL);
        pic_short_program(qts, head, 0, 0x50, true);
        qtest_writeq(qts, PIC_SHORT_WIN(head, 0) + 8, 0xfedcba9876543210ULL);
        pic_short_program(qts, head, 1, 0x50, true);
        g_assert_cmphex(qtest_readq(qts, PIC_SHORT_WIN(head, 1) + 8), ==,
                        0xfedcba9876543210ULL);
    }

    pic_short_program(qts, 0, 2, 0x40, true);
    qtest_writeb(qts, PIC_SHORT_WIN(0, 2) + 8, 0x3d);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x40000000ULL + 0x04000008), ==,
                    0x3d);
    qtest_writew(qts, PIC_SHORT_WIN(0, 2) + 8, 0x5a2c);
    g_assert_cmphex(qtest_readw(qts, BASE + 0x40000000ULL + 0x04000008), ==,
                    0x5a2c);
    qtest_writel(qts, PIC_SHORT_WIN(0, 2) + 8, 0x19e3b46a);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x40000000ULL + 0x04000008), ==,
                    0x19e3b46a);
    qtest_writeq(qts, PIC_SHORT_WIN(0, 2) + 8, 0x23a91d5ce047b682ULL);
    g_assert_cmphex(qtest_readq(qts, BASE + 0x40000000ULL + 0x04000008), ==,
                    0x23a91d5ce047b682ULL);

    /* Odd 2 MiB bases align down; the upper MiB remains an address offset. */
    for (head = 0; head < 2; head++) {
        pic_short_enable_card(qts, head, 1, 0x02000000, 0x04000000,
                              0x3001, PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        for (window = 0; window < 2; window++) {
            uint64_t bar = BASE + (head ? 0xc0000000ULL : 0x40000000ULL) +
                           0x02100000;
            uint64_t short_addr = PIC_SHORT_WIN(head, window) + 0x100024;

            pic_short_enable_card(qts, head, 1, 0x02100000, 0x04000000,
                                  0x3001, PCI_COMMAND_MEMORY |
                                  PCI_COMMAND_IO);
            pic_short_program(qts, head, window, 0x21, true);
            qtest_writel(qts, short_addr, 0x96b341e7 + window);
            g_assert_cmphex(qtest_readl(qts, bar + 0x24), ==,
                            0x96b341e7 + window);
        }
    }

    /* Distinct responders at the same numeric address separate both spaces. */
    pic_short_enable_card(qts, 0, 1, 0x3000, 0x04000000, 0x3001,
                          PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
    pic_short_program(qts, 0, 2, 0, true);
    pic_short_program(qts, 0, 3, 0, false);
    qtest_writel(qts, PIC_SHORT_WIN(0, 2) + 0x3000, 0x10293847);
    qtest_writeq(qts, PIC_SHORT_WIN(0, 3) + 0x3000, 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readl(qts, PIC_SHORT_WIN(0, 2) + 0x3000), ==,
                    0x10293847);
    g_assert_cmphex(qtest_readq(qts, PIC_SHORT_WIN(0, 3) + 0x3000), ==,
                    0xfedcba9876543210ULL);

    /* Memory discards bits 11:10; I/O preserves them. */
    pic_short_enable_card(qts, 0, 1, 0x02000000, 0x04000000,
                          0x3001, PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
    pic_short_program(qts, 0, 2, 0x820, true);
    qtest_writel(qts, PIC_SHORT_WIN(0, 2) + 0x24, 0x1a2b3c4d);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x40000000ULL + 0x02000024), ==,
                    0x1a2b3c4d);
    pic_short_enable_card(qts, 0, 1, 0x3000, 0x04000000,
                          0x82000021, PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
    pic_short_program(qts, 0, 3, 0x820, false);
    qtest_writel(qts, PIC_SHORT_WIN(0, 3) + 0x24, 0xa15c39e7);
    g_assert_cmphex(qtest_readl(qts, PIC_SHORT_WIN(0, 3) + 0x24), ==,
                    0xa15c39e7);

    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(qts, PIC_SHORT_DEVICE(0, 2)), ==, 0);
    g_assert_cmphex(qtest_readq(qts, PIC_SHORT_DEVICE(1, 3)), ==, 0);

    qtest_quit(qts);
}

static void test_short_device_io_attributes_and_boundaries(void)
{
    QTestState *qts = pic_short_start(false);

    for (unsigned head = 0; head < 2; head++) {
        uint64_t mem_alias = BASE + (head ? 0xc0000000ULL : 0x40000000ULL) +
                             0x02100024;

        pic_short_enable_card(qts, head, 1, 0x02100000, 0x04000000,
                              0xffff9, PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        pic_short_program(qts, head, 2, 0x21, true);
        qtest_writel(qts, PIC_SHORT_WIN(head, 2) + 0x24,
                     0x7e3a91c5 + head);

        for (unsigned attr = 13; attr <= 22; attr += 9) {
            qtest_writel(qts, mem_alias, 0x13579bdf + head);
            qtest_writeq(qts, PIC_SHORT_DEVICE(head, 2),
                         0x21 | (1U << 12) | (UINT64_C(1) << attr));
            qtest_writel(qts, PIC_SHORT_WIN(head, 2) + 0x24,
                         0xa1b2c3d4);
            g_assert_cmphex(qtest_readl(qts, mem_alias), ==,
                            0x13579bdf + head);
        }

        pic_short_program(qts, head, 2, 0x21, true);
        qtest_writel(qts, mem_alias, 0x13579bdf + head);
        qtest_writel(qts, PIC_SHORT_CFG(head, 1) + 4, PCI_COMMAND_IO);
        g_assert_cmphex(qtest_readl(qts, PIC_SHORT_CFG(head, 1) + 4) & 0xffff,
                        ==, PCI_COMMAND_IO);
        qtest_writel(qts, PIC_SHORT_WIN(head, 2) + 0x24, 0xa1b2c3d4);
        qtest_writel(qts, PIC_SHORT_CFG(head, 1) + 4,
                     PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        g_assert_cmphex(qtest_readl(qts, PIC_SHORT_CFG(head, 1) + 4) & 0xffff,
                        ==, PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        g_assert_cmphex(qtest_readl(qts, mem_alias), ==, 0x13579bdf + head);
        qtest_writel(qts, PIC_SHORT_WIN(head, 2) + 0x24, 0xa1b2c3d4);
        g_assert_cmphex(qtest_readl(qts, mem_alias), ==, 0xa1b2c3d4);

        pic_short_program(qts, head, 2, 0, false);
        qtest_writeq(qts, PIC_SHORT_WIN(head, 2) + 0xffff8,
                     0x8172635445362718ULL + head);
        qtest_writel(qts, PIC_SHORT_CFG(head, 1) + 4, PCI_COMMAND_MEMORY);
        qtest_writeq(qts, PIC_SHORT_WIN(head, 2) + 0xffff8,
                     0xfedcba9876543210ULL);
        qtest_writel(qts, PIC_SHORT_CFG(head, 1) + 4,
                     PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        g_assert_cmphex(qtest_readq(qts, PIC_SHORT_WIN(head, 2) + 0xffff8),
                        ==, 0x8172635445362718ULL + head);

        /* Four bytes of the crossing access overlap this BAR's wide scratch. */
        pic_short_enable_card(qts, head, 1, 0x02100000, 0x020ffff0,
                              0xffff9, PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        qtest_writeq(qts, BASE + (head ? 0xc0000000ULL : 0x40000000ULL) +
                     0x020ffff8, 0x193a5c7e80b2d4f6ULL + head);
        pic_short_program(qts, head, 3, 0x20, true);
        qtest_writeq(qts, HEAD(head) + 0x7ffffc,
                     0xfedcba9876543210ULL - head);
        g_assert_cmphex(qtest_readq(qts,
                        BASE + (head ? 0xc0000000ULL : 0x40000000ULL) +
                        0x020ffff8), ==, 0x193a5c7e80b2d4f6ULL + head);
    }

    qtest_writeq(qts, PIC_SHORT_GUARD, 0xdeadbeef12345678ULL);
    g_assert_cmphex(qtest_readq(qts, PIC_SHORT_GUARD), ==, 0);
    g_assert_cmphex(qtest_readq(qts, PIC_SHORT_DEVICE(1, 3)), ==,
                    0x20 | (1U << 12));

    /* A doubleword straddling the 1 MiB edge must not touch the BAR. */
    pic_short_program(qts, 0, 2, 0, false);
    qtest_writeq(qts, PIC_SHORT_WIN(0, 2) + 0xffff8, 0x8172635445362718ULL);
    qtest_writeq(qts, PIC_SHORT_WIN(0, 2) + 0xffffc, 0xdeadbeef12345678ULL);
    g_assert_cmphex(qtest_readq(qts, PIC_SHORT_WIN(0, 2) + 0xffff8), ==,
                    0x8172635445362718ULL);
    g_assert_cmphex(qtest_readq(qts, PIC_SHORT_WIN(0, 2) + 0xffffc), ==,
                    0);
    qtest_quit(qts);
}

static void test_short_device_io_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-short-migration-XXXXXX",
                                           NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = pic_short_start(false);
    QTestState *to = pic_short_start(true);

    for (unsigned head = 0; head < 2; head++) {
        pic_short_enable_card(from, head, 1, 0x02000000, 0x04000000,
                              0x3001, PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        pic_short_enable_card(to, head, 1, 0x02000000, 0x04000000,
                              0x3001, PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        pic_short_program(from, head, head, 0x40, true);
        qtest_writeq(from, PIC_SHORT_WIN(head, head) + 8,
                     0x0192a3b4c5d6e7f8ULL + head);
    }

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);

    for (unsigned head = 0; head < 2; head++) {
        g_assert_cmphex(qtest_readq(to, PIC_SHORT_WIN(head, head) + 8), ==,
                        0x0192a3b4c5d6e7f8ULL + head);
        qtest_writeq(to, PIC_SHORT_WIN(head, head) + 8,
                     0xfedcba9876543210ULL - head);
        g_assert_cmphex(qtest_readq(to, PIC_SHORT_WIN(head, head) + 8), ==,
                        0xfedcba9876543210ULL - head);
    }

    qtest_quit(to);
    qtest_quit(from);
    /* Incoming migration cleanup may already have removed its listener. */
    if (unlink(socket) < 0) {
        g_assert_cmpint(errno, ==, ENOENT);
    }
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static uint32_t pic_requests_at(QTestState *qts, const char *base,
                                unsigned head, unsigned pin, bool set)
{
    g_autofree char *path = g_strdup_printf("%s/head-%u", base, head);
    g_autofree char *property = g_strdup_printf("x-test-%s-requests-%u",
                                                set ? "set" : "clear", pin);
    QDict *reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, property);
    uint32_t count;

    g_assert_true(qdict_haskey(reply, "return"));
    count = qdict_get_int(reply, "return");

    qobject_unref(reply);
    return count;
}

static uint32_t pic_requests(QTestState *qts, unsigned head, unsigned pin,
                             bool set)
{
    return pic_requests_at(qts, PIC, head, pin, set);
}

static uint64_t target_metric(QTestState *qts, const char *property)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/fabric',"
        "'property':%s}}", property);

    if (!qdict_haskey(reply, "return")) {
        g_autoptr(GString) text = qobject_to_json_pretty(QOBJECT(reply), true);

        g_test_message("qom-get %s failed: %s", property, text->str);
        g_assert_true(qdict_haskey(reply, "return"));
    }
    return qdict_get_int(reply, "return");
}

static uint64_t pic_metric(QTestState *qts, const char *property)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        PIC_CHILD, property);

    if (!qdict_haskey(reply, "return")) {
        g_autoptr(GString) text = qobject_to_json_pretty(QOBJECT(reply), true);

        g_test_message("qom-get %s failed: %s", property, text->str);
        g_assert_true(qdict_haskey(reply, "return"));
    }
    return qdict_get_int(reply, "return");
}

static uint64_t qom_uint64(QTestState *qts, const char *path,
                           const char *property)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, property);

    if (!qdict_haskey(reply, "return")) {
        g_autoptr(GString) text =
            qobject_to_json_pretty(QOBJECT(reply), true);

        g_test_message("qom-get %s failed: %s", property, text->str);
        g_assert_true(qdict_haskey(reply, "return"));
    }
    return qdict_get_int(reply, "return");
}

static QTestState *pic_start(bool cards, const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors "
        "-device sgi-xio-test-target,id=fabric,size=65536 "
        "-device sgi-pic,id=pic,test-fixture=on,"
        "target-0=/machine/peripheral/fabric/fabric.ram[0] %s %s",
        cards ?
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1,"
        "vendor-id=0xfeed,device-id=0x3500 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1,"
        "vendor-id=0xfeed,device-id=0x3501" : "", extra);

    qtest_sysbus_mmio_map(qts, PIC, 0, BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    return qts;
}

static QTestState *pic_migration_version_start(bool legacy_source,
                                                bool incoming,
                                                bool stale_bedrock_mode)
{
    const char *pic_type = legacy_source ? "sgi-pic-v1-test" : "sgi-pic";
    const char *incoming_arg = incoming ? "-incoming defer " : "";
    const char *stale_arg = stale_bedrock_mode ?
        ",x-test-migration-bedrock-interrupts=on" : "";
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S %s"
        "-device sgi-xio-test-target,id=fabric,size=65536 "
        "-device %s,id=pic,test-fixture=on,"
        "target-0=/machine/peripheral/fabric/fabric.ram[0]%s",
        incoming_arg, pic_type, stale_arg);

    qtest_sysbus_mmio_map(qts, PIC, 0, BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    return qts;
}

static QTestState *pic_linked_start(bool cards, const char *fixture_options,
                                    const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors "
        "-device sgi-xio-test-target,id=fabric,size=16777216,pic-peer=on%s %s "
        "%s",
        fixture_options ? fixture_options : "",
        cards ?
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1,"
        "vendor-id=0xfeed,device-id=0x3500 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1,"
        "vendor-id=0xfeed,device-id=0x3501" : "",
        extra ? extra : "");

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    return qts;
}

static QTestState *pic_runtime_start(bool cards, const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors "
        "-device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,"
        "pic-runtime-profile=on,pic-linked=off,pic-bedrock-interrupts=off %s "
        "%s",
        cards ?
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1,"
        "vendor-id=0xfeed,device-id=0x3500 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1,"
        "vendor-id=0xfeed,device-id=0x3501" : "",
        extra ? extra : "");

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    return qts;
}

static QTestState *pic_runtime_cpu_start(bool cards, const char *extra)
{
    QTestState *qts = pic_runtime_start(cards, extra);

    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);
    return qts;
}

static void test_runtime_short_device_attribute_refusal(void)
{
    QTestState *qts = pic_runtime_start(true, NULL);
    const uint64_t device = 0x21 | (1U << 12);
    const uint64_t bar = BASE + 0x40000000ULL + 0x02100024;
    const uint64_t short_addr = PIC_SHORT_WIN(0, 2) + 0x24;

    pic_short_enable_card(qts, 0, 1, 0x02100000, 0x04000000,
                          0xffff9, PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
    qtest_writeq(qts, PIC_SHORT_DEVICE(0, 2), device);
    qtest_writel(qts, short_addr, 0x13579bdf);
    g_assert_cmphex(qtest_readl(qts, bar), ==, 0x13579bdf);

    qtest_writeq(qts, PIC_SHORT_DEVICE(0, 2), device | (1U << 13));
    g_assert_cmphex(qtest_readq(qts, PIC_SHORT_DEVICE(0, 2)), ==,
                    device | (1U << 13));
    qtest_writel(qts, short_addr, 0xa1b2c3d4);
    g_assert_cmphex(qtest_readl(qts, bar), ==, 0x13579bdf);
    qtest_quit(qts);
}

static void test_runtime_reset_compatibility_trial(void)
{
    static const uint64_t device_reset[] = {
        0x18001000, 0x18001002, 0x18001004, 0x18001005,
    };
    QTestState *qts = pic_runtime_cpu_start(false, NULL);
    QTestState *fixture = pic_short_start(false);
    unsigned head, i;

    for (head = 0; head < 2; head++) {
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x20), ==,
                        0x0f00320f);
        for (i = 0; i < G_N_ELEMENTS(device_reset); i++) {
            g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x200 + i * 8),
                            ==, device_reset[i]);
        }
    }

    qtest_writel(qts, CPU_HEAD(0) + 0x24, 0x0f00420f);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(0) + 0x24), ==,
                    0x0f00420f);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(1) + 0x24), ==,
                    0x0f00320f);

    qtest_writeq(qts, CPU_HEAD(1) + 0x210, 0x18581007);
    g_assert_cmphex(qtest_readq(qts, CPU_HEAD(1) + 0x210), ==,
                    0x18581007);

    for (head = 0; head < 2; head++) {
        qtest_writel(qts, CPU_HEAD(head) + 0x24, 0x0f00420f + head);
        for (i = 0; i < G_N_ELEMENTS(device_reset); i++) {
            qtest_writeq(qts, CPU_HEAD(head) + 0x200 + i * 8,
                         0xdead0000 + head * 0x100 + i);
        }
    }
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    for (head = 0; head < 2; head++) {
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x20), ==,
                        0x0f00320f);
        for (i = 0; i < G_N_ELEMENTS(device_reset); i++) {
            g_assert_cmphex(qtest_readq(qts,
                                       CPU_HEAD(head) + 0x200 + i * 8),
                            ==, device_reset[i]);
        }
    }

    for (head = 0; head < 2; head++) {
        g_assert_cmphex(qtest_readq(fixture, HEAD(head) + 0x20), ==, 0);
        for (i = 0; i < G_N_ELEMENTS(device_reset); i++) {
            g_assert_cmphex(qtest_readq(fixture,
                                       HEAD(head) + 0x200 + i * 8), ==, 0);
        }
    }
    qtest_writel(fixture, HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(fixture, HEAD(0) + 0x104), ==, 0);

    qtest_quit(fixture);
    qtest_quit(qts);
}

static char *pic_blank_backend(size_t size, const char *template)
{
    GError *error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp(template, &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *pic_tezro_runtime_start(bool incoming, char **bios_path,
                                          char **io9_data_path,
                                          char **io9_clock_path)
{
    const char *incoming_arg = incoming ? "-incoming defer" : "";
    /* Machine validation requires a full-size PROM, but qtest never runs it. */
    g_autofree uint8_t *payload = g_malloc0(1476264);
    char *io9_data = pic_blank_backend(
        0x8000, "sgi-pic-tezro-data-XXXXXX");
    char *io9_clock = pic_blank_backend(
        16, "sgi-pic-tezro-clock-XXXXXX");
    char *bios = sgi_prom_container_write(
        payload, 1476264, "sgi-pic-tezro-XXXXXX.img");
    g_autofree char *io9_drives = g_strdup_printf(
        "-drive if=none,id=sgi-ds1742-data-io9,file=%s,format=raw "
        "-drive if=none,id=sgi-ds1742-clock-io9,file=%s,format=raw",
        io9_data, io9_clock);
    QTestState *qts = qtest_initf(
        "-M tezro,topology=tezro-tower-multi,nodes=1,"
        "ip53-experimental=on,nvram0=sgi-ds1742-data-io9,"
        "nvram0-clock=sgi-ds1742-clock-io9 %s -bios %s %s -S",
        incoming_arg, bios, io9_drives);

    *bios_path = bios;
    *io9_data_path = io9_data;
    *io9_clock_path = io9_clock;
    qtest_sysbus_mmio_map(qts, "/machine/pic-pic-xio-b", 0, CPU_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/pic-pic-xio-f", 0,
                          CPU_BASE + 0x10000000);
    return qts;
}

static void assert_tezro_pic_profiles(QTestState *qts)
{
    static const char *const pic_path[] = {
        "/machine/pic-pic-xio-b", "/machine/pic-pic-xio-f",
    };

    for (unsigned host = 0; host < G_N_ELEMENTS(pic_path); host++) {
        g_autoptr(QDict) profile = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':{'path':%s,"
            "'property':'compatibility-profile'}}", pic_path[host]);

        g_assert_cmpstr(qdict_get_str(profile, "return"), ==,
                        "pic-rev3-bedrock-tezro-experimental");
    }
}

static void assert_tezro_pic_reset(QTestState *qts)
{
    static const char *const pic_path[] = {
        "/machine/pic-pic-xio-b", "/machine/pic-pic-xio-f",
    };
    static const uint64_t host_base[] = {
        CPU_BASE, CPU_BASE + 0x10000000,
    };

    for (unsigned host = 0; host < G_N_ELEMENTS(pic_path); host++) {
        for (unsigned head = 0; head < 2; head++) {
            uint64_t base = host_base[host] + head * PIC_HEAD_OFFSET;

            g_assert_cmphex(qtest_readq(qts, base + 0x28), ==, 0xfffff);
            g_assert_cmphex(qtest_readq(qts, base + 0x30), ==, 0);
            g_assert_cmphex(qtest_readq(qts, base + 0x38), ==, 0);
            g_assert_cmphex(qtest_readq(qts, base + 0x100), ==, 0);
            g_assert_cmphex(qtest_readq(qts, base + 0x128), ==, 0);
        }
        g_assert_cmpuint(qom_uint64(qts, pic_path[host],
                                   "x-test-linked-packet-count"), ==, 0);
    }
}

static void test_tezro_widget_register_profile_trial(void)
{
    static const char *const pic_path[] = {
        "/machine/pic-pic-xio-b", "/machine/pic-pic-xio-f",
    };
    static const uint32_t timeout[] = { 0x00123456, 0x00654321 };
    static const uint32_t upper[] = { 0x11223344, 0x55667788 };
    static const uint32_t lower[] = { 0x99aabbcc, 0xddeeff00 };
    static const uint64_t host_base[] = {
        CPU_BASE, CPU_BASE + 0x10000000,
    };
    g_autofree char *from_bios = NULL;
    g_autofree char *from_data = NULL;
    g_autofree char *from_clock = NULL;
    g_autofree char *to_bios = NULL;
    g_autofree char *to_data = NULL;
    g_autofree char *to_clock = NULL;
    QTestState *qts = pic_tezro_runtime_start(false, &from_bios, &from_data,
                                               &from_clock);

    assert_tezro_pic_profiles(qts);
    assert_tezro_pic_reset(qts);

    for (unsigned host = 0; host < G_N_ELEMENTS(pic_path); host++) {
        for (unsigned head = 0; head < 2; head++) {
            uint64_t base = host_base[host] + head * PIC_HEAD_OFFSET;
            uint32_t upper_word = 0x12000000 | (host << 16) | (head + 1);
            uint32_t lower_word = 0x34000000 | (host << 8) | head;

            qtest_writel(qts, base + 0x2c, 0x54321 + host * 2 + head);
            qtest_writel(qts, base + 0x34, upper_word);
            qtest_writel(qts, base + 0x3c, lower_word);
            g_assert_cmphex(qtest_readq(qts, base + 0x28), ==,
                            0x54321 + host * 2 + head);
            g_assert_cmphex(qtest_readq(qts, base + 0x30), ==, upper_word);
            g_assert_cmphex(qtest_readq(qts, base + 0x38), ==, lower_word);
            g_assert_cmpuint(qom_uint64(qts, pic_path[host],
                                       "x-test-linked-packet-count"), ==, 0);
        }
    }

    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    assert_tezro_pic_reset(qts);

    for (unsigned host = 0; host < G_N_ELEMENTS(pic_path); host++) {
        for (unsigned head = 0; head < 2; head++) {
            unsigned index = host * 2 + head;
            uint64_t base = host_base[host] + head * PIC_HEAD_OFFSET;
            uint32_t timeout_value = 0x50000 + index;
            uint32_t upper_word = 0x34000000 | (index + 1);
            uint32_t lower_word = 0x3000 + index * 0x1000;

            qtest_writel(qts, base + 0x2c, timeout_value);
            qtest_writel(qts, base + 0x34, upper_word);
            qtest_writel(qts, base + 0x3c, lower_word);
            qtest_writeq(qts, base + 0x128, index);
            qtest_writel(qts, base + 0x7c, 0xdeadbeef);
            g_assert_cmphex(qtest_readq(qts, base + 0x28), ==, timeout_value);
            g_assert_cmphex(qtest_readq(qts, base + 0x30), ==, upper_word);
            g_assert_cmphex(qtest_readq(qts, base + 0x38), ==, lower_word);
            g_assert_cmphex(qtest_readq(qts, base + 0x128), ==, index);
            g_assert_cmphex(qtest_readl(qts, base + 0x104), ==, 1U << 24);
        }
        g_assert_cmpuint(qom_uint64(qts, pic_path[host],
                                   "x-test-linked-packet-count"), ==, 0);
    }

    g_autofree char *dir = g_dir_make_tmp("sgi-pic-tezro-migration-XXXXXX",
                                          NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *to = pic_tezro_runtime_start(true, &to_bios, &to_data,
                                              &to_clock);

    assert_tezro_pic_profiles(to);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(qts, uri);
    sgi_wait_migration_status(to, "completed", 30);

    for (unsigned host = 0; host < G_N_ELEMENTS(pic_path); host++) {
        for (unsigned head = 0; head < 2; head++) {
            unsigned index = host * 2 + head;
            uint64_t base = host_base[host] + head * PIC_HEAD_OFFSET;

            g_assert_cmphex(qtest_readq(to, base + 0x28), ==,
                            0x50000 + index);
            g_assert_cmphex(qtest_readq(to, base + 0x30), ==,
                            0x34000000 | (index + 1));
            g_assert_cmphex(qtest_readq(to, base + 0x38), ==,
                            0x3000 + index * 0x1000);
            g_assert_cmphex(qtest_readq(to, base + 0x128), ==, index);
            g_assert_cmphex(qtest_readl(to, base + 0x104), ==, 1U << 24);
        }
        g_assert_cmpuint(qom_uint64(to, pic_path[host],
                                   "x-test-linked-packet-count"), ==, 0);
    }
    qtest_quit(qts);
    qtest_quit(to);
    unlink(socket);
    unlink(from_bios);
    unlink(from_data);
    unlink(from_clock);
    unlink(to_bios);
    unlink(to_data);
    unlink(to_clock);
    g_assert_cmpint(rmdir(dir), ==, 0);

    qts = pic_short_start(false);
    for (unsigned head = 0; head < 2; head++) {
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x28), ==, 0);
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x30), ==, 0);
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x38), ==, 0);
        qtest_writeq(qts, HEAD(head) + 0x28, timeout[head]);
        qtest_writeq(qts, HEAD(head) + 0x30, upper[head]);
        qtest_writeq(qts, HEAD(head) + 0x38, lower[head]);
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x28), ==, 0);
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x30), ==, 0);
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x38), ==, 0);
    }
    qtest_quit(qts);
}

static QTestState *pic_runtime_error_start(bool packet_failure)
{
    const char *failure = packet_failure ? ",packet-failure=on" : "";
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S -d unimp,guest_errors "
        "-device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,"
        "pic-runtime-profile=on,pic-linked=off,pic-bedrock-interrupts=off,"
        "packet-window-base=0x100000000%s", failure);

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);
    return qts;
}

static void test_runtime_invalid_address_error_trial(void)
{
    const uint64_t invalid_address = UINT64_C(1) << 24;
    const uint64_t destination = UINT64_C(0x100003000);
    uint8_t sentinel[8];
    uint8_t expected[8] = { 0 };
    uint8_t packet[8];
    QTestState *qts = pic_runtime_error_start(false);
    QTestState *fixture = pic_short_start(false);
    QTestState *bedrock;

    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(0) + 0x7c), ==, 0);
    qtest_writeb(qts, CPU_HEAD(0) + 0x7c, 0x11);
    qtest_writew(qts, CPU_HEAD(0) + 0x7c, 0x2233);
    qtest_writel(qts, CPU_HEAD(0) + 0x78, 0x44556677);
    qtest_writeq(qts, CPU_HEAD(0) + 0x78, 0x8899aabbccddeeffULL);
    qtest_writeq(qts, CPU_HEAD(0) + 0x7c, 0x1122334455667788ULL);
    g_assert_cmphex(qtest_readq(qts, CPU_HEAD(0) + 0x100), ==, 0);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 0);

    g_assert_cmphex(qtest_readl(fixture, HEAD(0) + 0x7c), ==, 0);
    qtest_writeb(fixture, HEAD(0) + 0x7c, 0x11);
    qtest_writew(fixture, HEAD(0) + 0x7c, 0x2233);
    qtest_writel(fixture, HEAD(0) + 0x78, 0x44556677);
    qtest_writeq(fixture, HEAD(0) + 0x78, 0x8899aabbccddeeffULL);
    qtest_writeq(fixture, HEAD(0) + 0x7c, 0x1122334455667788ULL);
    g_assert_cmphex(qtest_readq(fixture, HEAD(0) + 0x100), ==, 0);

    memset(sentinel, 0xa5, sizeof(sentinel));
    qtest_memwrite(qts, TARGET + 0x3000, sentinel, sizeof(sentinel));
    qtest_writel(qts, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(0) + 0x104), ==,
                    invalid_address);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(1) + 0x104), ==, 0);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 0);
    qtest_memread(qts, TARGET + 0x3000, packet, sizeof(packet));
    g_assert_cmpmem(packet, sizeof(packet), sentinel, sizeof(sentinel));

    qtest_writel(qts, CPU_HEAD(0) + 0x114, invalid_address);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(0) + 0x104), ==, 0);

    qtest_writeq(qts, HEAD(0) + 0x30, UINT64_C(0x34000000));
    qtest_writeq(qts, HEAD(0) + 0x38, destination);
    qtest_writel(qts, CPU_HEAD(0) + 0x10c, invalid_address);

    qtest_writeq(qts, HEAD(0) + 0x128, 0x80);
    qtest_writel(qts, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(0) + 0x104), ==,
                    invalid_address);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(1) + 0x104), ==, 0);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 0);
    qtest_writeq(qts, HEAD(0) + 0x128, 0x100);
    qtest_writel(qts, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(0) + 0x104), ==,
                    invalid_address);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 0);

    qtest_writeq(qts, HEAD(0) + 0x128, 0x65);
    qtest_writel(qts, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 1);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-size"), ==, 8);
    g_assert_cmphex(target_metric(qts, "x-test-packet-address"), ==,
                    destination);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 15);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-device-known"), ==, 0);
    expected[6] = 1;
    expected[7] = 0x65;
    qtest_memread(qts, TARGET + 0x3000, packet, sizeof(packet));
    g_assert_cmpmem(packet, sizeof(packet), expected, sizeof(expected));

    qtest_writel(qts, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 2);

    /* A matching duplicate of the lower high address field is accepted. */
    qtest_writeq(qts, HEAD(0) + 0x30, UINT64_C(0x00000001));
    qtest_writeq(qts, HEAD(0) + 0x38, destination);
    qtest_writel(qts, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 3);

    qtest_writeq(qts, HEAD(0) + 0x30, UINT64_C(0x00000002));
    qtest_writel(qts, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 3);

    qtest_writeq(qts, HEAD(0) + 0x30, UINT64_C(0x34000000));
    qtest_writeq(qts, HEAD(0) + 0x38, 0);
    qtest_writel(qts, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 3);

    qtest_writeq(qts, HEAD(0) + 0x30, UINT64_C(0x0000000100000001));
    qtest_writeq(qts, HEAD(0) + 0x38, destination);
    qtest_writel(qts, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 3);

    qtest_writeq(qts, HEAD(0) + 0x30, UINT64_C(0x34000000));
    qtest_writeq(qts, HEAD(0) + 0x38, UINT64_C(0x000f000100003000));
    qtest_writel(qts, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 3);

    qtest_writel(qts, CPU_HEAD(0) + 0x114, invalid_address);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(0) + 0x104), ==, 0);
    qtest_writeq(qts, HEAD(0) + 0x38, destination);
    qtest_writeq(qts, HEAD(0) + 0x30, UINT64_C(0x34000000));
    qtest_writel(qts, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 4);

    qtest_writel(qts, CPU_HEAD(1) + 0x7c, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(0) + 0x104), ==,
                    invalid_address);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(1) + 0x104), ==,
                    invalid_address);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 4);
    qtest_writel(qts, CPU_HEAD(1) + 0x114, invalid_address);
    qtest_writel(qts, CPU_HEAD(0) + 0x114, invalid_address);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(0) + 0x104), ==, 0);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(1) + 0x104), ==, 0);

    qtest_writeq(qts, HEAD(0) + 0x78, 0xdeadbeef);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x78), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 0);
    qtest_writeq(fixture, HEAD(0) + 0x78, 0xdeadbeef);
    g_assert_cmphex(qtest_readq(fixture, HEAD(0) + 0x78), ==, 0);
    g_assert_cmphex(qtest_readq(fixture, HEAD(0) + 0x100), ==, 0);

    qtest_quit(fixture);
    qtest_quit(qts);

    qts = pic_runtime_error_start(true);
    qtest_writeq(qts, HEAD(0) + 0x30, UINT64_C(0x34000000));
    qtest_writeq(qts, HEAD(0) + 0x38, destination);
    qtest_writeq(qts, HEAD(0) + 0x128, 7);
    qtest_writel(qts, CPU_HEAD(0) + 0x10c, invalid_address);
    qtest_writel(qts, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(0) + 0x104), ==,
                    invalid_address);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 1);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_ACCESS_ERROR);
    qtest_quit(qts);

    bedrock = pic_bedrock_parent_start(false, true);
    qtest_sysbus_mmio_map(bedrock, PIC_CHILD, 0, CPU_BASE);
    uint64_t bedrock_destination = II_REGISTER | II_NODE(0) |
                                  II_HUB_REMOTE | 0x90;
    uint32_t destination_upper = (0x34U << 24) |
                                ((bedrock_destination >> 32) & 0xffff);

    qtest_writeq(bedrock, HEAD(0) + 0x30, destination_upper);
    qtest_writeq(bedrock, HEAD(0) + 0x38,
                 bedrock_destination & UINT32_MAX);
    qtest_writeq(bedrock, HEAD(0) + 0x128, 0);
    qtest_writel(bedrock, CPU_HEAD(0) + 0x10c, invalid_address);
    qtest_writel(bedrock, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(bedrock, CPU_HEAD(0) + 0x104), ==,
                    invalid_address);
    g_assert_cmphex(qtest_readl(bedrock, CPU_HEAD(1) + 0x104), ==, 0);
    g_assert_cmpuint(target_metric(bedrock, "x-test-packet-calls"), ==, 1);
    g_assert_cmphex(target_metric(bedrock, "x-test-packet-address"), ==,
                    bedrock_destination);
    g_assert_cmpuint(target_metric(bedrock, "x-test-packet-size"), ==, 8);
    g_assert_cmphex(qtest_readq(bedrock, 0x10000000 + 0x98) &
                    1, ==, 1);

    qtest_writeq(bedrock, HEAD(0) + 0x128, 7);
    qtest_writel(bedrock, CPU_HEAD(0) + 0x7c, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(bedrock, CPU_HEAD(0) + 0x104), ==,
                    invalid_address);
    g_assert_cmpuint(target_metric(bedrock, "x-test-packet-calls"), ==, 2);
    g_assert_cmphex(target_metric(bedrock, "x-test-packet-address"), ==,
                    bedrock_destination);
    g_assert_cmphex(qtest_readq(bedrock, 0x10000000 + 0x98) & 0x81, ==,
                    0x81);
    qtest_quit(bedrock);
}

static void test_cpu_pio_conventional_lanes(void)
{
    QTestState *qts = pic_runtime_cpu_start(true, NULL);
    uint64_t raw = BASE + 0x40000000ULL + 0x04000000 + 8;
    uint64_t short_view = CPU_HEAD(0) + 0x600000 + 8;
    uint64_t large_view = CPU_HEAD(0) + 0x44000008;

    pic_short_enable_card(qts, 0, 1, 0x02000000, 0x04000000, 0x3001,
                          PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
    qtest_writeq(qts, CPU_HEAD(0) + 0x210, 0x40 | (1U << 12));
    qtest_writeq(qts, raw, UINT64_C(0x8877665544332211));

    g_assert_cmphex(qtest_readb(qts, short_view), ==, 0x44);
    g_assert_cmphex(qtest_readw(qts, short_view + 2), ==, 0x2211);
    g_assert_cmphex(qtest_readw(qts, short_view), ==, 0x4433);
    g_assert_cmphex(qtest_readl(qts, short_view), ==, 0x44332211);
    g_assert_cmphex(qtest_readb(qts, large_view), ==, 0x44);
    g_assert_cmphex(qtest_readw(qts, large_view + 2), ==, 0x2211);
    g_assert_cmphex(qtest_readl(qts, large_view), ==, 0x44332211);

    qtest_writeb(qts, short_view, 0xa5);
    g_assert_cmphex(qtest_readb(qts, raw + 3), ==, 0xa5);
    qtest_writew(qts, short_view + 2, 0x1234);
    g_assert_cmphex(qtest_readb(qts, raw), ==, 0x34);
    g_assert_cmphex(qtest_readb(qts, raw + 1), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, raw + 2), ==, 0x33);
    qtest_writew(qts, short_view, 0x5678);
    g_assert_cmphex(qtest_readb(qts, raw + 2), ==, 0x78);
    g_assert_cmphex(qtest_readb(qts, raw + 3), ==, 0x56);

    qtest_writel(qts, short_view, 0x89abcdef);
    g_assert_cmphex(qtest_readl(qts, raw), ==, 0x89abcdef);
    qtest_writeb(qts, large_view, 0x5a);
    g_assert_cmphex(qtest_readb(qts, raw + 3), ==, 0x5a);
    qtest_writel(qts, large_view, 0x12345678);
    g_assert_cmphex(qtest_readb(qts, raw), ==, 0x78);
    g_assert_cmphex(qtest_readb(qts, raw + 1), ==, 0x56);
    g_assert_cmphex(qtest_readb(qts, raw + 2), ==, 0x34);
    g_assert_cmphex(qtest_readb(qts, raw + 3), ==, 0x12);

    qtest_writew(qts, short_view + 1, 0x6677);
    g_assert_cmphex(qtest_readb(qts, raw + 1), ==, 0x77);
    g_assert_cmphex(qtest_readb(qts, raw + 2), ==, 0x66);
    qtest_writeq(qts, short_view, UINT64_C(0x1029384756abcdef));
    g_assert_cmphex(qtest_readq(qts, raw), ==,
                    UINT64_C(0x1029384756abcdef));

    qtest_writeb(qts, HEAD(0) + 0x600000 + 8, 0x3c);
    g_assert_cmphex(qtest_readb(qts, raw), ==, 0x3c);

    /* Repeat lane checks on the second PCI host bridge. */
    {
        uint64_t raw1 = BASE + 0xc0000000ULL + 0x04000000 + 8;
        uint64_t view1 = CPU_HEAD(1) + 0x600000 + 8;

        pic_short_enable_card(qts, 1, 1, 0x02000000, 0x04000000, 0x3001,
                              PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        qtest_writeq(qts, CPU_HEAD(1) + 0x210, 0x40 | (1U << 12));
        qtest_writeq(qts, raw1, UINT64_C(0x8877665544332211));
        qtest_writeb(qts, view1, 0xa5);
        g_assert_cmphex(qtest_readb(qts, raw1 + 3), ==, 0xa5);
        qtest_writew(qts, view1 + 2, 0x1234);
        g_assert_cmphex(qtest_readb(qts, raw1), ==, 0x34);
        g_assert_cmphex(qtest_readb(qts, raw1 + 1), ==, 0x12);
        g_assert_cmphex(qtest_readb(qts, raw1 + 2), ==, 0x33);
        qtest_writel(qts, view1, 0x89abcdef);
        g_assert_cmphex(qtest_readl(qts, raw1), ==, 0x89abcdef);
    }

    qtest_quit(qts);
}

static QTestState *pic_runtime_actual_pio_start(int serial_fd,
                                                bool second_isp,
                                                bool switched)
{
    g_autofree char *chardev = serial_fd >= 0 ?
        g_strdup_printf("-chardev socket,id=serial,fd=%d ", serial_fd) :
        g_strdup("");
    const char *serial_arg = serial_fd >= 0 ? ",chardev1=serial" : "";
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S -d unimp,guest_errors "
        "-device sgi-xio-test-target,id=fabric,size=16777216,pic-peer=on,"
        "pic-runtime-profile=on,pic-linked=off,pic-bedrock-interrupts=off,"
        "pic-switched=%s "
        "-device ds1742,id=clock "
        "-device sgi-ioc4,bus=pic-head-0.0,addr=1,clock-hz=33333333,"
        "clock=/machine/peripheral/clock/memory-region%s "
        "-device qlogic-isp12160,bus=pic-head-0.0,addr=2 %s %s",
        switched ? "on" : "off", serial_arg,
        second_isp ? "-device qlogic-isp12160,bus=pic-head-1.0,addr=2" : "",
        chardev);

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);
    return qts;
}

static QTestState *pic_ioc4_pci_status_start(bool incoming, bool compatible)
{
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S %s"
        "-device sgi-xio-test-target,id=fabric,size=16777216,pic-peer=on,"
        "pic-runtime-profile=on,pic-linked=off,pic-bedrock-interrupts=off "
        "-device sgi-ioc4,bus=pic-head-0.0,addr=1%s "
        "-device sgi-ioc4,bus=pic-head-0.0,addr=2",
        incoming ? "-incoming defer " : "",
        compatible ? ",pci-status-compat=on,ata-readback-compat=on" : "");

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);
    return qts;
}

static void test_ioc4_pci_status_compatibility(void)
{
    QTestState *qts = pic_ioc4_pci_status_start(false, true);

    g_assert_cmphex(qtest_readl(qts, PIC_SHORT_CFG(0, 1)), ==, 0x100a10a9);
    g_assert_cmphex(qtest_readl(qts, PIC_SHORT_CFG(0, 1) + 8) & 0xff, ==,
                    0x53);
    g_assert_cmphex(qtest_readw(qts, PIC_SHORT_CFG(0, 1) + 4), ==, 0x0220);
    g_assert_cmphex(qtest_readl(qts, PIC_SHORT_CFG(0, 1) + 4), ==,
                    0x02200000);
    g_assert_cmphex(qtest_readl(qts, PIC_SHORT_CFG(0, 2) + 4), ==, 0);

    qtest_writel(qts, IOC4_CONFIG + 4, PCI_COMMAND_MEMORY);
    qtest_writel(qts, IOC4_CONFIG + 0x10, IOC4_BAR);
    qtest_writel(qts, IOC4_CONFIG2 + 4, PCI_COMMAND_MEMORY);
    qtest_writel(qts, IOC4_CONFIG2 + 0x10, IOC4_BAR2);
    qtest_writel(qts, IOC4_MEMORY + IOC4_ATA_READBACK,
                 bswap32(0x12345678));
    g_assert_cmphex(bswap32(qtest_readl(
                        qts, IOC4_MEMORY + IOC4_ATA_READBACK)), ==,
                    0x12345678);
    qtest_writel(qts, IOC4_MEMORY2 + IOC4_ATA_READBACK,
                 bswap32(0xffffffff));
    g_assert_cmphex(bswap32(qtest_readl(
                        qts, IOC4_MEMORY2 + IOC4_ATA_READBACK)), ==, 0);

    qtest_writew(qts, PIC_SHORT_CFG(0, 1) + 6, PCI_COMMAND_MEMORY);
    g_assert_cmphex(qtest_readw(qts, PIC_SHORT_CFG(0, 1) + 4), ==, 0x0220);
    g_assert_cmphex(qtest_readl(qts, PIC_SHORT_CFG(0, 1) + 4), ==,
                    0x02200002);
    qtest_writew(qts, PIC_SHORT_CFG(0, 1) + 4, 0xffff);
    g_assert_cmphex(qtest_readw(qts, PIC_SHORT_CFG(0, 1) + 4), ==, 0x0220);
    g_assert_cmphex(qtest_readl(qts, PIC_SHORT_CFG(0, 1) + 4), ==,
                    0x02200002);
    qtest_writel(qts, PIC_SHORT_CFG(0, 1) + 4, 0x00040001);
    g_assert_cmphex(qtest_readw(qts, PIC_SHORT_CFG(0, 1) + 4), ==, 0x0220);
    g_assert_cmphex(qtest_readl(qts, PIC_SHORT_CFG(0, 1) + 4), ==,
                    0x02200001);

    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readw(qts, PIC_SHORT_CFG(0, 1) + 4), ==, 0x0220);
    g_assert_cmphex(qtest_readl(qts, PIC_SHORT_CFG(0, 1) + 4), ==,
                    0x02200000);
    g_assert_cmphex(qtest_readl(qts, PIC_SHORT_CFG(0, 2) + 4), ==, 0);

    qtest_quit(qts);
}

static void test_ioc4_pci_status_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir =
        g_dir_make_tmp("sgi-pic-ioc4-status-migration-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_ioc4_pci_status_start(false, true);
    QTestState *to = pic_ioc4_pci_status_start(true, true);

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_writew(from, PIC_SHORT_CFG(0, 1) + 6, PCI_COMMAND_MEMORY);
    g_assert_cmphex(qtest_readl(from, PIC_SHORT_CFG(0, 1) + 4), ==,
                    0x02200002);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readw(to, PIC_SHORT_CFG(0, 1) + 4), ==, 0x0220);
    g_assert_cmphex(qtest_readl(to, PIC_SHORT_CFG(0, 1) + 4), ==,
                    0x02200002);
    g_assert_cmphex(qtest_readl(to, PIC_SHORT_CFG(0, 2) + 4), ==, 0);

    qtest_qmp_assert_success(to, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readw(to, PIC_SHORT_CFG(0, 1) + 4), ==, 0x0220);
    g_assert_cmphex(qtest_readl(to, PIC_SHORT_CFG(0, 1) + 4), ==,
                    0x02200000);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_ioc4_pci_status_migration_mode_mismatch(void)
{
    GError *error = NULL;
    g_autofree char *dir =
        g_dir_make_tmp("sgi-pic-ioc4-status-mismatch-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_ioc4_pci_status_start(false, true);
    QTestState *to = pic_ioc4_pci_status_start(true, false);
    int64_t deadline;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_probe_child(to)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_cpu_pio_actual_ioc4(void)
{
    int sockets[2];
    QTestState *qts;
    const uint8_t tx = 0x51;
    uint8_t clock_control;
    uint8_t received;
    struct pollfd pfd = { .fd = -1, .events = POLLIN };
    uint64_t cpu_view = CPU_HEAD(0) + 0x600000;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    qts = pic_runtime_actual_pio_start(sockets[1], false, false);
    close(sockets[1]);
    qtest_writel(qts, PIC_SHORT_CFG(0, 1) + 0x10, IOC4_BAR);
    qtest_writel(qts, PIC_SHORT_CFG(0, 1) + 4, PCI_COMMAND_MEMORY);
    qtest_writeq(qts, CPU_HEAD(0) + 0x210, 0x10 | (1U << 12));
    g_assert_cmphex(qtest_readq(qts, CPU_HEAD(0) + 0x210), ==,
                    0x10 | (1U << 12));

    /* IOC4 UART byte offsets stay native through its active PCI BAR. */
    qtest_writeb(qts, cpu_view + 0x383, 3);
    qtest_writeb(qts, cpu_view + 0x382, 1);
    qtest_writeb(qts, cpu_view + 0x384, 0x03);
    qtest_writeb(qts, cpu_view + 0x380, tx);
    pfd.fd = sockets[0];
    g_assert_cmpint(poll(&pfd, 1, 1000), ==, 1);
    g_assert_cmpint(read(sockets[0], &received, 1), ==, 1);
    g_assert_cmphex(received, ==, tx);

    /* The supported IOC4 enable register is an asymmetric native-order word. */
    qtest_writel(qts, cpu_view + 0x10, 0x00000040);
    g_assert_cmphex(qtest_readl(qts, cpu_view + 0x10), ==, 0x40);
    g_assert_cmphex(pic_ioc4_read_reg(qts, 0x10), ==, 0x40);

    /* The BAR clock alias exposes the DS1742 clock bytes at its BAR offset. */
    clock_control = qtest_readb(qts, IOC4_MEMORY + 0x87ff8) | 0x80;
    qtest_writeb(qts, cpu_view + 0x87ff8, clock_control);
    qtest_writeb(qts, cpu_view + 0x87ff9, 0x42);
    g_assert_cmphex(qtest_readb(qts, cpu_view + 0x87ff8), ==, clock_control);
    g_assert_cmphex(qtest_readb(qts, cpu_view + 0x87ff9), ==, 0x42);
    g_assert_cmphex(qtest_readb(qts, IOC4_MEMORY + 0x87ff9), ==, 0x42);
    g_assert_cmphex(qtest_readb(qts, IOC4_MEMORY + 0x87ff8), ==,
                    clock_control);

    qtest_quit(qts);
    close(sockets[0]);
}

static void test_cpu_pio_isp12160(void)
{
    QTestState *qts = pic_runtime_actual_pio_start(-1, false, false);
    uint64_t cpu_view = CPU_HEAD(0) + 0x600000;
    uint64_t raw = BASE + 0x45000000ULL;

    qtest_writel(qts, PIC_SHORT_CFG(0, 2) + 0x14, 0x05000000);
    qtest_writel(qts, PIC_SHORT_CFG(0, 2) + 4, PCI_COMMAND_MEMORY);
    qtest_writeq(qts, CPU_HEAD(0) + 0x210, 0x50 | (1U << 12));
    g_assert_cmphex(qtest_readq(qts, CPU_HEAD(0) + 0x210), ==,
                    0x50 | (1U << 12));

    /* CPU offsets 0x0e and 0x72 select adapter offsets 0x0c and 0x70. */
    qtest_writew(qts, cpu_view + 0x0e, 1);
    g_assert_cmphex(qtest_readw(qts, cpu_view + 0x0e), ==, 1);
    g_assert_cmphex(qtest_readw(qts, raw + 0x0c), ==, 1);
    qtest_writew(qts, cpu_view + 0x0e, 0);
    g_assert_cmphex(qtest_readw(qts, cpu_view + 0x0e), ==, 0);

    g_assert_cmphex(qtest_readw(qts, cpu_view + 0x70), ==, 0x4953);
    qtest_writew(qts, cpu_view + 0x72, 0x0006);
    qtest_writew(qts, cpu_view + 0xc2, 0x5000);
    g_assert_cmphex(qtest_readw(qts, cpu_view + 0x72), ==, 0x4000);
    g_assert_cmphex(qtest_readw(qts, cpu_view + 0x70), ==, 0x4953);
    g_assert_cmphex(qtest_readw(qts, raw + 0x70), ==, 0x4000);
    g_assert_cmphex(qtest_readw(qts, raw + 0x72), ==, 0x4953);

    qtest_quit(qts);
}

static hwaddr isp_mbox_cpu_offset(unsigned mailbox)
{
    return (0x70 + mailbox * 2) ^ 2;
}

static void isp_mailbox_write(QTestState *qts, uint64_t cpu_view,
                              unsigned mailbox, uint16_t value)
{
    qtest_writew(qts, cpu_view + isp_mbox_cpu_offset(mailbox), value);
}

static uint16_t isp_mailbox_read(QTestState *qts, uint64_t cpu_view,
                                 unsigned mailbox)
{
    return qtest_readw(qts, cpu_view + isp_mbox_cpu_offset(mailbox));
}

static uint16_t isp_mailbox_command(QTestState *qts, uint64_t cpu_view,
                                    uint16_t command)
{
    isp_mailbox_write(qts, cpu_view, 0, command);
    qtest_writew(qts, cpu_view + 0xc2, 0x5000);
    return isp_mailbox_read(qts, cpu_view, 0);
}

static void isp_mailbox_ack(QTestState *qts, uint64_t cpu_view)
{
    qtest_writew(qts, cpu_view + 0xc2, 0x7000);
    qtest_writew(qts, cpu_view + 0x0e, 0);
}

static void isp_load_ram_a64(QTestState *qts, uint64_t cpu_view,
                             uint64_t address, uint16_t base, uint16_t count)
{
    uint16_t status;

    isp_mailbox_write(qts, cpu_view, 1, base);
    isp_mailbox_write(qts, cpu_view, 2, address >> 16);
    isp_mailbox_write(qts, cpu_view, 3, address);
    isp_mailbox_write(qts, cpu_view, 4, count);
    isp_mailbox_write(qts, cpu_view, 6, address >> 48);
    isp_mailbox_write(qts, cpu_view, 7, address >> 32);
    status = isp_mailbox_command(qts, cpu_view, 9);

    isp_mailbox_ack(qts, cpu_view);
    g_assert_cmphex(status, ==, 0x4000);
}

static uint16_t isp_read_ram_word(QTestState *qts, uint64_t cpu_view,
                                  uint16_t address)
{
    uint16_t status, value;

    isp_mailbox_write(qts, cpu_view, 1, address);
    status = isp_mailbox_command(qts, cpu_view, 5);
    value = isp_mailbox_read(qts, cpu_view, 2);

    isp_mailbox_ack(qts, cpu_view);
    g_assert_cmphex(status, ==, 0x4000);
    return value;
}

static void test_direct64_isp_load(void)
{
    static const uint8_t source[12] = {
        0xa5, 0x10, 0x11, 0x12, 0x13, 0x14,
        0x15, 0x16, 0x17, 0x5a, 0x00, 0x6b,
    };
    static const uint16_t byte_stream_words[4] = {
        0x1110, 0x1312, 0x1514, 0x1716,
    };
    static const uint16_t word_value_words[4] = {
        0x1011, 0x16a5, 0x1415, 0x6b13,
    };
    const uint64_t payload = 0x1ffd;
    const uint64_t isp_payload = 0xc08000;
    const unsigned heads[] = { 0, 1 };
    QTestState *qts = pic_runtime_actual_pio_start(-1, true, false);
    uint8_t observed[sizeof(source)];
    unsigned h, i, mode;

    qtest_memwrite(qts, TARGET + payload - 1, source, sizeof(source));
    qtest_memwrite(qts, TARGET + isp_payload - 1, source, sizeof(source));

    for (h = 0; h < G_N_ELEMENTS(heads); h++) {
        unsigned head = heads[h];
        uint64_t cpu_view = CPU_HEAD(head) + 0x600000;
        uint64_t dma_base = UINT64_C(0xa) << 60;

        qtest_writel(qts, PIC_SHORT_CFG(head, 2) + 0x14, 0x05000000);
        qtest_writel(qts, PIC_SHORT_CFG(head, 2) + 4,
                     PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
        qtest_writeq(qts, CPU_HEAD(head) + 0x20, head ? 4 : 3);
        qtest_writeq(qts, CPU_HEAD(head) + 0x80,
                     (head ? 2ULL : 1ULL) << 20);
        qtest_writeq(qts, CPU_HEAD(head) + 0x208, 1U << 19);
        qtest_writeq(qts, CPU_HEAD(head) + 0x210,
                     0x50 | (1U << 12));

        for (mode = 0; mode < 4; mode++) {
            uint64_t address = dma_base | payload |
                ((mode & 1) ? UINT64_C(1) << 58 : 0) |
                ((mode & 2) ? UINT64_C(1) << 56 : 0) |
                ((mode & 1) ? 0 : UINT64_C(1) << 55);
            const uint16_t *expected = mode & 1 ? word_value_words :
                                                  byte_stream_words;

            isp_load_ram_a64(qts, cpu_view, address, 0x100 + mode * 4, 4);
            for (i = 0; i < 4; i++) {
                g_assert_cmphex(isp_read_ram_word(qts, cpu_view,
                                                  0x100 + mode * 4 + i),
                                ==, expected[i]);
            }
        }

        /* This ISP12160 firmware address requests prefetch and channel 1. */
        isp_load_ram_a64(qts, cpu_view, dma_base | (UINT64_C(1) << 59) |
                         (UINT64_C(1) << 57) | (UINT64_C(1) << 55) |
                         isp_payload, 0x180 + h * 4, 4);
        for (i = 0; i < 4; i++) {
            g_assert_cmphex(isp_read_ram_word(qts, cpu_view,
                                              0x180 + h * 4 + i),
                            ==, byte_stream_words[i]);
        }
    }

    qtest_memread(qts, TARGET + payload - 1, observed, sizeof(source));
    g_assert_cmpmem(observed, sizeof(observed), source, sizeof(source));
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 4);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-device-known"), ==, 0);
    qtest_quit(qts);
}

static void direct64_card_write(QTestState *qts, unsigned head,
                                uint64_t address, uint32_t length,
                                uint32_t seed)
{
    qtest_writel(qts, CARD(head), address);
    qtest_writel(qts, CARD(head) + 0x18, address >> 32);
    qtest_writel(qts, CARD(head) + 4, length);
    qtest_writel(qts, CARD(head) + 8, seed);
    qtest_writel(qts, CARD(head) + 0xc, 1);
}

static void direct64_card_copy(QTestState *qts, unsigned head,
                               uint64_t source, uint64_t destination,
                               uint32_t length)
{
    qtest_writel(qts, CARD(head), destination);
    qtest_writel(qts, CARD(head) + 0x18, destination >> 32);
    qtest_writel(qts, CARD(head) + 0x14, source);
    qtest_writel(qts, CARD(head) + 0x1c, source >> 32);
    qtest_writel(qts, CARD(head) + 4, length);
    qtest_writel(qts, CARD(head) + 0xc, 2);
}

static void direct64_card_enable(QTestState *qts, unsigned head)
{
    qtest_writel(qts, HEAD(head) + 0x21010, 0x01000000);
    qtest_writel(qts, HEAD(head) + 0x21004,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writeq(qts, HEAD(head) + 0x20, head ? 4 : 3);
    qtest_writeq(qts, HEAD(head) + 0x80, (head ? 2ULL : 1ULL) << 20);
    qtest_writeq(qts, HEAD(head) + 0x200, 1U << 19);
}

static void test_direct64_card_write(void)
{
    static const uint8_t initial[16] = {
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
    };
    const uint64_t payload = 0x2ffc;
    const uint64_t widget = UINT64_C(0xa) << 60;
    const unsigned heads[] = { 0, 1 };
    QTestState *qts = pic_runtime_cpu_start(true, NULL);
    uint8_t observed[sizeof(initial)];
    unsigned h, i;

    qtest_memwrite(qts, TARGET + payload - 3, initial, sizeof(initial));
    for (h = 0; h < G_N_ELEMENTS(heads); h++) {
        unsigned head = heads[h];

        direct64_card_enable(qts, head);
        direct64_card_write(qts, head, widget | (UINT64_C(1) << 55) |
                            payload, 8, 0x31 + head * 0x20);
        qtest_memread(qts, TARGET + payload - 3, observed, sizeof(observed));
        for (i = 0; i < 8; i++) {
            g_assert_cmphex(observed[3 + i], ==,
                            0x31 + head * 0x20 + i);
        }
        for (i = 0; i < sizeof(observed); i++) {
            if (i < 3 || i > 10) {
                g_assert_cmphex(observed[i], ==, 0xa5);
            }
        }

        direct64_card_write(qts, head, widget | payload, 8,
                            0x61 + head * 0x20);
        qtest_memread(qts, TARGET + payload - 3, observed, sizeof(observed));
        for (i = 0; i < 8; i++) {
            unsigned index = ((payload + i) ^ 3) - (payload - 3);

            g_assert_cmphex(observed[index], ==, 0x61 + head * 0x20 + i);
        }
        for (i = 0; i < sizeof(observed); i++) {
            if (i < 3 || i > 10) {
                g_assert_cmphex(observed[i], ==, 0xa5);
            }
        }
    }
    qtest_quit(qts);
}

static void test_direct64_requester_width(void)
{
    static const uint8_t sentinel[24] = {
        0xa5, 0x5a, 0x91, 0x2c, 0xe3, 0x74, 0x16, 0xc8,
        0x37, 0xf0, 0x6b, 0x42, 0x8d, 0x19, 0xb6, 0x53,
        0x72, 0x0d, 0xc4, 0x38, 0x6e, 0x11, 0xfa, 0x85,
    };
    static const uint8_t source_bytes[24] = {
        0x13, 0x84, 0x2b, 0xd6, 0x50, 0xa1, 0x7c, 0x09,
        0xe2, 0x35, 0xb8, 0x4f, 0x61, 0xca, 0x96, 0x0d,
        0x77, 0x18, 0xf3, 0x42, 0xad, 0x5e, 0x20, 0x89,
    };
    const uint64_t widget = UINT64_C(0xa) << 60;
    const uint64_t payload = 0x3400;
    const uint64_t copy_source = 0x3800;
    const uint64_t copy_destination = 0x3a04;
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S -d unimp,guest_errors "
        "-device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,"
        "pic-runtime-profile=on,pic-linked=off,pic-bedrock-interrupts=off "
        "-device sgi-bridge-dma-test-64bit,bus=pic-head-0.0,addr=1 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1 "
        "-global sgi-pic.req64-slots-head0=0x2");
    uint8_t observed[sizeof(sentinel)];
    uint8_t copied[sizeof(sentinel)];

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);

    for (unsigned reset = 0; reset < 2; reset++) {
        for (unsigned head = 0; head < 2; head++) {
            direct64_card_enable(qts, head);
            g_assert_cmphex(qtest_readl(qts, CARD(head) + 0x38), ==,
                            head == 0 ? 1 : 0);
            qtest_memwrite(qts, TARGET + payload - 4, sentinel,
                           sizeof(sentinel));
            direct64_card_write(qts, head, widget | payload, 13, 0x30);
            qtest_memread(qts, TARGET + payload - 4, observed,
                          sizeof(observed));

            for (unsigned i = 0; i < 13; i++) {
                unsigned index = ((payload + i) ^
                                  (head == 0 ? 7 : 3)) - (payload - 4);

                g_assert_cmphex(observed[index], ==, 0x30 + i);
            }
            for (unsigned i = 0; i < sizeof(observed); i++) {
                bool touched = false;

                for (unsigned j = 0; j < 13; j++) {
                    unsigned index = ((payload + j) ^
                                      (head == 0 ? 7 : 3)) - (payload - 4);

                    touched |= i == index;
                }
                if (!touched) {
                    g_assert_cmphex(observed[i], ==, sentinel[i]);
                }
            }
            memset(observed, 0xa5, sizeof(observed));
            qtest_memwrite(qts, TARGET + copy_source - 4, source_bytes,
                           sizeof(source_bytes));
            qtest_memwrite(qts, TARGET + copy_destination - 4, observed,
                           sizeof(observed));
            direct64_card_copy(qts, head, widget | copy_source,
                               widget | copy_destination, 13);
            g_assert_cmphex(qtest_readl(qts, CARD(head) + 0x28), ==,
                            MEMTX_OK);
            qtest_memread(qts, TARGET + copy_destination - 4, copied,
                          sizeof(copied));
            for (unsigned i = 0; i < 13; i++) {
                unsigned source_index = ((copy_source + i) ^
                                         (head == 0 ? 7 : 3)) -
                                        (copy_source - 4);
                unsigned destination_index = ((copy_destination + i) ^
                                              (head == 0 ? 7 : 3)) -
                                             (copy_destination - 4);

                g_assert_cmphex(copied[destination_index], ==,
                                source_bytes[source_index]);
            }
            for (unsigned i = 0; i < sizeof(copied); i++) {
                bool touched = false;

                for (unsigned j = 0; j < 13; j++) {
                    unsigned index = ((copy_destination + j) ^
                                      (head == 0 ? 7 : 3)) -
                                     (copy_destination - 4);

                    touched |= i == index;
                }
                if (!touched) {
                    g_assert_cmphex(copied[i], ==, 0xa5);
                }
            }
        }

        if (reset == 0) {
            qtest_system_reset(qts);
        }
    }
    qtest_quit(qts);
}

static void test_direct64_requester_and_dir_map_controls(void)
{
    const uint64_t address = (UINT64_C(0xa) << 60) |
                             (UINT64_C(1) << 55) | 0x2400;
    const unsigned heads[] = { 0, 1 };
    uint8_t sentinel[8], observed[8];
    QTestState *qts = pic_runtime_cpu_start(true, NULL);

    memset(sentinel, 0xa5, sizeof(sentinel));
    for (unsigned h = 0; h < G_N_ELEMENTS(heads); h++) {
        unsigned head = heads[h];
        uint64_t calls;

        direct64_card_enable(qts, head);
        qtest_memwrite(qts, TARGET + 0x2400, sentinel, sizeof(sentinel));
        calls = target_metric(qts, "x-test-packet-calls");

        qtest_writeq(qts, HEAD(head) + 0x200, 0);
        qtest_writeq(qts, HEAD(head) + 0x20, 5 + head);
        qtest_writeq(qts, HEAD(head) + 0x80, 0xd12345);
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x80), ==, 0xd12345);
        direct64_card_write(qts, head, address, sizeof(sentinel), 0x31 + head);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                         calls + 1);
        qtest_memread(qts, TARGET + 0x2400, observed, sizeof(observed));
        for (unsigned i = 0; i < sizeof(observed); i++) {
            g_assert_cmphex(observed[i], ==, 0x31 + head + i);
        }
        g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==,
                         5 + head);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-address"), ==,
                         0x2400);

        qtest_writeq(qts, HEAD(head) + 0x200, 1U << 19);
        qtest_writeq(qts, HEAD(head) + 0x20, 7 + head);
        qtest_writeq(qts, HEAD(head) + 0x80, 0x21abcd);
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x80), ==, 0x21abcd);
        direct64_card_write(qts, head, address, sizeof(sentinel),
                            0x51 + head);
        qtest_memread(qts, TARGET + 0x2400, observed, sizeof(observed));
        for (unsigned i = 0; i < sizeof(observed); i++) {
            g_assert_cmphex(observed[i], ==, 0x51 + head + i);
        }
        g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==,
                         7 + head);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-address"), ==,
                         0x2400);
    }
    qtest_quit(qts);
}

static void test_direct64_attribute_matrix(void)
{
    const uint64_t widget = UINT64_C(0xa) << 60;
    const unsigned heads[] = { 0, 1 };
    uint8_t source[4], observed[4];
    QTestState *qts = pic_runtime_cpu_start(true, NULL);

    for (unsigned h = 0; h < G_N_ELEMENTS(heads); h++) {
        unsigned head = heads[h];

        direct64_card_enable(qts, head);
        for (unsigned swap = 0; swap < 2; swap++) {
            for (unsigned attributes = 0; attributes < 16; attributes++) {
                uint64_t calls = target_metric(qts, "x-test-packet-calls");
                uint64_t bits = ((attributes & 1) ? UINT64_C(1) << 58 : 0) |
                                ((attributes & 2) ? UINT64_C(1) << 56 : 0) |
                                ((attributes & 4) ? UINT64_C(1) << 59 : 0) |
                                ((attributes & 8) ? UINT64_C(1) << 57 : 0) |
                                (swap ? UINT64_C(1) << 55 : 0);
                uint64_t source_address = widget | bits | 0x4000;
                uint64_t destination = widget | bits | 0x5000;

                for (unsigned i = 0; i < sizeof(source); i++) {
                    source[i] = 0x40 + i;
                }
                qtest_memwrite(qts, TARGET + 0x4000, source, sizeof(source));
                memset(observed, 0xa5, sizeof(observed));
                qtest_memwrite(qts, TARGET + 0x5000, observed,
                               sizeof(observed));
                direct64_card_copy(qts, head, source_address, destination,
                                   sizeof(source));
                g_assert_cmphex(qtest_readl(qts, CARD(head) + 0x28), ==,
                                MEMTX_OK);
                g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"),
                                 ==, calls + (swap ? 2 : 8));
                g_assert_cmpuint(target_metric(qts, "x-test-packet-size"),
                                 ==, swap ? sizeof(source) : 1);
                g_assert_cmphex(target_metric(qts, "x-test-packet-address"),
                                ==, 0x5000);
                qtest_memread(qts, TARGET + 0x5000, observed,
                              sizeof(observed));
                g_assert_cmpmem(observed, sizeof(observed), source,
                                sizeof(source));

                direct64_card_write(qts, head, destination, sizeof(source),
                                    0x60 + attributes * 4 + swap * 8);
                qtest_memread(qts, TARGET + 0x5000, observed,
                              sizeof(observed));
                for (unsigned i = 0; i < sizeof(observed); i++) {
                    unsigned source_index = swap ? i : (i ^ 3);

                    g_assert_cmphex(observed[i], ==,
                        (0x60 + attributes * 4 + swap * 8) + source_index);
                }
            }
        }
    }
    qtest_quit(qts);
}

static QTestState *pic_runtime_packet_window_start(void)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors "
        "-device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,"
        "pic-runtime-profile=on,pic-linked=off,pic-bedrock-interrupts=off,"
        "packet-window-base=0x124c000 "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1");

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);
    return qts;
}

static void test_direct64_kernel_queue_addresses(void)
{
    static const uint64_t queue_addresses[] = {
        UINT64_C(0xa38000000124c000), UINT64_C(0xa380000001250000),
    };
    static const uint8_t source[8] = {
        0x91, 0x20, 0xe3, 0x44, 0x15, 0xb6, 0x77, 0xc8,
    };
    uint8_t actual[16];
    uint8_t initialized[16];
    QTestState *qts = pic_runtime_packet_window_start();

    memset(initialized, 0xa5, sizeof(initialized));
    qtest_memwrite(qts, TARGET, source, sizeof(source));
    for (unsigned h = 0; h < 2; h++) {
        uint64_t calls;

        direct64_card_enable(qts, h);
        calls = target_metric(qts, "x-test-packet-calls");
        qtest_memwrite(qts, TARGET + 0x4000, initialized,
                       sizeof(initialized));
        direct64_card_copy(qts, h, queue_addresses[0], queue_addresses[1],
                           sizeof(source));
        g_assert_cmphex(qtest_readl(qts, CARD(h) + 0x28), ==, MEMTX_OK);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                         calls + 2);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-size"), ==,
                         sizeof(source));
        g_assert_cmphex(target_metric(qts, "x-test-packet-address"), ==,
                        0x01250000);
        qtest_memread(qts, TARGET + 0x4000, actual, sizeof(actual));
        g_assert_cmpmem(actual, 8, source, sizeof(source));
        g_assert_cmpmem(actual + 8, 8, initialized + 8, 8);
        qtest_memread(qts, TARGET, actual, sizeof(actual));
        g_assert_cmpmem(actual, 8, source, sizeof(source));
    }
    qtest_quit(qts);
}

static void test_direct64_rejects_unsupported_addresses(void)
{
    static const uint64_t unsupported_bits[] = {
        UINT64_C(1) << 54, UINT64_C(1) << 53, UINT64_C(1) << 52,
        UINT64_C(1) << 51, UINT64_C(1) << 50, UINT64_C(1) << 49,
        UINT64_C(1) << 48,
    };
    const uint64_t widget = UINT64_C(0xa) << 60;
    const unsigned heads[] = { 0, 1 };
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-direct64-refusal-XXXXXX",
                                          &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log_contents = NULL;
    uint8_t sentinel[8], observed[8];
    QTestState *qts;
    unsigned h, i;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    qts = qtest_initf("-M none -cpu R10000 -S -d unimp,guest_errors -D %s "
        "-device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,"
        "pic-runtime-profile=on,pic-linked=off,pic-bedrock-interrupts=off "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1", log_path);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);

    memset(sentinel, 0xa5, sizeof(sentinel));
    qtest_memwrite(qts, TARGET + 0x1000, sentinel, sizeof(sentinel));
    for (h = 0; h < G_N_ELEMENTS(heads); h++) {
        unsigned head = heads[h];
        uint64_t calls;

        direct64_card_enable(qts, head);
        calls = target_metric(qts, "x-test-packet-calls");
        for (i = 0; i < G_N_ELEMENTS(unsupported_bits); i++) {
            direct64_card_write(qts, head, widget | unsupported_bits[i] |
                                0x1000, 4, 0x80 + i);
            g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                             calls);
            g_assert_cmphex(qtest_readl(qts, CARD(head) + 0x28), !=,
                            MEMTX_OK);
            qtest_memread(qts, TARGET + 0x1000, observed, sizeof(observed));
            g_assert_cmpmem(observed, sizeof(observed), sentinel,
                            sizeof(sentinel));
        }
        direct64_card_write(qts, head, widget | (UINT64_C(1) << 47) |
                            0x1000, 4, 0x90);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                         calls);
        g_assert_cmphex(qtest_readl(qts, CARD(head) + 0x28), !=, MEMTX_OK);
        qtest_memread(qts, TARGET + 0x1000, observed, sizeof(observed));
        g_assert_cmpmem(observed, sizeof(observed), sentinel,
                        sizeof(sentinel));
        direct64_card_write(qts, head, UINT64_C(1) << 32, 4, 0xa0);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                         calls);
        g_assert_cmphex(qtest_readl(qts, CARD(head) + 0x28), !=, MEMTX_OK);
        qtest_memread(qts, TARGET + 0x1000, observed, sizeof(observed));
        g_assert_cmpmem(observed, sizeof(observed), sentinel,
                        sizeof(sentinel));
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log_contents, "direct64 reserved-bits"));
    g_assert_nonnull(strstr(log_contents, "direct64 target-zero"));
    g_assert_nonnull(strstr(log_contents, "direct64 register-space"));
    unlink(log_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_direct64_range_boundary_is_split(void)
{
    const uint64_t boundary = UINT64_C(1) << 47;
    uint8_t initial[8], observed[8];
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S -d unimp,guest_errors "
        "-device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,"
        "pic-runtime-profile=on,packet-window-base=140737488355326 "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1");

    memset(initial, 0xa5, sizeof(initial));
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);
    qtest_memwrite(qts, TARGET, initial, sizeof(initial));
    direct64_card_enable(qts, 0);
    direct64_card_write(qts, 0, (UINT64_C(0xa) << 60) |
                        (UINT64_C(1) << 55) | (boundary - 2), 4, 0x90);

    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 1);
    g_assert_cmphex(qtest_readl(qts, CARD(0) + 0x28), ==,
                    MEMTX_DECODE_ERROR);
    qtest_memread(qts, TARGET, observed, sizeof(observed));
    g_assert_cmphex(observed[0], ==, 0x90);
    g_assert_cmphex(observed[1], ==, 0x91);
    for (unsigned i = 2; i < sizeof(observed); i++) {
        g_assert_cmphex(observed[i], ==, 0xa5);
    }
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_OK);
    qtest_quit(qts);
}

static void test_direct64_target_error_preserves_data(void)
{
    const uint64_t widget = UINT64_C(0xa) << 60;
    uint8_t sentinel[8], observed[8];
    QTestState *qts = pic_runtime_cpu_start(
        true, "-global sgi-xio-test-target.packet-failure=on");

    memset(sentinel, 0xa5, sizeof(sentinel));
    qtest_memwrite(qts, TARGET + 0x3000, sentinel, sizeof(sentinel));
    for (unsigned head = 0; head < 2; head++) {
        direct64_card_enable(qts, head);
    }
    for (unsigned head = 0; head < 2; head++) {
        for (unsigned swap = 0; swap < 2; swap++) {
            for (unsigned attributes = 0; attributes < 4; attributes++) {
                uint64_t bits = ((attributes & 1) ? UINT64_C(1) << 58 : 0) |
                                ((attributes & 2) ? UINT64_C(1) << 56 : 0) |
                                (swap ? UINT64_C(1) << 55 : 0);
                uint64_t destination = widget | bits | 0x3000;
                uint64_t source = widget | bits | (UINT64_C(1) << 47) |
                                  0x3000;

                qtest_memwrite(qts, TARGET + 0x3000, sentinel,
                               sizeof(sentinel));
                direct64_card_write(qts, head, destination,
                                    sizeof(sentinel), 0x51);
                g_assert_cmphex(qtest_readl(qts, CARD(head) + 0x28), ==,
                                MEMTX_ACCESS_ERROR);
                qtest_memread(qts, TARGET + 0x3000, observed,
                              sizeof(observed));
                g_assert_cmpmem(observed, sizeof(observed), sentinel,
                                sizeof(sentinel));

                direct64_card_copy(qts, head, source, destination,
                                   sizeof(sentinel));
                g_assert_cmphex(qtest_readl(qts, CARD(head) + 0x28), ==,
                                MEMTX_DECODE_ERROR);
                qtest_memread(qts, TARGET + 0x3000, observed,
                              sizeof(observed));
                g_assert_cmpmem(observed, sizeof(observed), sentinel,
                                sizeof(sentinel));
            }
        }
    }
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_ACCESS_ERROR);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 16);
    qtest_quit(qts);
}

static void test_direct64_crossbow_widget_routing(void)
{
    const uint64_t payload = 0x1800;
    const uint64_t control = 0x10000000;
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S -d unimp,guest_errors "
        "-device sgi-xbow-transport-test,id=switch "
        "-device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,"
        "pic-runtime-profile=on,pic-linked=on,pic-switched=on,"
        "target-port=/machine/peripheral/switch/xbow,target-index=9 "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1");
    uint64_t initial8, initial10;

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/switch", 0, control);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 3, BASE);
    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);
    direct64_card_enable(qts, 0);
    initial8 = qtest_readq(qts, control + 0x100 + 1 * 32 + 16);
    initial10 = qtest_readq(qts, control + 0x100 + 3 * 32 + 16);

    direct64_card_write(qts, 0, (UINT64_C(8) << 60) |
                        (UINT64_C(1) << 55) | payload, 8, 0x28);
    g_assert_cmpuint(qtest_readq(qts, control + 0x100 + 1 * 32), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, control + 0x100 + 3 * 32), ==, 0);
    g_assert_cmphex(qtest_readq(qts, control + 0x100 + 1 * 32 + 8), ==,
                    payload);
    g_assert_cmphex(qtest_readq(qts, control + 0x100 + 1 * 32 + 24), ==, 24);
    g_assert_cmphex(qtest_readq(qts, control + 0x100 + 1 * 32 + 16), !=,
                    initial8);
    g_assert_cmphex(qtest_readq(qts, control + 0x100 + 3 * 32 + 16), ==,
                    initial10);

    direct64_card_write(qts, 0, (UINT64_C(10) << 60) |
                        (UINT64_C(1) << 55) | payload, 8, 0x3a);
    g_assert_cmpuint(qtest_readq(qts, control + 0x100 + 1 * 32), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, control + 0x100 + 3 * 32), ==, 1);
    g_assert_cmphex(qtest_readq(qts, control + 0x100 + 3 * 32 + 8), ==,
                    payload);
    g_assert_cmphex(qtest_readq(qts, control + 0x100 + 3 * 32 + 24), ==, 24);
    g_assert_cmphex(qtest_readq(qts, control + 0x100 + 3 * 32 + 16), !=,
                    initial10);

    direct64_card_write(qts, 0, (UINT64_C(12) << 60) |
                        (UINT64_C(1) << 55) | payload, 8, 0x4c);
    g_assert_cmpuint(qtest_readq(qts, control + 0x100 + 1 * 32), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, control + 0x100 + 3 * 32), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, CARD(0) + 0x28), ==, MEMTX_ERROR);
    qtest_quit(qts);
}

static void test_cpu_pio_order_follows_pci_mapping(void)
{
    QTestState *qts = pic_runtime_cpu_start(
        true,
        "-device ds1742,id=clock "
        "-device sgi-ioc4,bus=pic-head-0.0,addr=2,clock-hz=33333333,"
        "clock=/machine/peripheral/clock/memory-region");
    uint64_t cpu_short = CPU_HEAD(0) + 0x600008;
    uint64_t ordinary = BASE + 0x44000008;
    uint64_t native = BASE + 0x45000000 + 0x87ff8;
    uint64_t relocated = BASE + 0x46000000 + 0x87ff8;
    uint64_t cpu_clock = CPU_HEAD(0) + 0x600000 + 0x87ff8;
    uint8_t clock_control;

    pic_short_enable_card(qts, 0, 1, 0x02000000, 0x04000000, 0x3001,
                          PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
    qtest_writel(qts, PIC_SHORT_CFG(0, 2) + 0x10, 0x05000000);
    qtest_writel(qts, PIC_SHORT_CFG(0, 2) + 4, PCI_COMMAND_MEMORY);
    qtest_writeq(qts, CPU_HEAD(0) + 0x210, 0x40 | (1U << 12));
    qtest_writeq(qts, ordinary, UINT64_C(0x0123456789abcdef));
    qtest_writeb(qts, cpu_short, 0xa1);
    g_assert_cmphex(qtest_readb(qts, ordinary + 3), ==, 0xa1);

    qtest_writeq(qts, CPU_HEAD(0) + 0x210, 0x50 | (1U << 12));
    clock_control = qtest_readb(qts, native) | 0x80;
    qtest_writeb(qts, cpu_clock, clock_control);
    qtest_writeb(qts, cpu_clock + 1, 0x42);
    g_assert_cmphex(qtest_readb(qts, cpu_clock + 1), ==, 0x42);
    g_assert_cmphex(qtest_readb(qts, native + 1), ==, 0x42);

    qtest_writel(qts, PIC_SHORT_CFG(0, 2) + 0x10, 0x06000000);
    qtest_writeq(qts, CPU_HEAD(0) + 0x210, 0x60 | (1U << 12));
    qtest_writeb(qts, cpu_clock + 1, 0x43);
    g_assert_cmphex(qtest_readb(qts, relocated + 1), ==, 0x43);

    qtest_writel(qts, PIC_SHORT_CFG(0, 2) + 4, 0);
    qtest_writeb(qts, cpu_clock + 1, 0x44);
    qtest_writel(qts, PIC_SHORT_CFG(0, 2) + 4, PCI_COMMAND_MEMORY);
    g_assert_cmphex(qtest_readb(qts, relocated + 1), ==, 0x43);
    qtest_quit(qts);
}

static void test_cpu_pio_overlapping_bars_use_active_function(void)
{
    QTestState *qts = pic_runtime_cpu_start(
        true,
        "-device sgi-ioc4,bus=pic-head-0.0,addr=2,clock-hz=33333333");
    uint64_t raw = BASE + 0x45000010;
    uint64_t cpu_short = CPU_HEAD(0) + 0x600010;

    pic_short_enable_card(qts, 0, 1, 0x05000000, 0x04000000, 0x3001,
                          PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
    qtest_writel(qts, PIC_SHORT_CFG(0, 2) + 0x10, 0x05000000);
    qtest_writel(qts, PIC_SHORT_CFG(0, 2) + 4, PCI_COMMAND_MEMORY);
    qtest_writeq(qts, CPU_HEAD(0) + 0x210, 0x50 | (1U << 12));

    /* IOC4 owns the overlap: its enable register differs from the fixture's. */
    qtest_writel(qts, raw, 0x40);
    g_assert_cmphex(qtest_readl(qts, raw), ==, 0x40);
    qtest_writel(qts, cpu_short, 0x20);
    g_assert_cmphex(qtest_readl(qts, raw), ==, 0x20000040);

    qtest_quit(qts);
}

static QTestState *pic_profile_migration_start(bool runtime_profile,
                                               bool incoming,
                                               bool v5_migration_source)
{
    const char *incoming_arg = incoming ? "-incoming defer" : "";
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S %s "
        "-device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,"
        "pic-runtime-profile=%s,pic-linked=on,pic-bedrock-interrupts=on,"
        "pic-v5-migration-source=%s "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1",
        incoming_arg, runtime_profile ? "on" : "off",
        v5_migration_source ? "on" : "off");

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    if (runtime_profile) {
        qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);
    }
    return qts;
}

static QTestState *pic_profile_v6_migration_start(bool incoming)
{
    const char *incoming_arg = incoming ? "-incoming defer" : "";
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S %s "
        "-device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,"
        "pic-runtime-profile=on,pic-linked=on,pic-bedrock-interrupts=on,"
        "pic-v6-migration-source=on "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1",
        incoming_arg);

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);
    return qts;
}

static QTestState *pic_profile_v7_migration_start(bool incoming,
                                                 bool previous_binary)
{
    const char *incoming_arg = incoming ? "-incoming defer" : "";
    const char *args = "-M none -cpu R10000 -S %s "
        "-device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,"
        "pic-runtime-profile=on,pic-linked=on,pic-bedrock-interrupts=on "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1";
    g_autofree char *formatted = g_strdup_printf(args, incoming_arg);
    QTestState *qts = previous_binary ?
        qtest_init_ext("QTEST_QEMU_BINARY_SRC", formatted, NULL, true) :
        qtest_initf("%s", formatted);

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);
    return qts;
}

static QTestState *pic_profile_width_migration_start(bool incoming,
                                                 bool previous_binary,
                                                 uint8_t req64_slots)
{
    const char *incoming_arg = incoming ? "-incoming defer" : "";
    const char *args = "-M none -cpu R10000 -S %s "
        "-device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,"
        "pic-runtime-profile=on,pic-linked=on,pic-bedrock-interrupts=on "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1";
    g_autofree char *formatted_args = g_strdup_printf(args, incoming_arg);
    g_autofree char *formatted = previous_binary ?
        g_strdup(formatted_args) :
        g_strdup_printf("%s -global sgi-pic.req64-slots-head0=0x%02x",
                        formatted_args, req64_slots);
    QTestState *qts = previous_binary ?
        qtest_init_ext("QTEST_QEMU_BINARY_SRC", formatted, NULL, true) :
        qtest_initf("%s", formatted);

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);
    return qts;
}

static QTestState *pic_bedrock_parent_start_full(bool map_io_registers,
                                                 bool runtime_profile,
                                                 bool incoming)
{
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -m 256M -S -d unimp,guest_errors "
        "-device sgi-xio-test-target,id=memory,size=16777216 "
        "-device sgi-bedrock,id=bedrock,processor-count=1,"
        "dma-memory=/machine/peripheral/memory/memory.ram[0] "
        "-device sgi-xio-test-target,id=fabric,size=16777216,pic-peer=on,"
        "pic-runtime-profile=%s,"
        "pic-linked=on,pic-switched=on,pic-bedrock-interrupts=on,"
        "target-port=/machine/peripheral/bedrock/io %s",
        runtime_profile ? "on" : "off", incoming ? "-incoming defer" : "");

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/bedrock", 0,
                          0x10000000);
    if (map_io_registers) {
        qtest_sysbus_mmio_map(qts, "/machine/peripheral/bedrock", 4,
                              0x3000000000000ULL);
    }
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 3, BASE);
    return qts;
}

static QTestState *pic_bedrock_parent_start(bool map_io_registers,
                                            bool runtime_profile)
{
    return pic_bedrock_parent_start_full(map_io_registers, runtime_profile,
                                         false);
}

static QTestState *pic_bedrock_start(bool runtime_profile)
{
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S -d unimp,guest_errors "
        "-device sgi-xio-test-target,id=memory,size=16777216,"
        "sparse-node-memory=on "
        "-device sgi-bedrock-io,id=serving,node-number=1,"
        "device-mask-reset=0x0000ffffffffffff,"
        "dma-memory=/machine/peripheral/memory/sparse-dma[0],"
        "hub-registers=/machine/peripheral/memory/memory.node1.ram[0] "
        "-device sgi-bedrock-io,id=ingress,node-number=0,"
        "dma-memory=/machine/peripheral/memory/sparse-dma[0],"
        "hub-registers=/machine/peripheral/memory/memory.ram[0] "
        "-device sgi-xio-test-target,id=fabric,size=16777216,pic-peer=on,"
        "pic-runtime-profile=%s,"
        "pic-bedrock-interrupts=on,target-port=ingress,"
        "target-peer=serving,pic-switched=on "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1,"
        "vendor-id=0xfeed,device-id=0x3500 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1,"
        "vendor-id=0xfeed,device-id=0x3501",
        runtime_profile ? "on" : "off");

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/memory", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/memory", 1,
                          TARGET_NODE1);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 3, BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/ingress", 0,
                          INGRESS_REGS);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/serving", 0,
                          SERVING_REGS);
    return qts;
}

static void pic_bedrock_configure_route(QTestState *qts,
                                        bool remote_register)
{
    uint64_t direct = 15ULL << 20;

    if (remote_register) {
        direct |= (II_REGISTER | II_NODE(1) | II_HUB_REMOTE) >> 31;
    }
    qtest_writel(qts, HEAD(0) + 0x21010, 0x01000000);
    qtest_writel(qts, HEAD(0) + 0x21004, 6);
    qtest_writeq(qts, HEAD(0) + 0x20, 14);
    qtest_writeq(qts, HEAD(0) + 0x80, direct);
    qtest_writel(qts, HEAD(1) + 0x21010, 0x01000000);
    qtest_writel(qts, HEAD(1) + 0x21004, 6);
    qtest_writeq(qts, HEAD(1) + 0x20, 15);
    qtest_writeq(qts, HEAD(1) + 0x80,
                 (14ULL << 20) | (remote_register ?
                 ((II_REGISTER | II_NODE(1) | II_HUB_REMOTE) >> 31) : 0));
}

static void pic_bedrock_configure(QTestState *qts, bool remote_register)
{
    pic_bedrock_configure_route(qts, remote_register);
    qtest_writel(qts, CARD(0), remote_register ? 0x81801000 : 0x80001000);
    qtest_writel(qts, CARD(0) + 4, 8);
    qtest_writel(qts, CARD(1), remote_register ? 0x81801000 : 0x80001000);
    qtest_writel(qts, CARD(1) + 4, 8);
}

static QTestState *pic_bedrock_ioc4_start(int serial_fd, bool incoming)
{
    g_autofree char *chardev = serial_fd >= 0 ?
        g_strdup_printf("-chardev socket,id=serial,fd=%d ", serial_fd) :
        g_strdup("");
    const char *incoming_arg = incoming ? "-incoming defer " : "";
    const char *serial_arg = serial_fd >= 0 ? ",chardev1=serial" : "";
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -d unimp,guest_errors "
        "%s"
        "-device sgi-xio-test-target,id=memory,size=16777216 "
        "-device sgi-bedrock-io,id=serving,node-number=1,"
        "device-mask-reset=0x0000ffffffffffff,"
        "dma-memory=/machine/peripheral/memory/memory.ram[0],"
        "hub-registers=/machine/peripheral/memory/memory.ram[0] "
        "-device sgi-bedrock-io,id=ingress,node-number=0,"
        "dma-memory=/machine/peripheral/memory/memory.ram[0],"
        "hub-registers=/machine/peripheral/memory/memory.ram[0] "
        "-device sgi-xio-test-target,id=fabric,size=16777216,pic-peer=on,"
        "target-port=ingress,target-peer=serving,pic-switched=on,"
        "pic-test-irq-route=on "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1,"
        "vendor-id=0xfeed,device-id=0x3501 "
        "%s"
        "-device sgi-ioc4,bus=pic-head-0.0,addr=1,clock-hz=33333333,"
        "serial-dma-experimental=on%s",
        incoming_arg, chardev, serial_arg);

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/memory", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 3, BASE);
    qtest_writeq(qts, HEAD(0) + 0x200, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x200), ==, 0);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/ingress", 0,
                          INGRESS_REGS);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/serving", 0,
                          SERVING_REGS);
    return qts;
}

static void pic_ioc4_check_register_endianness(QTestState *qts);

static void pic_bedrock_ioc4_enable(QTestState *qts)
{
    g_assert_cmphex(qtest_readl(qts, IOC4_CONFIG), ==, 0x100a10a9);
    qtest_writel(qts, IOC4_CONFIG + 4, PCI_COMMAND_MEMORY |
                 PCI_COMMAND_MASTER);
    qtest_writel(qts, IOC4_CONFIG + 0x10, IOC4_BAR);
    g_assert_cmphex(qtest_readl(qts, IOC4_CONFIG + 0x10), ==, IOC4_BAR);
    pic_ioc4_check_register_endianness(qts);
}

static void pic_ioc4_write_reg(QTestState *qts, unsigned offset,
                               uint32_t value)
{
    /* PIC forwards PCI memory bytes little-endian to IOC4's big-endian BAR. */
    qtest_writel(qts, IOC4_MEMORY + offset, bswap32(value));
}

static uint32_t pic_ioc4_read_reg(QTestState *qts, unsigned offset)
{
    return bswap32(qtest_readl(qts, IOC4_MEMORY + offset));
}

static void pic_bedrock_ioc4_set_dma_base(QTestState *qts)
{
    pic_ioc4_write_reg(qts, IOC4_DMA_BASE + 4, 0);
    pic_ioc4_write_reg(qts, IOC4_DMA_BASE, 0x80000000);
}

static void pic_ioc4_check_register_endianness(QTestState *qts)
{
    /*
     * A reload write also loads the idle count, which reads back in bits
     * 16-27, so the whole word tells a byte-swapped access apart.
     */
    pic_ioc4_write_reg(qts, IOC4_DMA_RX_TIMER, 0x3a5);
    g_assert_cmphex(pic_ioc4_read_reg(qts, IOC4_DMA_RX_TIMER), ==,
                    0x03a503a5);
    pic_ioc4_write_reg(qts, IOC4_DMA_RX_TIMER, 0);
}

static void pic_ioc4_wait_reg(QTestState *qts, unsigned offset, uint32_t mask,
                              uint32_t expected)
{
    unsigned i;

    for (i = 0; i < 1000; i++) {
        if ((pic_ioc4_read_reg(qts, offset) & mask) == expected) {
            return;
        }
        qtest_clock_step(qts, 62500);
        g_usleep(1000);
    }
    g_assert_cmphex(pic_ioc4_read_reg(qts, offset) & mask, ==, expected);
}

static void pic_ioc4_expect_bytes(QTestState *qts, const char *phase, int fd,
                                  const uint8_t *expected, size_t len)
{
    uint8_t actual[16];
    size_t received = 0;

    g_assert_cmpuint(len, <=, sizeof(actual));
    while (received < len) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 1000);
        ssize_t count;

        if (ready == 0) {
            g_test_message("IOC4 %s timed out: command=0x%08x BAR=0x%08x "
                           "LCR=0x%02x IIR=0x%02x MCR=0x%02x LSR=0x%02x "
                           "DMA control=0x%08x TX produce=0x%08x "
                           "TX consume=0x%08x RX produce=0x%08x",
                           phase,
                           qtest_readl(qts, IOC4_CONFIG + 4),
                           qtest_readl(qts, IOC4_CONFIG + 0x10),
                           qtest_readb(qts, IOC4_UART0 + 3),
                           qtest_readb(qts, IOC4_UART0 + 2),
                           qtest_readb(qts, IOC4_UART0 + 4),
                           qtest_readb(qts, IOC4_UART0 + 5),
                           pic_ioc4_read_reg(qts, IOC4_DMA_CONTROL),
                           pic_ioc4_read_reg(qts, IOC4_DMA_TX_PRODUCE),
                           pic_ioc4_read_reg(qts, 0x318),
                           pic_ioc4_read_reg(qts, IOC4_DMA_RX_PRODUCE));
        }
        g_assert_cmpint(ready, ==, 1);
        count = read(fd, actual + received, len - received);
        g_assert_cmpint(count, >, 0);
        received += count;
    }
    g_assert_cmpmem(actual, len, expected, len);
}

static void pic_ioc4_configure_interrupt(QTestState *qts)
{
    qtest_writeq(qts, SERVING_REGS + 0x120, UINT64_C(0x00ff000000000000));
    g_assert_cmphex(qtest_readq(qts, SERVING_REGS + 0x120), ==,
                    UINT64_C(0x00ffffffffffffff));
    qtest_writeq(qts, HEAD(0) + 0x20, 14);
    qtest_writeq(qts, HEAD(0) + 0x130, IOC4_IRQ_DESTINATION);
    qtest_writeq(qts, HEAD(0) + 0x118, 1);
}

static void pic_ioc4_raise_uart_interrupt(QTestState *qts)
{
    pic_ioc4_write_reg(qts, 0x18, UINT32_MAX);
    pic_ioc4_write_reg(qts, 0x1c, UINT32_MAX);
    pic_ioc4_write_reg(qts, 0x08, UINT32_MAX);
    pic_ioc4_write_reg(qts, 0x0c, UINT32_MAX);
    qtest_writeb(qts, IOC4_UART0 + 1, 2);
    g_assert_cmphex(pic_ioc4_read_reg(qts, 0x08) & IOC4_UART_PASS, ==,
                    IOC4_UART_PASS);
    pic_ioc4_write_reg(qts, 0x10, IOC4_UART_PASS);
}

static void test_bedrock_ioc4_serial_dma(void)
{
    int sv[2];
    QTestState *qts;
    const uint8_t pio = 0x51;
    const uint8_t tx[8] = { 0x52, 0, 0, 0, 0x40, 0, 0, 0 };
    const uint8_t rx[] = { 0x61, 0x62, 0x63 };
    const uint8_t tx_expected = 0x52;
    uint8_t entry[8];

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = pic_bedrock_ioc4_start(sv[1], false);
    close(sv[1]);
    pic_bedrock_configure_route(qts, false);
    pic_bedrock_ioc4_enable(qts);

    qtest_writeb(qts, IOC4_UART0 + 3, 3);
    qtest_writeb(qts, IOC4_UART0 + 2, 1);
    qtest_writeb(qts, IOC4_UART0 + 4, 0x03);
    qtest_writeb(qts, IOC4_UART0, pio);
    pic_ioc4_expect_bytes(qts, "PIO", sv[0], &pio, 1);

    pic_bedrock_ioc4_set_dma_base(qts);
    qtest_memwrite(qts, TARGET, tx, sizeof(tx));
    pic_ioc4_write_reg(qts, IOC4_DMA_CONTROL, IOC4_DMA_ENABLE);
    pic_ioc4_write_reg(qts, IOC4_DMA_TX_PRODUCE, 8);
    pic_ioc4_expect_bytes(qts, "TX DMA", sv[0], &tx_expected, 1);

    g_assert_cmpint(write(sv[0], rx, sizeof(rx)), ==, sizeof(rx));
    g_usleep(10000);
    pic_ioc4_write_reg(qts, IOC4_DMA_CONTROL,
                       IOC4_DMA_ENABLE | IOC4_DMA_DRAIN);
    pic_ioc4_wait_reg(qts, IOC4_DMA_RX_PRODUCE, 0xff8, 8);
    qtest_memread(qts, TARGET + 0x400, entry, sizeof(entry));
    g_assert_cmpmem(entry, 3, rx, sizeof(rx));
    g_assert_cmphex(entry[3], ==, 0);
    g_assert_cmphex(entry[4], ==, IOC4_RX_FROM_LINE);
    g_assert_cmphex(entry[5], ==, IOC4_RX_FROM_LINE);
    g_assert_cmphex(entry[6], ==, IOC4_RX_FROM_LINE);
    g_assert_cmphex(entry[7], ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

static void test_bedrock_ioc4_interrupt(void)
{
    QTestState *qts = pic_bedrock_ioc4_start(-1, false);

    pic_bedrock_configure_route(qts, false);
    pic_bedrock_ioc4_enable(qts);
    pic_ioc4_configure_interrupt(qts);
    qtest_writeq(qts, TARGET + 0x1000, 0x55);
    pic_ioc4_raise_uart_interrupt(qts);

    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 1);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0x55);
    g_assert_cmpuint(pic_requests_at(qts, PIC_CHILD, 0, 0, true), ==, 0);

    qtest_writeq(qts, HEAD(0) + 0x108, 1);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0x11234);
    g_assert_cmphex(target_metric(qts, "x-test-packet-address"), ==,
                    II_REGISTER | II_NODE(1) | II_HUB_REMOTE | 0x1000);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_OK);
    qtest_writeq(qts, HEAD(0) + 0x110, 1);
    g_assert_cmpuint(pic_requests_at(qts, PIC_CHILD, 0, 0, true), ==, 2);

    qtest_writeq(qts, HEAD(0) + 0x108, 0);
    qtest_writeq(qts, HEAD(0) + 0x108, 1);
    g_assert_cmpuint(pic_requests_at(qts, PIC_CHILD, 0, 0, true), ==, 3);
    g_assert_cmphex(qtest_readb(qts, IOC4_UART0 + 2), ==, 2);
    pic_ioc4_write_reg(qts, 0x08, IOC4_UART_PASS);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0x1234);
    g_assert_cmpuint(pic_requests_at(qts, PIC_CHILD, 0, 0, false), ==, 1);
    qtest_writeq(qts, HEAD(0) + 0x110, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 0);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x20), ==, 0);
    g_assert_cmphex(pic_ioc4_read_reg(qts, 0x08), ==, 0);
    qtest_quit(qts);
}

static void test_bedrock_ioc4_pending_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-ioc4-bedrock-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    int from_sv[2], to_sv[2];
    QTestState *from;
    QTestState *to;
    const uint8_t tx[8] = { 0x53, 0, 0, 0, 0x40, 0, 0, 0 };
    const uint8_t tx_expected = 0x53;
    const uint8_t rx[] = { 0x71, 0x72, 0x73 };
    uint8_t entry[8];
    const uint8_t serial_data = IOC4_UART_DATA;

    g_assert_no_error(error);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, from_sv), ==, 0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, to_sv), ==, 0);
    from = pic_bedrock_ioc4_start(from_sv[1], false);
    close(from_sv[1]);
    to = pic_bedrock_ioc4_start(to_sv[1], true);
    close(to_sv[1]);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);

    pic_bedrock_configure_route(from, false);
    pic_bedrock_ioc4_enable(from);
    pic_ioc4_configure_interrupt(from);
    pic_ioc4_raise_uart_interrupt(from);
    g_assert_cmphex(qtest_readq(from, HEAD(0) + 0x100), ==, 1);
    qtest_writeq(from, HEAD(0) + 0x108, 1);
    g_assert_cmphex(qtest_readq(from, TARGET + 0x1000), ==, 0x11234);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(pic_ioc4_read_reg(to, 0x08) & IOC4_UART_PASS, ==,
                    IOC4_UART_PASS);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x100), ==, 1);
    g_assert_cmphex(qtest_readq(to, TARGET + 0x1000), ==, 0x11234);

    /*
     * Restoring a pending source sends the PIC nothing: the line was high
     * and stays high. The pass-through bit is a level, so the UART read
     * that quiets the port lowers it, once, and the acknowledgement after
     * it changes nothing.
     */
    g_assert_cmpuint(pic_requests_at(to, PIC_CHILD, 0, 0, false), ==, 0);
    g_assert_cmpuint(pic_requests_at(to, PIC_CHILD, 0, 0, true), ==, 1);
    g_assert_cmphex(qtest_readb(to, IOC4_UART0 + 2), ==, 2);
    g_assert_cmpuint(pic_requests_at(to, PIC_CHILD, 0, 0, false), ==, 1);
    pic_ioc4_write_reg(to, 0x08, IOC4_UART_PASS);
    g_assert_cmphex(qtest_readq(to, TARGET + 0x1000), ==, 0x1234);
    g_assert_cmpuint(pic_requests_at(to, PIC_CHILD, 0, 0, false), ==, 1);
    qtest_writeq(to, HEAD(0) + 0x110, 1);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x100), ==, 0);

    qtest_writeb(to, IOC4_UART0 + 3, 3);
    qtest_writeb(to, IOC4_UART0 + 2, 1);
    qtest_writeb(to, IOC4_UART0 + 4, 0x03);
    qtest_writeb(to, IOC4_UART0, IOC4_UART_DATA);
    pic_ioc4_expect_bytes(to, "migration PIO", to_sv[0], &serial_data, 1);
    pic_bedrock_ioc4_set_dma_base(to);
    qtest_memwrite(to, TARGET, tx, sizeof(tx));
    pic_ioc4_write_reg(to, IOC4_DMA_CONTROL, IOC4_DMA_ENABLE);
    pic_ioc4_write_reg(to, IOC4_DMA_TX_PRODUCE, 8);
    pic_ioc4_expect_bytes(to, "migration TX DMA", to_sv[0], &tx_expected, 1);
    g_assert_cmpint(write(to_sv[0], rx, sizeof(rx)), ==, sizeof(rx));
    g_usleep(10000);
    pic_ioc4_write_reg(to, IOC4_DMA_CONTROL,
                       IOC4_DMA_ENABLE | IOC4_DMA_DRAIN);
    pic_ioc4_wait_reg(to, IOC4_DMA_RX_PRODUCE, 0xff8, 8);
    qtest_memread(to, TARGET + 0x400, entry, sizeof(entry));
    g_assert_cmpmem(entry, 3, rx, sizeof(rx));
    g_assert_cmphex(entry[4], ==, IOC4_RX_FROM_LINE);
    g_assert_cmphex(entry[5], ==, IOC4_RX_FROM_LINE);
    g_assert_cmphex(entry[6], ==, IOC4_RX_FROM_LINE);

    qtest_qmp_assert_success(to, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x100), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x20), ==, 0);
    g_assert_cmphex(pic_ioc4_read_reg(to, 0x08), ==, 0);

    qtest_quit(from);
    qtest_quit(to);
    close(from_sv[0]);
    close(to_sv[0]);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void pic_bedrock_dma(QTestState *qts, uint32_t pattern)
{
    qtest_writel(qts, CARD(0) + 8, pattern);
    qtest_writel(qts, CARD(0) + 0xc, 1);
}

static void test_linked_receiving_ii_denial(void)
{
    QTestState *qts = pic_bedrock_start(false);
    uint8_t sentinel[8];
    uint8_t bytes[8];

    memset(sentinel, 0xa5, sizeof(sentinel));
    qtest_memwrite(qts, TARGET + 0x1000, sentinel, sizeof(sentinel));
    qtest_memwrite(qts, TARGET_NODE1 + 0x1000, sentinel, sizeof(sentinel));
    pic_bedrock_configure(qts, false);
    qtest_writeq(qts, INGRESS_REGS + 0x118,
                 qtest_readq(qts, INGRESS_REGS + 0x118) & ~(1ULL << 14));
    pic_bedrock_dma(qts, 0x61);
    qtest_memread(qts, TARGET_NODE1 + 0x1000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), sentinel, sizeof(sentinel));
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), sentinel, sizeof(sentinel));
    g_assert_cmphex(qtest_readl(qts, CARD(0) + 8), ==, 0x61);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 1);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-address"), ==,
                     XIO_WIDGET(15) | 0x1000);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 14);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-device-known"), ==,
                     0);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_ACCESS_ERROR);
    g_assert_cmpuint(pic_metric(qts, "x-test-linked-packet-count"), ==, 1);

    qtest_writeq(qts, INGRESS_REGS + 0x118, UINT64_MAX);
    pic_bedrock_dma(qts, 0x71);
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x71 + i);
    }
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 2);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-address"), ==,
                     XIO_WIDGET(15) | 0x1000);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 14);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_OK);
    qtest_quit(qts);
}

static void test_linked_serving_ii_denial(void)
{
    QTestState *qts = pic_bedrock_start(false);
    uint8_t sentinel[8];
    uint8_t bytes[8];

    memset(sentinel, 0xa5, sizeof(sentinel));
    qtest_memwrite(qts, TARGET + 0x1000, sentinel, sizeof(sentinel));
    qtest_memwrite(qts, TARGET_NODE1 + 0x1000, sentinel, sizeof(sentinel));
    pic_bedrock_configure(qts, true);

    /*
     * Refuse at ingress first, then prove ingress approval cannot bypass the
     * serving node's own device mask for this same remote register address.
     */
    qtest_writeq(qts, INGRESS_REGS + 0x118,
                 qtest_readq(qts, INGRESS_REGS + 0x118) & ~(1ULL << 14));
    pic_bedrock_dma(qts, 0x61);
    qtest_writeq(qts, INGRESS_REGS + 0x118, UINT64_MAX);
    pic_bedrock_dma(qts, 0x71);
    qtest_memread(qts, TARGET_NODE1 + 0x1000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), sentinel, sizeof(sentinel));
    g_assert_cmphex(qtest_readl(qts, CARD(0) + 8), ==, 0x71);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 2);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-address"), ==,
                     XIO_WIDGET(15) | II_REGISTER | II_NODE(1) |
                     II_HUB_REMOTE | 0x1000);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 14);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-device-known"), ==,
                     0);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_ACCESS_ERROR);
    g_assert_cmpuint(pic_metric(qts, "x-test-linked-packet-count"), ==, 2);

    qtest_writel(qts, CARD(1) + 8, 0x71);
    qtest_writel(qts, CARD(1) + 0xc, 1);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-address"), ==,
                     XIO_WIDGET(14) | II_REGISTER | II_NODE(1) |
                     II_HUB_REMOTE | 0x1000);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 15);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 3);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_ACCESS_ERROR);

    qtest_writeq(qts, SERVING_REGS + 0x120, UINT64_MAX);
    pic_bedrock_dma(qts, 0x81);
    qtest_memread(qts, TARGET_NODE1 + 0x1000, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x81 + i);
    }
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 4);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_OK);
    qtest_writel(qts, CARD(1) + 8, 0x91);
    qtest_writel(qts, CARD(1) + 0xc, 1);
    qtest_memread(qts, TARGET_NODE1 + 0x1000, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x91 + i);
    }
    g_assert_cmpuint(target_metric(qts, "x-test-packet-address"), ==,
                     XIO_WIDGET(14) | II_REGISTER | II_NODE(1) |
                     II_HUB_REMOTE | 0x1000);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 15);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 5);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_OK);
    qtest_quit(qts);
}

static void test_direct64_bedrock_high_payload_source_denial(void)
{
    const uint64_t swap = UINT64_C(1) << 55;
    const uint64_t local_destination = (UINT64_C(0xf) << 60) | swap | 0x1000;
    const uint64_t destination = (UINT64_C(0xf) << 60) | swap |
                                (UINT64_C(1) << 33) | 0x1000;
    uint8_t sentinel[8], bytes[8];
    QTestState *qts = pic_bedrock_start(true);

    memset(sentinel, 0xa5, sizeof(sentinel));
    qtest_memwrite(qts, TARGET + 0x1000, sentinel, sizeof(sentinel));
    qtest_memwrite(qts, TARGET_NODE1 + 0x1000, sentinel, sizeof(sentinel));
    direct64_card_enable(qts, 0);
    qtest_writeq(qts, HEAD(0) + 0x20, 14);

    direct64_card_write(qts, 0, local_destination, sizeof(sentinel), 0x31);
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0x31 + i);
    }
    g_assert_cmphex(target_metric(qts, "x-test-packet-address"), ==,
                    XIO_WIDGET(15) | 0x1000);

    qtest_memwrite(qts, TARGET + 0x1000, sentinel, sizeof(sentinel));
    qtest_writeq(qts, INGRESS_REGS + 0x118,
                 qtest_readq(qts, INGRESS_REGS + 0x118) & ~(1ULL << 14));
    direct64_card_write(qts, 0, destination, sizeof(sentinel), 0x61);
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), sentinel, sizeof(sentinel));
    qtest_memread(qts, TARGET_NODE1 + 0x1000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), sentinel, sizeof(sentinel));
    g_assert_cmphex(qtest_readl(qts, CARD(0) + 0x28), ==,
                    MEMTX_ACCESS_ERROR);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_ACCESS_ERROR);
    g_assert_cmphex(target_metric(qts, "x-test-packet-address"), ==,
                    XIO_WIDGET(15) | (UINT64_C(1) << 33) | 0x1000);

    qtest_writeq(qts, INGRESS_REGS + 0x118, UINT64_MAX);
    direct64_card_write(qts, 0, destination, sizeof(sentinel), 0x71);
    qtest_memread(qts, TARGET_NODE1 + 0x1000, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0x71 + i);
    }
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), sentinel, sizeof(sentinel));

    qtest_memwrite(qts, TARGET + 0x1000, sentinel, sizeof(sentinel));
    qtest_memwrite(qts, TARGET_NODE1 + 0x1000, sentinel, sizeof(sentinel));
    direct64_card_write(qts, 0,
                        (UINT64_C(0xf) << 60) | swap |
                        (UINT64_C(2) << 33) | 0x1000,
                        sizeof(sentinel), 0x81);
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), sentinel, sizeof(sentinel));
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_DECODE_ERROR);
    g_assert_cmphex(target_metric(qts, "x-test-packet-address"), ==,
                    XIO_WIDGET(15) | (UINT64_C(2) << 33) | 0x1000);

    qtest_memwrite(qts, TARGET + 0x1000, sentinel, sizeof(sentinel));
    qtest_memwrite(qts, TARGET_NODE1 + 0x1000, sentinel, sizeof(sentinel));
    direct64_card_write(qts, 0, (UINT64_C(0xf) << 60) | swap |
                        (UINT64_C(1) << 33) | 0x01000000,
                        sizeof(sentinel), 0x91);
    g_assert_cmphex(qtest_readl(qts, CARD(0) + 0x28), ==,
                    MEMTX_DECODE_ERROR);
    qtest_memread(qts, TARGET_NODE1 + 0x1000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), sentinel, sizeof(sentinel));
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), sentinel, sizeof(sentinel));
    qtest_quit(qts);
}

static void assert_pic_identity_word(uint64_t value, unsigned head,
                                     uint32_t manufacturer, uint32_t low_bit)
{
    g_assert_cmphex((value >> 28) & 0xf, ==, 3);
    g_assert_cmphex((value >> 12) & 0xffff, ==,
                    head ? 0xd112 : 0xd102);
    g_assert_cmphex((value & 0xffe) >> 1, ==, manufacturer);
    g_assert_cmphex(value & 1, ==, low_bit);
}

static void assert_pic_identity(QTestState *qts, uint32_t manufacturer)
{
    for (unsigned head = 0; head < 2; head++) {
        assert_pic_identity_word(qtest_readq(qts, HEAD(head)), head,
                                 manufacturer, 0);
        assert_pic_identity_word(qtest_readl(qts, HEAD(head) + 4), head,
                                 manufacturer, 0);
    }
}

static void test_identity_fields(void)
{
    QTestState *qts = pic_start(false, "");

    assert_pic_identity(qts, 0x24);
    qtest_writeq(qts, HEAD(0) + 0x20, 5);
    qtest_writeq(qts, HEAD(1) + 0x20, 6);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    assert_pic_identity(qts, 0x24);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x20), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x20), ==, 0);
    qtest_quit(qts);
}

static void assert_runtime_pic_identity(QTestState *qts)
{
    for (unsigned head = 0; head < 2; head++) {
        assert_pic_identity_word(qtest_readq(qts, HEAD(head)), head,
                                 0x24, 1);
        assert_pic_identity_word(qtest_readl(qts, HEAD(head) + 4), head,
                                 0x24, 1);
    }
}

static void test_runtime_identity_bit(void)
{
    QTestState *qts = pic_runtime_start(false, NULL);

    assert_runtime_pic_identity(qts);
    qtest_writeq(qts, HEAD(0) + 0x20, 5);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    assert_runtime_pic_identity(qts);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x20), ==, 0x0f00320f);
    qtest_quit(qts);
}

static void test_runtime_host_error_field(void)
{
    QTestState *qts = pic_runtime_cpu_start(false, NULL);

    for (unsigned head = 0; head < 2; head++) {
        uint64_t value = UINT64_C(0x1234567800000000) | (head + 1);

        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x128), ==, 0);
        qtest_writeq(qts, HEAD(head) + 0x128, value);
        g_assert_cmphex(qtest_readl(qts, CPU_HEAD(head) + 0x12c), ==,
                        head + 1);
        qtest_writel(qts, CPU_HEAD(head) + 0x12c, 0x55 + head);
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x128), ==,
                        UINT64_C(0x1234567800000000) | (0x55 + head));
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x100), ==, 0);
    }
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    for (unsigned head = 0; head < 2; head++) {
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x128), ==, 0);
    }
    qtest_quit(qts);
}

static void test_cpu_word_view_registers(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-cpu-word-view-XXXXXX",
                                          &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log_arg = NULL;
    g_autofree char *log_contents = NULL;
    QTestState *qts;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    log_arg = g_strdup_printf("-D %s", log_path);
    qts = pic_runtime_cpu_start(true, log_arg);

    for (unsigned head = 0; head < 2; head++) {
        uint64_t packet_head = HEAD(head);
        uint64_t cpu_config = CPU_HEAD(head) + 0x21000;
        uint64_t packet_config = packet_head + 0x21000;
        uint64_t upper = UINT64_C(0x1234567800000000) | head;

        pic_short_enable_card(qts, head, 1, 0x03000000,
                              PIC_SHORT_MEMORY_BASE, 0x3001,
                              PCI_COMMAND_MEMORY | PCI_COMMAND_IO);

        qtest_writel(qts, cpu_config + 4,
                     PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        g_assert_cmphex(qtest_readl(qts, cpu_config + 4) & 0xffff, ==,
                        PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        g_assert_cmphex(qtest_readl(qts, packet_config + 4) & 0xffff, ==,
                        PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
        qtest_writel(qts, cpu_config + 0x14, 0x04000000);
        g_assert_cmphex(qtest_readl(qts, cpu_config + 0x14), ==,
                        0x04000000);
        g_assert_cmphex(qtest_readl(qts, packet_config + 0x14), ==,
                        0x04000000);
        qtest_writeq(qts, BASE +
                     (head ? 0xc0000000ULL : 0x40000000ULL) +
                     0x04000008, UINT64_C(0x1234abcd5678ef01) | head);
        g_assert_cmphex(qtest_readq(qts, BASE +
                                   (head ? 0xc0000000ULL : 0x40000000ULL) +
                                   0x04000008), ==,
                        UINT64_C(0x1234abcd5678ef01) | head);
        qtest_writel(qts, cpu_config + 0x14, PIC_SHORT_MEMORY_BASE);
        g_assert_cmphex(qtest_readl(qts, cpu_config + 0x14), ==,
                        PIC_SHORT_MEMORY_BASE);
        g_assert_cmphex(qtest_readl(qts, packet_config + 0x14), ==,
                        PIC_SHORT_MEMORY_BASE);

        g_assert_cmphex(qtest_readl(qts, CPU_HEAD(head) + 4), ==,
                        (uint32_t)qtest_readq(qts, CPU_HEAD(head)));
        g_assert_cmphex(qtest_readl(qts, packet_head + 4), ==,
                        (uint32_t)qtest_readq(qts, packet_head));
        qtest_writel(qts, CPU_HEAD(head) + 4, 0);
        g_assert_cmphex(qtest_readl(qts, CPU_HEAD(head) + 4), ==,
                        (uint32_t)qtest_readq(qts, CPU_HEAD(head)));
        qtest_writeq(qts, CPU_HEAD(head) + 0x130, upper | 0x11223344);
        g_assert_cmphex(qtest_readl(qts, CPU_HEAD(head) + 0x134), ==,
                        0x11223344 | head);
        qtest_writel(qts, CPU_HEAD(head) + 0x134, 0xaabbccdd);
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x130), ==,
                        upper | 0xaabbccdd);
        qtest_writeq(qts, packet_head + 0x130,
                     upper | 0x76543210);
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x130), ==,
                        upper | 0x76543210);
        g_assert_cmphex(qtest_readl(qts, packet_head + 0x134), ==, 0);
        qtest_writel(qts, packet_head + 0x134, 0xdeadbeef);
        g_assert_cmphex(qtest_readq(qts, packet_head + 0x130), ==,
                        upper | 0x76543210);

        qtest_writel(qts, CPU_HEAD(head) + 0x204,
                     0x80000000U | PIC_SHORT_MEMORY_BASE / 0x100000 |
                     (1U << 12));
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x200), ==,
                        PIC_SHORT_MEMORY_BASE / 0x100000 | (1U << 12));
        qtest_writeq(qts, CPU_HEAD(head) + 0x200000 + 8,
                     UINT64_C(0x8877665544332211) | head);
        g_assert_cmphex(qtest_readq(qts, BASE +
                                   (head ? 0xc0000000ULL : 0x40000000ULL) +
                                   PIC_SHORT_MEMORY_BASE + 8), ==,
                        UINT64_C(0x8877665544332211) | head);
        qtest_writel(qts, CPU_HEAD(head) + 0x200000 + 12, 0xaabbccdd);
        g_assert_cmphex(qtest_readl(qts, CPU_HEAD(head) + 0x200000 + 12), ==,
                        0xaabbccdd);
        g_assert_cmphex(qtest_readl(qts, BASE +
                                   (head ? 0xc0000000ULL : 0x40000000ULL) +
                                   PIC_SHORT_MEMORY_BASE + 12), ==,
                        0xaabbccdd);

        for (unsigned bank = 0; bank < 3; bank++) {
            uint64_t request_upper = UINT64_C(0xfedcba9800000000);
            uint64_t request = 0x18000 + bank * 0x800 + head * 8;

            qtest_writeq(qts, CPU_HEAD(head) + request,
                         request_upper | 0x55667788 | head);
            g_assert_cmphex(qtest_readl(qts, CPU_HEAD(head) + request + 4), ==,
                            0x55667788 | head);
            qtest_writel(qts, CPU_HEAD(head) + request + 4, 0x99aabbcc);
            g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + request), ==,
                            request_upper | 0x99aabbcc);
        }

        qtest_writeq(qts, CPU_HEAD(head) + 0x20,
                     UINT64_C(0x1234567800000005));
        qtest_writel(qts, CPU_HEAD(head) + 0x24, 0x10a);
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x20), ==, 0xa);
        qtest_writeq(qts, CPU_HEAD(head) + 0x80,
                     UINT64_C(0x1234567800000007));
        qtest_writel(qts, CPU_HEAD(head) + 0x84, 0x80012345);
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x80), ==,
                        0x12345);
        qtest_writeq(qts, CPU_HEAD(head) + 0xc8,
                     UINT64_C(0x1234567800000000) | 0x100);
        qtest_writel(qts, CPU_HEAD(head) + 0xcc, 0x2345);
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0xc8), ==,
                        UINT64_C(0x1234567800002345));

        qtest_writeq(qts, CPU_HEAD(head) + 0x100, 0);
        qtest_writel(qts, CPU_HEAD(head) + 0x104, 0xffffffff);
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x100), ==, 0);
        qtest_writeq(qts, CPU_HEAD(head) + 0x120, 0x1234);
        qtest_writel(qts, CPU_HEAD(head) + 0x124, 0x5678);
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x120), ==, 0x1234);
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x50), ==, 0);
        qtest_writel(qts, CPU_HEAD(head) + 0x54, 0x9876);
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x50), ==, 0);
        qtest_writel(qts, CPU_HEAD(head) + 0x618, 0xfeed);
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x618), ==, 0);
    }

    qtest_writel(qts, CPU_HEAD(0) + 0x24, 0x5);
    uint64_t control_before[2] = {
        qtest_readq(qts, CPU_HEAD(0) + 0x20),
        qtest_readq(qts, CPU_HEAD(1) + 0x20),
    };
    g_assert_cmphex(control_before[0], ==, 0x5);
    g_assert_cmphex(control_before[1], ==, 0xa);
    qtest_writel(qts, CPU_HEAD(0) + 0x20, 0xffffffff);
    qtest_writel(qts, CPU_HEAD(0) + 0x26, 0xffffffff);
    qtest_writel(qts, CPU_HEAD(0) + 0x22, 0xffffffff);
    qtest_writew(qts, CPU_HEAD(0) + 0x24, 0xffff);
    qtest_writel(qts, CPU_HEAD(0) + 0x7ffffc, 0xffffffff);
    qtest_writel(qts, CPU_HEAD(0) + 0x7ffffe, 0xffffffff);
    for (unsigned head = 0; head < 2; head++) {
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x20), ==,
                        control_before[head]);
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log_contents,
                            "offset 0x20 size 8 value 0x10a unsupported"));
    g_assert_nonnull(strstr(log_contents,
                            "offset 0x80 size 8 value 0x80012345 unsupported"));
    g_assert_nonnull(strstr(log_contents,
                            "offset 0x200 size 8 value 0x80001020 "
                            "unsupported"));
    unlink(log_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_cpu_word_view_profile_isolation(void)
{
    QTestState *qts = pic_start(false, "");

    qtest_sysbus_mmio_map(qts, PIC, 0, CPU_BASE);
    qtest_writeq(qts, CPU_HEAD(0) + 0x20, 0x5);
    qtest_writel(qts, CPU_HEAD(0) + 0x24, 0x5678);
    g_assert_cmphex(qtest_readq(qts, CPU_HEAD(0) + 0x20), ==, 0x5);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(0) + 0x24), ==, 0);
    g_assert_cmphex(qtest_readl(qts, CPU_HEAD(0) + 4), ==,
                    (uint32_t)qtest_readq(qts, CPU_HEAD(0)));
    for (unsigned head = 0; head < 2; head++) {
        for (unsigned slot = 0x28; slot <= 0x38; slot += 8) {
            uint64_t address = CPU_HEAD(head) + slot;

            g_assert_cmphex(qtest_readq(qts, address), ==, 0);
            qtest_writeq(qts, address, 0x12345678);
            g_assert_cmphex(qtest_readq(qts, address), ==, 0);
            g_assert_cmphex(qtest_readl(qts, address + 4), ==, 0);
            qtest_writel(qts, address + 4, 0xdeadbeef);
            g_assert_cmphex(qtest_readq(qts, address), ==, 0);
        }
    }
    qtest_quit(qts);
}

static void test_widget_registers(void)
{
    QTestState *qts = pic_runtime_cpu_start(false, NULL);
    static const uint64_t seed[] = { 0xfffff, 0, 0 };

    for (unsigned head = 0; head < 2; head++) {
        for (unsigned reg = 0; reg < G_N_ELEMENTS(seed); reg++) {
            uint64_t slot = 0x28 + reg * 8;
            uint64_t initial = (UINT64_C(0x1234567800000000) | seed[reg]) ^
                               ((uint64_t)head << 40);
            uint32_t low = reg == 1 && head ? 0x5a5a5a5a : 0xa5a5a5a5;

            g_assert_cmphex(qtest_readq(qts, BASE + head * 0x800000 + slot),
                            ==, seed[reg]);
            qtest_writeq(qts, BASE + head * 0x800000 + slot, initial);
            g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + slot), ==,
                            initial);
            qtest_writel(qts, CPU_HEAD(head) + slot + 4, low);
            g_assert_cmphex(qtest_readl(qts, CPU_HEAD(head) + slot + 4), ==,
                            low);
            g_assert_cmphex(qtest_readq(qts, BASE + head * 0x800000 + slot),
                            ==, (initial & UINT64_C(0xffffffff00000000)) | low);
            qtest_writeq(qts, CPU_HEAD(head) + slot, initial ^ 0x55aa55aa);
            g_assert_cmphex(qtest_readq(qts, BASE + head * 0x800000 + slot),
                            ==, initial ^ 0x55aa55aa);

            /* Narrow packet controls stay refused and cannot change storage. */
            qtest_writel(qts, BASE + head * 0x800000 + slot + 4, 0xdeadbeef);
            g_assert_cmphex(qtest_readl(qts,
                                        BASE + head * 0x800000 + slot + 4),
                            ==, 0);
            g_assert_cmphex(qtest_readq(qts,
                                       BASE + head * 0x800000 + slot),
                            ==, initial ^ 0x55aa55aa);

            uint64_t packet_addr = BASE + head * 0x800000 + slot;
            uint64_t cpu_addr = CPU_HEAD(head) + slot;

            qtest_writeb(qts, packet_addr, 0x7e);
            qtest_writew(qts, packet_addr, 0x7e5a);
            qtest_writel(qts, packet_addr, 0x7e5a3c1d);
            qtest_writel(qts, packet_addr + 2, 0xdeadbeef);
            g_assert_cmphex(qtest_readb(qts, packet_addr), ==, 0);
            g_assert_cmphex(qtest_readw(qts, packet_addr), ==, 0);
            g_assert_cmphex(qtest_readl(qts, packet_addr), ==, 0);
            g_assert_cmphex(qtest_readl(qts, packet_addr + 2), ==, 0);

            qtest_writeb(qts, cpu_addr, 0x7e);
            qtest_writew(qts, cpu_addr, 0x7e5a);
            qtest_writel(qts, cpu_addr, 0x7e5a3c1d);
            qtest_writel(qts, cpu_addr + 2, 0xdeadbeef);
            g_assert_cmphex(qtest_readb(qts, cpu_addr), ==, 0);
            g_assert_cmphex(qtest_readw(qts, cpu_addr), ==, 0);
            g_assert_cmphex(qtest_readl(qts, cpu_addr), ==, 0);
            g_assert_cmphex(qtest_readl(qts, cpu_addr + 2), ==, 0);
            g_assert_cmphex(qtest_readq(qts, packet_addr), ==,
                            initial ^ 0x55aa55aa);
        }
    }

    unsigned active_head = 0;
    unsigned pin = 0;
    uint64_t pending_bit = 1;
    uint64_t destination = (UINT64_C(0x34) << 48) | II_REGISTER |
                           II_NODE(0) | II_HUB_REMOTE | 0x90;

    qtest_writeq(qts, CPU_HEAD(active_head) + 0x130, destination);
    qtest_writel(qts, CPU_HEAD(active_head) + 0x11c, 1);
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, pin, 1);
    qtest_writel(qts, CPU_HEAD(active_head) + 0x10c, 1);
    uint64_t status = qtest_readq(qts, CPU_HEAD(active_head) + 0x100);
    uint32_t set_requests = pic_requests_at(qts, PIC_CHILD, active_head, pin,
                                             true);
    uint32_t clear_requests = pic_requests_at(qts, PIC_CHILD, active_head, pin,
                                               false);
    uint64_t calls = target_metric(qts, "x-test-packet-calls");
    qtest_writeq(qts, BASE + 0x30, UINT64_C(0x123456789abcdef0));
    qtest_writeq(qts, BASE + 0x38, UINT64_C(0xfedcba9876543210));
    qtest_writeq(qts, CPU_HEAD(0) + 0x30, UINT64_C(0x123456789abcdef0));
    qtest_writeq(qts, CPU_HEAD(0) + 0x38, UINT64_C(0xfedcba9876543210));
    qtest_writel(qts, CPU_HEAD(0) + 0x34, 0x0badcafe);
    qtest_writel(qts, CPU_HEAD(0) + 0x3c, 0x0badcafe);
    g_assert_cmphex(qtest_readq(qts, CPU_HEAD(active_head) + 0x100), ==,
                    status);
    g_assert_cmphex(status & pending_bit, ==, pending_bit);
    g_assert_cmpuint(pic_requests_at(qts, PIC_CHILD, active_head, pin, true),
                     ==,
                     set_requests);
    g_assert_cmpuint(pic_requests_at(qts, PIC_CHILD, active_head, pin, false),
                     ==,
                     clear_requests);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, calls);

    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    for (unsigned head = 0; head < 2; head++) {
        g_assert_cmphex(qtest_readq(qts, BASE + head * 0x800000 + 0x28), ==,
                        0xfffff);
        g_assert_cmphex(qtest_readq(qts, BASE + head * 0x800000 + 0x30), ==,
                        0);
        g_assert_cmphex(qtest_readq(qts, BASE + head * 0x800000 + 0x38), ==,
                        0);
        g_assert_cmphex(qtest_readl(qts, CPU_HEAD(head) + 0x2c), ==,
                        0xfffff);
    }
    qtest_quit(qts);
}

static void test_cpu_word_view_interrupt_effects(void)
{
    QTestState *qts = pic_bedrock_parent_start(false, true);

    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);
    for (unsigned head = 0; head < 2; head++) {
        unsigned level = 0x34 + head;
        uint64_t pending_bit = UINT64_C(1) << level;
        uint64_t address = II_REGISTER | II_NODE(0) | II_HUB_REMOTE | 0x90;
        uint64_t destination = ((uint64_t)level << 48) | address;
        uint64_t calls = target_metric(qts, "x-test-packet-calls");
        g_autofree char *head_path = g_strdup_printf(
            PIC_CHILD "/head-%u", head);

        qtest_writeq(qts, CPU_HEAD(head) + 0x130, destination);
        qtest_writel(qts, CPU_HEAD(head) + 0x134, (uint32_t)destination);
        qtest_writel(qts, CPU_HEAD(head) + 0x11c, 1);
        qtest_set_irq_in(qts, head_path, NULL, 0, 1);
        qtest_set_irq_in(qts, head_path, NULL, 1, 1);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                         calls);

        qtest_writel(qts, CPU_HEAD(head) + 0x10c, 1);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                         calls + 1);
        g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & pending_bit,
                        ==, pending_bit);
        qtest_writel(qts, CPU_HEAD(head) + 0x10c, 1);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                         calls + 1);

        qtest_writel(qts, CPU_HEAD(head) + 0x114, 1);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                         calls + 2);
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x100) & 3, ==, 3);
        g_assert_cmphex(qtest_readl(qts, CPU_HEAD(head) + 0x104) & 3, ==, 3);
        qtest_set_irq_in(qts, head_path, NULL, 0, 0);
        qtest_writel(qts, CPU_HEAD(head) + 0x114, 1);
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x100) & 3, ==, 2);
        g_assert_cmphex(qtest_readl(qts, CPU_HEAD(head) + 0x104) & 3, ==, 2);
        qtest_set_irq_in(qts, head_path, NULL, 1, 0);
        qtest_writel(qts, CPU_HEAD(head) + 0x114, 2);
        g_assert_cmphex(qtest_readq(qts, CPU_HEAD(head) + 0x100) & 3, ==, 0);
        g_assert_cmphex(qtest_readl(qts, CPU_HEAD(head) + 0x104) & 3, ==, 0);
    }
    qtest_quit(qts);
}

static QTestState *pic_identity_migration_start(bool incoming,
                                                bool old_source)
{
    const char *incoming_arg = incoming ? "-incoming defer" : "";
    g_autofree char *args = NULL;
    QTestState *qts;

    args = g_strdup_printf("-M none -cpu R10000 -S %s "
        "-device sgi-xio-test-target,id=fabric,size=65536 "
        "-device sgi-pic,id=pic,test-fixture=on,"
        "target-0=/machine/peripheral/fabric/fabric.ram[0]", incoming_arg);
    if (old_source) {
        qts = qtest_init_ext("QTEST_QEMU_BINARY_SRC", args, NULL, true);
    } else {
        qts = qtest_initf("%s", args);
    }
    qtest_sysbus_mmio_map(qts, PIC, 0, BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    return qts;
}

static void test_identity_migration(bool old_source)
{
    GError *error = NULL;
    g_autofree char *dir = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from;
    QTestState *to;

    if (old_source && !g_getenv("QTEST_QEMU_BINARY_SRC")) {
        g_test_skip("QTEST_QEMU_BINARY_SRC is not set");
        return;
    }
    dir = g_dir_make_tmp("sgi-pic-identity-XXXXXX", &error);
    g_assert_no_error(error);
    from = pic_identity_migration_start(false, old_source);
    to = pic_identity_migration_start(true, false);
    assert_pic_identity(from, old_source ? 0x12 : 0x24);
    qtest_writeq(from, HEAD(0) + 0x20, 5);
    qtest_writeq(from, HEAD(1) + 0x20, 6);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    assert_pic_identity(to, 0x24);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x20), ==, 5);
    g_assert_cmphex(qtest_readq(to, HEAD(1) + 0x20), ==, 6);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_current_identity_migration(void)
{
    test_identity_migration(false);
}

static void test_old_identity_migration(void)
{
    test_identity_migration(true);
}

static void test_heads_and_widths(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-width-XXXXXX", &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log_contents = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    extra = g_strdup_printf("-D %s", log_path);
    qts = pic_start(false, extra);

    assert_pic_identity(qts, 0x24);
    qtest_writeq(qts, HEAD(0) + 0x20, 0x123456789abcdef3ULL);
    qtest_writeq(qts, HEAD(1) + 0x20, 0xfedcba9876543217ULL);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x20), ==, 3);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x20), ==, 7);
    qtest_writeq(qts, HEAD(0) + 0x108, 1);
    qtest_writeq(qts, HEAD(1) + 0x108, 0x80);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x108), ==, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x108), ==, 0x80);
    qtest_writeb(qts, HEAD(0) + 0x108, 0xff);
    qtest_writew(qts, HEAD(0) + 0x108, 0xffff);
    qtest_writel(qts, HEAD(0) + 0x108, 0xffffffff);
    qtest_writel(qts, HEAD(0) + 0x10c, 0xffffffff);
    g_assert_cmphex(qtest_readb(qts, HEAD(0) + 0x108), ==, 0);
    g_assert_cmphex(qtest_readw(qts, HEAD(0) + 0x108), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x108), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x108), ==, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x108), ==, 0x80);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio write offset 0x108 size 1"));
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio write offset 0x108 size 2"));
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio write offset 0x108 size 4"));
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio write offset 0x10c size 4"));
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio read offset 0x108 size 1"));
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio read offset 0x108 size 2"));
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio read offset 0x108 size 4"));
    unlink(log_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_configuration(void)
{
    QTestState *qts = pic_start(true, "");

    /* These IDs belong only to the explicitly selected synthetic fixture. */
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x20000), ==, 0x3502feed);
    g_assert_cmphex(qtest_readl(qts, HEAD(1) + 0x20000), ==, 0x3503feed);
    qtest_writel(qts, HEAD(0) + 0x20004, PCI_COMMAND_MEMORY);
    qtest_writel(qts, HEAD(0) + 0x20010, 0x01000000);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x20004) &
                    PCI_COMMAND_MEMORY, ==, PCI_COMMAND_MEMORY);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x20010), ==, 0x01000000);
    g_assert_cmphex(qtest_readl(qts, HEAD(1) + 0x20010), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x21000), ==, 0x3500feed);
    g_assert_cmphex(qtest_readl(qts, HEAD(1) + 0x21000), ==, 0x3501feed);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x22000), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x21100), ==, 0xffffffff);
    g_assert_cmphex(qtest_readb(qts, HEAD(0) + 0x21000), ==, 0x35);
    g_assert_cmphex(qtest_readb(qts, HEAD(0) + 0x21003), ==, 0xed);
    qtest_writeb(qts, HEAD(0) + 0x21007, 5);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x21004) & 7, ==, 5);
    qtest_writew(qts, HEAD(0) + 0x21006, 6);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x21004) & 7, ==, 6);
    qtest_writel(qts, HEAD(0) + 0x21004, 7);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x21004) & 7, ==, 7);
    qtest_writeq(qts, HEAD(0) + 0xc8, (2ULL << 16) | (3 << 11));
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x28000), ==, 0xffffffff);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0xc8), ==, 0);
    qtest_writeq(qts, HEAD(0) + 0x08, 0xffff);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x08), ==, 0);
    qtest_writeq(qts, HEAD(0) + 0x600, 0xffff);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x600), ==, 0);
    qtest_quit(qts);
}

static void test_slot_bounds(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init("-M none -cpu R10000 -S "
            "-device sgi-pic,id=pic,test-fixture=on "
            "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=5");

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*slot 5 function 0 not available*reserved*");
}

static void test_unresolved_default_identity(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init("-M none -cpu R10000 -S "
            "-device sgi-pic,id=pic");

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*PIC revision 3 PCI device 0 identity is"
                              " unresolved*test-fixture=on is only for"
                              " synthetic register tests*");
}

static void test_unconnected_linked_transport(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init("-M none -cpu R10000 -S "
            "-device sgi-pic,id=pic,test-fixture=on,linked-transport=on");

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*PIC linked transport requires a typed peer "
                              "before realization*");
}

static void assert_pic_profile_realize_error(const char *profile_options,
                                              const char *message)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
            "-device sgi-pic,id=pic,%s", profile_options);

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(message);
}

static void test_bedrock_profile_is_fixture_only(void)
{
    assert_pic_profile_realize_error(
        "compatibility-profile=pic-rev3-bedrock",
        "*pic-rev3-bedrock is only for qtest fixtures*");
}

static void test_runtime_profile_rejects_fixture_mode(void)
{
    assert_pic_profile_realize_error(
        "compatibility-profile=pic-rev3-bedrock-experimental,"
        "test-fixture=on",
        "*PIC runtime profile cannot enable test-fixture*");
}

static void test_runtime_profile_non_qtest_cli_refuses_static_sysbus(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "none", "-display", "none",
        "-device",
        "sgi-xio-test-target,id=fabric,pic-runtime-profile=on", NULL,
    };
    g_autofree char *command = g_strjoinv(" ", (char **)args);
    g_autofree char *stderr_text = NULL;
    int status;
    GError *error = NULL;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                               NULL, &stderr_text, &status, &error));
    g_assert_no_error(error);
    if (!stderr_text || !strstr(stderr_text,
            "Parameter 'driver' expects a dynamic sysbus device type "
            "for the machine")) {
        g_test_message("non-qtest CLI did not report the static SysBus "
                       "device refusal; "
                       "wait status 0x%x; command: %s; stderr: %s",
                       status, command,
                       stderr_text ? stderr_text : "(empty)");
    }
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), ==, 1);
    g_assert_nonnull(strstr(stderr_text,
        "Parameter 'driver' expects a dynamic sysbus device type "
        "for the machine"));
}

static void test_runtime_profile_derives_linked_transport(void)
{
    assert_pic_profile_realize_error(
        "compatibility-profile=pic-rev3-bedrock-experimental",
        "*PIC linked transport requires a typed peer before realization*");
}

static void test_runtime_profile_is_immutable_after_realize(void)
{
    QTestState *qts = pic_runtime_start(false, "");
    QDict *error = qtest_qmp_assert_failure_ref(qts,
        "{'execute':'qom-set','arguments':{'path':"
        "'/machine/peripheral/fabric/pic',"
        "'property':'compatibility-profile',"
        "'value':'pic-rev3-bedrock'}}");

    g_assert_cmpstr(qdict_get_str(error, "desc"), ==,
        "Attempt to set property 'compatibility-profile' on anonymous device "
        "(type 'sgi-pic') after it was realized");
    qobject_unref(error);
    qtest_quit(qts);
}

static void pic_runtime_copy_rejects_source(QTestState *qts, uint64_t source,
                                            const uint8_t *sentinel,
                                            size_t size)
{
    uint8_t after[8];

    g_assert_cmpuint(size, <=, sizeof(after));
    qtest_memwrite(qts, TARGET + 0x3000, sentinel, size);
    qtest_writel(qts, CARD(0), 0x80003000);
    qtest_writel(qts, CARD(0) + 4, (uint32_t)size);
    qtest_writel(qts, CARD(0) + 0x14, (uint32_t)source);
    qtest_writel(qts, CARD(0) + 0x1c, (uint32_t)(source >> 32));
    qtest_writel(qts, CARD(0) + 0xc, 2);
    qtest_memread(qts, TARGET + 0x3000, after, size);
    g_assert_cmpmem(after, size, sentinel, size);
}

static void test_runtime_profile_reserved_device_and_dma(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-runtime-copy-XXXXXX",
                                          &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log_arg = NULL;
    g_autofree char *log_contents = NULL;
    QTestState *qts;
    uint8_t bytes[8];
    uint8_t copied[8];
    uint8_t sentinel[8] = { 0xa5, 0xa5, 0xa5, 0xa5,
                            0xa5, 0xa5, 0xa5, 0xa5 };
    const uint64_t unsupported_sources[] = {
        UINT64_C(0x100000000), UINT64_C(0x180000000),
        UINT64_C(0x40000000),
    };

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    log_arg = g_strdup_printf("-D %s", log_path);
    qts = pic_runtime_start(true, log_arg);

    for (unsigned head = 0; head < 2; head++) {
        uint32_t address = 0x80001000 + head * 0x1000;
        uint8_t pattern = 0x31 + head * 0x10;

        g_assert_cmphex(qtest_readl(qts, HEAD(head) + 0x20000), ==,
                        0xffffffff);
        g_assert_cmphex(qtest_readl(qts, HEAD(head) + 0x21000), ==,
                        0x3500feed + head * 0x10000);
        qtest_writel(qts, HEAD(head) + 0x21010, 0x01000000);
        qtest_writel(qts, HEAD(head) + 0x21004,
                     PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
        qtest_writeq(qts, HEAD(head) + 0x20, 4 + head);
        qtest_writeq(qts, HEAD(head) + 0x80, 0);
        qtest_writeq(qts, HEAD(head) + 0x200,
                     head ? 1ULL << 19 : 0);
        qtest_writel(qts, CARD(head), address);
        qtest_writel(qts, CARD(head) + 4, sizeof(bytes));
        qtest_writel(qts, CARD(head) + 8, pattern);
        qtest_writel(qts, CARD(head) + 0xc, 1);
        qtest_memread(qts, TARGET + 0x1000 + head * 0x1000,
                      bytes, sizeof(bytes));
        for (unsigned i = 0; i < sizeof(bytes); i++) {
            g_assert_cmpuint(bytes[i], ==,
                             pattern + (head ? (i ^ 3) : i));
        }
    }

    /* A supported source read must produce the expected destination copy. */
    qtest_writel(qts, CARD(0), 0x80002000);
    qtest_writel(qts, CARD(0) + 4, sizeof(bytes));
    qtest_writel(qts, CARD(0) + 0x14, 0x80001000);
    qtest_writel(qts, CARD(0) + 0x1c, 0);
    qtest_writel(qts, CARD(0) + 0xc, 2);
    qtest_memread(qts, TARGET + 0x2000, copied, sizeof(copied));
    for (unsigned i = 0; i < sizeof(copied); i++) {
        g_assert_cmpuint(copied[i], ==, 0x31 + i);
    }

    for (unsigned i = 0; i < G_N_ELEMENTS(unsupported_sources); i++) {
        pic_runtime_copy_rejects_source(qts, unsupported_sources[i], sentinel,
                                        sizeof(sentinel));
    }

    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    for (unsigned head = 0; head < 2; head++) {
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x80), ==, 0);
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x200), ==,
                        0x18001000);
    }
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log_contents, NULL, &error));
    g_assert_no_error(error);
    for (unsigned i = 0; i < G_N_ELEMENTS(unsupported_sources); i++) {
        g_autofree char *copy_failure = g_strdup_printf(
            "sgi-bridge-dma-test: the copy's read of 0x%" PRIx64
            " failed, so it writes nothing", unsupported_sources[i]);
        g_autofree char *pic_refusal = unsupported_sources[i] > UINT32_MAX ?
            g_strdup_printf("sgi-pic: head 0 direct64 target-zero address "
                            "0x%016" PRIx64 " from slot",
                            unsupported_sources[i]) :
            g_strdup_printf("sgi-pic: head 0 unsupported DMA 0x%" PRIx64,
                            unsupported_sources[i]);

        g_assert_nonnull(strstr(log_contents, copy_failure));
        g_assert_nonnull(strstr(log_contents, pic_refusal));
    }
    unlink(log_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_runtime_dir_map_unsupported_fields(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-dir-map-XXXXXX", &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log_contents = NULL;
    QTestState *qts;
    const uint64_t supported = UINT64_C(0x1ffff) | (UINT64_C(0xf) << 20);

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    qts = qtest_initf("-M none -cpu R10000 -S -d unimp,guest_errors -D %s "
        "-device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,"
        "pic-runtime-profile=on,pic-linked=off,pic-bedrock-interrupts=off",
        log_path);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    for (unsigned head = 0; head < 2; head++) {
        uint64_t value = (UINT64_C(1) << 63) | (UINT64_C(1) << 17) |
                         (UINT64_C(3) << 20) | 0x1234;

        qtest_writeq(qts, HEAD(head) + 0x80, value);
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x80), ==,
                        value & supported);
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log_contents,
        "head 0 origin=widget-mmio write offset 0x80 size 8 value "
        "0x8000000000321234 unsupported"));
    g_assert_nonnull(strstr(log_contents,
        "head 1 origin=widget-mmio write offset 0x80 size 8 value "
        "0x8000000000321234 unsupported"));
    g_unlink(log_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_type1_configuration(void)
{
    QTestState *qts = qtest_init("-M none -cpu R10000 -S "
        "-device sgi-pic,id=pic,test-fixture=on "
        "-device pci-bridge,id=down,bus=pic-head-0.0,addr=4,chassis_nr=1 "
        "-device sgi-bridge-dma-test,bus=down,addr=3,"
        "vendor-id=0xfeed,device-id=0x3504 "
        "-device sgi-bridge-dma-test,bus=down,addr=4,"
        "vendor-id=0xfeed,device-id=0x3505");

    qtest_sysbus_mmio_map(qts, PIC, 0, BASE);
    qtest_writel(qts, HEAD(0) + 0x24018, 0x00020200);
    qtest_writeq(qts, HEAD(0) + 0xc8, (2ULL << 16) | (3 << 11));
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x28000), ==, 0x3504feed);
    qtest_writeq(qts, HEAD(0) + 0xc8, (2ULL << 16) | (4 << 11));
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x28000), ==, 0x3505feed);
    qtest_writeq(qts, HEAD(0) + 0xc8, (2ULL << 16) | (5 << 11));
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x28000), ==, 0xffffffff);
    qtest_writeq(qts, HEAD(1) + 0xc8, (2ULL << 16) | (3 << 11));
    g_assert_cmphex(qtest_readl(qts, HEAD(1) + 0x28000), ==, 0xffffffff);
    qtest_quit(qts);
}

static void test_unsupported_registers(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-reg-XXXXXX", &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log_contents = NULL;
    QTestState *qts;
    const char *at;
    unsigned count = 0;
    unsigned i;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors -D %s "
        "-device sgi-pic,id=pic,test-fixture=on", log_path);
    qtest_sysbus_mmio_map(qts, PIC, 0, BASE);
    qtest_writeq(qts, HEAD(0) + 0x08, 0x55);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x08), ==, 0);
    qtest_writeq(qts, HEAD(0) + 0x600, 0x66);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x600), ==, 0);
    qtest_writeb(qts, HEAD(0) + 0x108, 0x77);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x108), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x40000000), ==, 0);
    for (i = 0; i < 40; i++) {
        qtest_writeq(qts, HEAD(0) + 0x600, 0x100 + i);
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log_contents,
                            "head 0 origin=widget-mmio write offset 0x8"
                            " size 8 value 0x55"));
    g_assert_nonnull(strstr(log_contents,
                            "head 0 origin=widget-mmio read offset 0x8"
                            " size 8"));
    g_assert_nonnull(strstr(log_contents,
                            "head 0 origin=widget-mmio write offset 0x600"
                            " size 8 value 0x66"));
    g_assert_nonnull(strstr(log_contents,
                            "head 0 origin=widget-mmio write offset 0x108"
                            " size 1 value 0x77"));
    g_assert_nonnull(strstr(log_contents,
                            "head 0 origin=pci-memory-window read offset"
                            " 0x40000000 size 8"));
    for (at = log_contents; (at = strstr(at, "further unsupported accesses"
                                      " suppressed")); at++) {
        count++;
    }
    g_assert_cmpuint(count, ==, 1);
    g_assert_null(strstr(log_contents, "size 8 value 0x127"));
    unlink(log_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_pin_and_reset(void)
{
    QTestState *qts = pic_start(false, "");

    /* Packet data is a declared model rule, not a measured PIC format. */
    qtest_writeq(qts, HEAD(0) + 0x130, 0x1234000000001000ULL);
    qtest_writeq(qts, HEAD(1) + 0x168, 0x5678000000002000ULL);
    qtest_writeq(qts, HEAD(0) + 0x80, 0x1234);
    qtest_writeq(qts, HEAD(0) + 0x20, 0x123456789abcdef3ULL);
    qtest_writeq(qts, HEAD(1) + 0x20, 7);
    qtest_writeq(qts, HEAD(0) + 0xc8, (2ULL << 16) | (3 << 11));
    qtest_writeq(qts, HEAD(0) + 0x18000, 0xfeedface);
    qtest_writeq(qts, HEAD(1) + 0x19000, 0xcafebabe);
    qtest_writeq(qts, HEAD(1) + 0x80, 0x1234);
    qtest_writeq(qts, HEAD(1) + 0xc8, (3ULL << 16) | (4 << 11));
    qtest_set_irq_in(qts, PIC "/head-0", NULL, 0, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 1);
    g_assert_cmpuint(pic_requests(qts, 0, 0, true), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x100), ==, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0);
    qtest_writeq(qts, HEAD(0) + 0x108, 1);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0x11234);
    g_assert_cmpuint(pic_requests(qts, 0, 0, true), ==, 1);
    qtest_set_irq_in(qts, PIC "/head-1", NULL, 7, 1);
    qtest_writeq(qts, HEAD(1) + 0x108, 0x80);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x2000), ==, 0x15678);
    g_assert_cmpuint(pic_requests(qts, 1, 7, true), ==, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x100), ==, 0x80);
    qtest_writeq(qts, HEAD(0) + 0x110, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 1);
    g_assert_cmpuint(pic_requests(qts, 0, 0, true), ==, 2);
    qtest_writeq(qts, TARGET + 0x1000, 0x55);
    qtest_writeq(qts, HEAD(0) + 0x130, 0x1234000100001000ULL);
    qtest_writeq(qts, HEAD(0) + 0x110, 1);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0x55);
    g_assert_cmpuint(pic_requests(qts, 0, 0, true), ==, 3);
    qtest_writeq(qts, HEAD(0) + 0x130, 0x1234000000001000ULL);
    qtest_writeq(qts, HEAD(0) + 0x118, 1);
    qtest_set_irq_in(qts, PIC "/head-0", NULL, 0, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0x1234);
    g_assert_cmpuint(pic_requests(qts, 0, 0, false), ==, 1);
    qtest_writeq(qts, HEAD(1) + 0x118, 0x80);
    qtest_set_irq_in(qts, PIC "/head-1", NULL, 7, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x2000), ==, 0x5678);
    g_assert_cmpuint(pic_requests(qts, 1, 7, false), ==, 1);
    qtest_writeq(qts, HEAD(0) + 0x110, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 0);
    qtest_set_irq_in(qts, PIC "/head-0", NULL, 0, 1);
    qtest_set_irq_in(qts, PIC "/head-1", NULL, 7, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x100), ==, 0x80);
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x108), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x130), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x80), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x20), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0xc8), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x18000), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x108), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x20), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x100), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x80), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0xc8), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x19000), ==, 0);
    qtest_quit(qts);
}

static void test_remaining_pin_edges(void)
{
    static const struct {
        unsigned head;
        unsigned pin;
        uint64_t target_offset;
        uint16_t level;
    } cases[] = {
        { 0, 7, 0x3000, 0x3456 },
        { 1, 0, 0x4000, 0x789a },
    };
    unsigned i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        unsigned head = cases[i].head;
        unsigned other = 1 - head;
        unsigned pin = cases[i].pin;
        uint64_t bit = 1U << pin;
        uint64_t target = TARGET + cases[i].target_offset;
        g_autofree char *path = g_strdup_printf(PIC "/head-%u", head);
        QTestState *qts = pic_start(false, "");

        qtest_writeq(qts, HEAD(head) + 0x130 + pin * 8,
                     ((uint64_t)cases[i].level << 48) |
                     cases[i].target_offset);
        qtest_set_irq_in(qts, path, NULL, pin, 1);
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x100), ==, bit);
        g_assert_cmphex(qtest_readq(qts, HEAD(other) + 0x100), ==, 0);
        g_assert_cmpuint(pic_requests(qts, head, pin, true), ==, 0);
        g_assert_cmpuint(pic_requests(qts, other, pin, true), ==, 0);
        qtest_writeq(qts, HEAD(head) + 0x108, bit);
        g_assert_cmphex(qtest_readq(qts, target), ==,
                        0x10000 | cases[i].level);
        g_assert_cmpuint(pic_requests(qts, head, pin, true), ==, 1);
        g_assert_cmpuint(pic_requests(qts, other, pin, true), ==, 0);
        qtest_set_irq_in(qts, path, NULL, pin, 1);
        g_assert_cmpuint(pic_requests(qts, head, pin, true), ==, 1);
        qtest_writeq(qts, HEAD(head) + 0x110, bit);
        g_assert_cmpuint(pic_requests(qts, head, pin, true), ==, 2);
        g_assert_cmphex(qtest_readq(qts, target), ==,
                        0x10000 | cases[i].level);
        qtest_writeq(qts, HEAD(head) + 0x118, bit);
        qtest_set_irq_in(qts, path, NULL, pin, 0);
        g_assert_cmphex(qtest_readq(qts, target), ==, cases[i].level);
        g_assert_cmpuint(pic_requests(qts, head, pin, false), ==, 1);
        g_assert_cmpuint(pic_requests(qts, other, pin, false), ==, 0);
        g_assert_cmphex(qtest_readq(qts, HEAD(other) + 0x108), ==, 0);
        qtest_quit(qts);
    }
}

static void test_direct_dma(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-direct-XXXXXX", &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log_contents = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    uint8_t bytes[8];
    uint8_t unchanged[8];
    unsigned i;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    extra = g_strdup_printf("-D %s", log_path);
    qts = pic_start(true, extra);
    for (i = 0; i < 2; i++) {
        qtest_writel(qts, HEAD(i) + 0x21010, 0x01000000);
        qtest_writel(qts, HEAD(i) + 0x21004, 6);
        qtest_writeq(qts, HEAD(i) + 0x80, 0);
        qtest_writel(qts, CARD(i), 0x80001000 + i * 0x1000);
        qtest_writel(qts, CARD(i) + 4, 8);
        qtest_writel(qts, CARD(i) + 8, i ? 0x10 : 0x01);
        qtest_writel(qts, CARD(i) + 0xc, 1);
    }
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 1 + i);
    }
    qtest_memread(qts, TARGET + 0x2000, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x10 + i);
    }
    qtest_writeq(qts, HEAD(0) + 0x200, 1U << 19);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x200), ==, 1U << 19);
    qtest_writel(qts, CARD(0), 0x80001000);
    qtest_writel(qts, CARD(0) + 4, 8);
    qtest_writel(qts, CARD(0) + 8, 0x21);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x21 + (i ^ 3));
    }
    qtest_writel(qts, CARD(0), 0x80000ffc);
    qtest_writel(qts, CARD(0) + 8, 0x31);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0xffc, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x31 + (i ^ 3));
    }
    qtest_writeq(qts, HEAD(0) + 0x200, (1U << 19) | (1U << 20));
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x200), ==,
                    (1U << 19) | (1U << 20));
    qtest_memread(qts, TARGET + 0x1000, unchanged, sizeof(unchanged));
    qtest_writeq(qts, HEAD(0) + 0x80, 0xfULL << 20);
    qtest_writel(qts, CARD(0), 0x80001000);
    qtest_writel(qts, CARD(0) + 8, 0x41);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), unchanged, sizeof(unchanged));
    qtest_writeq(qts, HEAD(0) + 0x80, 0);
    qtest_writeq(qts, HEAD(0) + 0x200, 0);
    qtest_writel(qts, CARD(0), 0x8000fffc);
    qtest_writel(qts, CARD(0) + 8, 0x60);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0xfffc, bytes, 4);
    for (i = 0; i < 4; i++) {
        g_assert_cmpuint(bytes[i], ==, 0x60 + i);
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log_contents,
                            "head 0 invalid DMA target f address 0x1000"
                            " from slot 1"));
    unlink(log_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_linked_dma(void)
{
    QTestState *qts = pic_linked_start(true, "", "");
    uint8_t bytes[8];
    uint64_t calls_before;
    unsigned i;

    g_assert_cmphex(target_metric(qts, "packet-window-base"), ==, 0);

    for (i = 0; i < 2; i++) {
        qtest_writel(qts, HEAD(i) + 0x21010, 0x01000000);
        qtest_writel(qts, HEAD(i) + 0x21004, 6);
        qtest_writeq(qts, HEAD(i) + 0x80, 1ULL << 20);
        qtest_writeq(qts, HEAD(i) + 0x20,
                     (i ? 0xfedcba9876543217ULL : 0x123456789abcdef3ULL));
    }
    qtest_writeq(qts, HEAD(0) + 0x200, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x200), ==, 0);
    qtest_writel(qts, CARD(0), 0x80001000);
    qtest_writel(qts, CARD(0) + 4, 8);
    qtest_writel(qts, CARD(0) + 8, 0x41);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x41 + i);
    }
    g_assert_cmpuint(target_metric(qts, "x-test-packet-address"), ==,
                     0x1000);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 3);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-device-known"), ==,
                     0);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_OK);

    calls_before = target_metric(qts, "x-test-packet-calls");
    qtest_writeq(qts, HEAD(1) + 0x200, 1ULL << 19);
    qtest_writel(qts, CARD(1), 0x80002000);
    qtest_writel(qts, CARD(1) + 4, 8);
    qtest_writel(qts, CARD(1) + 8, 0x51);
    qtest_writel(qts, CARD(1) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x2000, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x51 + (i ^ 3));
    }
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls") - calls_before,
                     ==, 8);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-address"), ==,
                     0x2004);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 7);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-device-known"), ==,
                     0);
    memset(bytes, 0xa5, sizeof(bytes));
    qtest_memwrite(qts, TARGET + 0x3000, bytes, sizeof(bytes));
    qtest_writeq(qts, HEAD(0) + 0x80, (1ULL << 20) | 0x10000);
    qtest_writel(qts, CARD(0), 0x80003000);
    qtest_writel(qts, CARD(0) + 8, 0x71);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-address"), ==,
                     0x800000003000ULL);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_DECODE_ERROR);
    qtest_memread(qts, TARGET + 0x3000, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0xa5);
    }
    qtest_writeq(qts, HEAD(0) + 0x20, 0xabcdef0123456785ULL);
    qtest_writeq(qts, HEAD(0) + 0x80, 1ULL << 20);
    qtest_writel(qts, CARD(0), 0x80004000);
    qtest_writel(qts, CARD(0) + 8, 0x81);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x4000, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x81 + i);
    }
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x20), ==, 5);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 5);
    qtest_quit(qts);
}

static void test_linked_partial_callback(void)
{
    QTestState *qts = pic_linked_start(true,
        ",packet-failure=on,packet-partial=4", "");
    uint8_t sentinel[8] = { 0xa5, 0xa5, 0xa5, 0xa5,
                            0xa5, 0xa5, 0xa5, 0xa5 };
    uint8_t bytes[8];
    unsigned i;

    qtest_memwrite(qts, TARGET + 0x1000, sentinel, sizeof(sentinel));
    qtest_writel(qts, HEAD(0) + 0x21010, 0x01000000);
    qtest_writel(qts, HEAD(0) + 0x21004, 6);
    qtest_writeq(qts, HEAD(0) + 0x80, 1ULL << 20);
    qtest_writeq(qts, HEAD(0) + 0x20, 3);
    qtest_writel(qts, CARD(0), 0x80001000);
    qtest_writel(qts, CARD(0) + 4, 8);
    qtest_writel(qts, CARD(0) + 8, 0x61);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    for (i = 0; i < 4; i++) {
        g_assert_cmpuint(bytes[i], ==, 0x61 + i);
    }
    g_assert_cmpmem(bytes + 4, 4, sentinel + 4, 4);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_ACCESS_ERROR);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 1);
    g_assert_cmpuint(pic_metric(qts, "x-test-linked-last-result"), ==,
                     MEMTX_ACCESS_ERROR);
    qtest_quit(qts);
}

static void test_linked_route_rejection(void)
{
    QTestState *qts = pic_linked_start(true, "", "");
    uint8_t sentinel[8] = { 0xa5, 0xa5, 0xa5, 0xa5,
                            0xa5, 0xa5, 0xa5, 0xa5 };
    uint8_t bytes[8];

    qtest_memwrite(qts, TARGET + 0x1000, sentinel, sizeof(sentinel));
    qtest_writel(qts, HEAD(0) + 0x21010, 0x01000000);
    qtest_writel(qts, HEAD(0) + 0x21004, 6);
    qtest_writeq(qts, HEAD(0) + 0x80, 1ULL << 20);
    qtest_writel(qts, CARD(0), 0x40000000);
    qtest_writel(qts, CARD(0) + 4, 8);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), sentinel, sizeof(sentinel));
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 0);
    g_assert_cmpuint(pic_metric(qts, "x-test-linked-packet-count"), ==, 0);
    qtest_quit(qts);
}

static void test_linked_nested_dma(void)
{
    static const uint8_t expected_nested[4] = { 0xd1, 0xd2, 0xd3, 0xd4 };
    QTestState *qts = pic_linked_start(true, ",nested-dma=on", "");
    uint8_t outer[8];
    uint8_t nested[4];
    unsigned i;

    qtest_writel(qts, HEAD(0) + 0x21010, 0x01000000);
    qtest_writel(qts, HEAD(0) + 0x21004, 6);
    qtest_writeq(qts, HEAD(0) + 0x80, 1ULL << 20);
    qtest_writeq(qts, HEAD(0) + 0x20, 3);
    qtest_writel(qts, CARD(0), 0x80001000);
    qtest_writel(qts, CARD(0) + 4, 8);
    qtest_writel(qts, CARD(0) + 8, 0x41);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x1000, outer, sizeof(outer));
    qtest_memread(qts, TARGET + 0x3000, nested, sizeof(nested));
    for (i = 0; i < sizeof(outer); i++) {
        g_assert_cmpuint(outer[i], ==, 0x41 + i);
    }
    g_assert_cmpmem(nested, sizeof(nested), expected_nested,
                    sizeof(expected_nested));
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 2);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-address"), ==,
                     0x3000);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 3);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-device-known"), ==,
                     0);
    qtest_quit(qts);
}

static void test_linked_interrupts(void)
{
    QTestState *qts = pic_linked_start(false,
        ",packet-window-base=0x100000000000", "");
    uint64_t destination = (0x1234ULL << 48) | (1ULL << 44) | 0x1000;
    uint64_t other_destination = (0x5678ULL << 48) | (1ULL << 44) |
                                 0x2000;

    qtest_writeq(qts, HEAD(0) + 0x20, 3);
    qtest_writeq(qts, HEAD(0) + 0x130, destination);
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 1);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 0);
    qtest_writeq(qts, HEAD(0) + 0x108, 1);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0x11234);
    g_assert_cmpuint(pic_requests_at(qts, PIC_CHILD, 0, 0, true), ==, 1);
    g_assert_cmpuint(pic_requests_at(qts, PIC_CHILD, 1, 0, true), ==, 0);
    g_assert_cmphex(target_metric(qts, "x-test-packet-address"), ==,
                    (1ULL << 44) | 0x1000);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_OK);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 3);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-device-known"), ==,
                     0);
    qtest_writeq(qts, HEAD(0) + 0x110, 1);
    g_assert_cmpuint(pic_requests_at(qts, PIC_CHILD, 0, 0, true), ==, 2);
    qtest_writeq(qts, HEAD(0) + 0x118, 1);
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0x1234);
    g_assert_cmpuint(pic_requests_at(qts, PIC_CHILD, 0, 0, false), ==, 1);

    qtest_writeq(qts, HEAD(1) + 0x20, 7);
    qtest_writeq(qts, HEAD(1) + 0x168, other_destination);
    qtest_set_irq_in(qts, PIC_CHILD "/head-1", NULL, 7, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x100), ==, 0x80);
    g_assert_cmpuint(pic_requests_at(qts, PIC_CHILD, 1, 7, true), ==, 0);
    qtest_writeq(qts, HEAD(1) + 0x108, 0x80);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x2000), ==, 0x15678);
    g_assert_cmpuint(pic_requests_at(qts, PIC_CHILD, 1, 7, true), ==, 1);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 7);
    qtest_writeq(qts, HEAD(1) + 0x118, 0x80);
    qtest_set_irq_in(qts, PIC_CHILD "/head-1", NULL, 7, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x2000), ==, 0x5678);
    g_assert_cmpuint(pic_requests_at(qts, PIC_CHILD, 1, 7, false), ==, 1);

    qtest_writeq(qts, HEAD(0) + 0x130,
                 (0x2345ULL << 48) | (1ULL << 44) | (1ULL << 32));
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 1);
    qtest_writeq(qts, HEAD(0) + 0x108, 1);
    g_assert_cmphex(target_metric(qts, "x-test-packet-address"), ==,
                    (1ULL << 44) | (1ULL << 32));
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 3);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 6);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_DECODE_ERROR);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0x1234);
    qtest_quit(qts);
}

static void test_linked_interrupt_window_bounds(void)
{
    static const uint8_t expected[8] = { 0, 0, 0, 0, 0, 1, 0x12, 0x34 };
    QTestState *qts = pic_linked_start(false,
        ",packet-window-base=0x100000000000", "");
    uint8_t bytes[8];
    uint64_t base = UINT64_C(1) << 44;

    memset(bytes, 0xa5, sizeof(bytes));
    qtest_writeq(qts, HEAD(0) + 0x20, 3);
    qtest_writeq(qts, HEAD(0) + 0x130,
                 (UINT64_C(0x1234) << 48) | 0x1000);
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 1);
    qtest_writeq(qts, HEAD(0) + 0x108, 1);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0x11234);
    g_assert_cmphex(target_metric(qts, "x-test-packet-address"), ==, 0x1000);

    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 0);
    qtest_writeq(qts, HEAD(0) + 0x110, 1);
    qtest_writeq(qts, HEAD(0) + 0x130, (UINT64_C(0x1234) << 48) |
                 base | 0xfffff8);
    qtest_memwrite(qts, TARGET + 0xfffff8, bytes, sizeof(bytes));
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 1);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_OK);

    qtest_writeq(qts, HEAD(0) + 0x110, 1);
    qtest_writeq(qts, HEAD(0) + 0x130, (UINT64_C(0x2345) << 48) |
                 base | 0xfffff9);
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 0);
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 1);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_DECODE_ERROR);
    qtest_memread(qts, TARGET + 0xfffff8, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), expected, sizeof(expected));
    qtest_quit(qts);
}

static void assert_packet_window_realize_error(const char *command)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init(command);

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*invalid XIO test packet window base*");
}

static void test_packet_window_rejects_small_base(void)
{
    assert_packet_window_realize_error("-M none -cpu R10000 -S "
        "-device sgi-xio-test-target,id=bad,size=4096,"
        "packet-window-base=4095");
}

static void test_packet_window_rejects_out_of_packet_base(void)
{
    assert_packet_window_realize_error("-M none -cpu R10000 -S "
        "-device sgi-xio-test-target,id=bad,size=4096,"
        "packet-window-base=281474976710656");
}

static void test_packet_window_rejects_overflow(void)
{
    assert_packet_window_realize_error("-M none -cpu R10000 -S "
        "-device sgi-xio-test-target,id=bad,size=4096,"
        "packet-window-base=281474976706561");
}

static void test_packet_window_rejects_target_port(void)
{
    assert_packet_window_realize_error("-M none -cpu R10000 -m 256M -S "
        "-device sgi-xio-test-target,id=memory,size=16777216 "
        "-device sgi-bedrock,id=bedrock,processor-count=1,"
        "dma-memory=/machine/peripheral/memory/memory.ram[0] "
        "-device sgi-xio-test-target,id=bad,size=4096,"
        "packet-window-base=1048576,"
        "target-port=/machine/peripheral/bedrock/io");
}

static void test_packet_window_exact_limit_fit(void)
{
    QTestState *qts = qtest_init("-M none -cpu R10000 -S "
        "-device sgi-xio-test-target,id=fit,size=4096,"
        "packet-window-base=281474976706560");

    qtest_quit(qts);
}

static void test_bedrock_interrupt_profile(bool runtime_profile)
{
    unsigned map_io_registers;

    for (map_io_registers = 0; map_io_registers < 2; map_io_registers++) {
        QTestState *qts = pic_bedrock_parent_start(map_io_registers,
                                                   runtime_profile);

        for (unsigned head = 0; head < 2; head++) {
            unsigned level = 0x34 + head;
            uint64_t pending_bit = UINT64_C(1) << level;
            uint64_t address = II_REGISTER | II_NODE(0) | II_HUB_REMOTE | 0x90;
            uint64_t destination = ((uint64_t)level << 48) | address;
            uint64_t calls = target_metric(qts, "x-test-packet-calls");
            g_autofree char *head_path = g_strdup_printf(
                PIC_CHILD "/head-%u", head);

            qtest_writeq(qts, HEAD(head) + 0x20, 14 + head);
            qtest_writeq(qts, HEAD(head) + 0x130, destination);
            qtest_writeq(qts, HEAD(head) + 0x118, 1);
            qtest_set_irq_in(qts, head_path, NULL, 0, 1);
            g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                             calls);
            qtest_writeq(qts, HEAD(head) + 0x108, 1);
            g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                             calls + 1);
            g_assert_cmphex(target_metric(qts, "x-test-packet-address"), ==,
                            address);
            g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==,
                             14 + head);
            g_assert_cmphex(target_metric(qts, "x-test-packet-result"), ==,
                            MEMTX_OK);
            g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) &
                            pending_bit, ==, pending_bit);

            qtest_set_irq_in(qts, head_path, NULL, 0, 0);
            g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                             calls + 2);
            qtest_writeq(qts, HEAD(head) + 0x110, 1);
            g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x100) & 1, ==, 0);
            g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) &
                            pending_bit, ==, 0);

            qtest_set_irq_in(qts, head_path, NULL, 0, 1);
            g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) &
                            pending_bit, ==, pending_bit);
            qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
            g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x100), ==, 0);
            g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) &
                            pending_bit, ==, 0);
        }

        for (unsigned level_index = 0; level_index < 2; level_index++) {
            unsigned level = level_index ? 128 : 6;
            uint64_t address = II_REGISTER | II_NODE(0) | II_HUB_REMOTE | 0xa0;
            uint64_t destination = ((uint64_t)level << 48) | address;
            uint64_t calls = target_metric(qts, "x-test-packet-calls");

            qtest_writeq(qts, HEAD(0) + 0x138, destination);
            qtest_writeq(qts, HEAD(0) + 0x118, 2);
            qtest_writeq(qts, HEAD(0) + 0x108, 2);
            qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 1, 1);
            g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                             calls);
            qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 1, 0);
            g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                             calls);
        }
        qtest_quit(qts);
    }
}

static void test_linked_bedrock_interrupt(void)
{
    test_bedrock_interrupt_profile(false);
}

static void test_runtime_profile_bedrock_interrupt(void)
{
    test_bedrock_interrupt_profile(true);
}

static void test_runtime_profile_bedrock_level_mirror(void)
{
    const uint64_t pending = UINT64_C(1) << 11;
    const uint64_t address = II_REGISTER | II_NODE(0) | II_HUB_REMOTE | 0x90;
    const uint64_t destination = (UINT64_C(11) << 48) | address;
    const uint64_t invalid_destination = (UINT64_C(11) << 48) |
        (II_REGISTER | II_NODE(2) | II_HUB_REMOTE | 0x90);
    QTestState *qts = pic_bedrock_parent_start(false, true);

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    qtest_irq_intercept_out_named(qts, "/machine/peripheral/bedrock",
                                  "hardware-irq");
    qtest_writeq(qts, 0x10000000 + 0xa8, pending);
    qtest_writeq(qts, HEAD(0) + 0x20, 14);
    qtest_writeq(qts, HEAD(0) + 0x148, destination);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x118), ==, 0);
    qtest_writeq(qts, HEAD(0) + 0x108, 1U << 3);
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 3, 1);
    g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & pending, ==,
                    pending);
    g_assert_true(qtest_get_irq(qts, 0));

    for (unsigned service = 0; service < 3; service++) {
        qtest_writeq(qts, 0x10000000 + 0x90, 11);
        g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & pending, ==,
                        0);
        g_assert_false(qtest_get_irq(qts, 0));
        qtest_clock_step(qts, 500000);
        g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & pending, ==,
                        0);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & pending, ==,
                        pending);
        g_assert_true(qtest_get_irq(qts, 0));
    }

    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 3, 0);
    qtest_writeq(qts, 0x10000000 + 0x90, 11);
    qtest_clock_step(qts, 2000000);
    g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & pending, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 3, 1);
    g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & pending, ==,
                    pending);
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 3, 0);
    qtest_writeq(qts, 0x10000000 + 0x90, 11);
    qtest_writeq(qts, HEAD(0) + 0x148, invalid_destination);
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 3, 1);
    qtest_clock_step(qts, 2000000);
    g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & pending, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 3, 0);
    qtest_quit(qts);
}

static void test_runtime_profile_bedrock_rearm_one_level(void)
{
    const uint64_t a = UINT64_C(1) << 11;
    const uint64_t b = UINT64_C(1) << 12;
    const uint64_t address = II_REGISTER | II_NODE(0) | II_HUB_REMOTE | 0x90;
    QTestState *qts = pic_bedrock_parent_start(true, true);

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    qtest_writeq(qts, 0x10000000 + 0xa8, a | b);
    qtest_writeq(qts, HEAD(0) + 0x20, 14);
    qtest_writeq(qts, HEAD(0) + 0x148, (UINT64_C(11) << 48) | address);
    qtest_writeq(qts, HEAD(0) + 0x150, (UINT64_C(12) << 48) | address);
    qtest_writeq(qts, HEAD(0) + 0x108, (1U << 3) | (1U << 4));

    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 3, 1);
    qtest_writeq(qts, 0x10000000 + 0x90, 11);
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 4, 1);
    g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & (a | b), ==, b);
    qtest_writeq(qts, 0x3000000000000ULL + 0x90, 12);
    g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & (a | b), ==, 0);
    qtest_clock_step(qts, 1500000);
    g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & (a | b), ==, a);
    qtest_quit(qts);
}

static void test_runtime_profile_bedrock_rearm_staggered_levels(void)
{
    const uint64_t a = UINT64_C(1) << 11;
    const uint64_t b = UINT64_C(1) << 12;
    const uint64_t address = II_REGISTER | II_NODE(0) | II_HUB_REMOTE | 0x90;
    QTestState *qts = pic_bedrock_parent_start(false, true);

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    qtest_writeq(qts, 0x10000000 + 0xa8, a | b);
    qtest_writeq(qts, HEAD(0) + 0x20, 14);
    qtest_writeq(qts, HEAD(0) + 0x148, (UINT64_C(11) << 48) | address);
    qtest_writeq(qts, HEAD(0) + 0x150, (UINT64_C(12) << 48) | address);
    qtest_writeq(qts, HEAD(0) + 0x108, (1U << 3) | (1U << 4));
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 3, 1);
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 4, 1);

    qtest_writeq(qts, 0x10000000 + 0x90, 11);
    qtest_clock_step(qts, 500000);
    qtest_writeq(qts, 0x10000000 + 0x90, 12);
    qtest_clock_step(qts, 500000);
    g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & (a | b), ==, a);
    qtest_clock_step(qts, 500000);
    g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & (a | b), ==,
                    a | b);
    qtest_quit(qts);
}

static void test_runtime_profile_bedrock_rearm_migration(void)
{
    const uint64_t pending = UINT64_C(1) << 11;
    const uint64_t address = II_REGISTER | II_NODE(0) | II_HUB_REMOTE | 0x90;
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-rearm-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_bedrock_parent_start_full(false, true, false);
    QTestState *to = pic_bedrock_parent_start_full(false, true, true);

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(from, "{'execute':'cont'}");
    qtest_writeq(from, 0x10000000 + 0xa8, pending);
    qtest_writeq(from, HEAD(0) + 0x20, 14);
    qtest_writeq(from, HEAD(0) + 0x148,
                 (UINT64_C(11) << 48) | address);
    qtest_writeq(from, HEAD(0) + 0x108, 1U << 3);
    qtest_set_irq_in(from, PIC_CHILD "/head-0", NULL, 3, 1);
    qtest_writeq(from, 0x10000000 + 0x90, 11);
    qtest_clock_step(from, 500000);
    g_assert_cmphex(qtest_readq(from, 0x10000000 + 0x98) & pending, ==, 0);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, 0x10000000 + 0x98) & pending, ==, 0);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    qtest_clock_step(to, 1000000);
    g_assert_cmphex(qtest_readq(to, 0x10000000 + 0x98) & pending, ==,
                    pending);

    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_linked_bedrock_interrupt_node_route(void)
{
    static const uint8_t set_value[8] = { 0, 0, 0, 0, 0, 0, 1, 0x34 };
    static const uint8_t clear_value[8] = { 0, 0, 0, 0, 0, 0, 0, 0x34 };
    QTestState *qts = pic_bedrock_start(false);
    uint64_t address = II_REGISTER | II_NODE(1) | II_HUB_REMOTE | 0x90;
    uint64_t destination = (UINT64_C(0x34) << 48) | address;
    uint8_t bytes[8];
    uint8_t ingress_sentinel[8];

    memset(ingress_sentinel, 0xa5, sizeof(ingress_sentinel));
    qtest_memwrite(qts, TARGET + 0x90, ingress_sentinel,
                   sizeof(ingress_sentinel));
    qtest_writeq(qts, SERVING_REGS + 0x120, UINT64_C(0x00ff000000000000));
    g_assert_cmphex(qtest_readq(qts, SERVING_REGS + 0x120), ==,
                    UINT64_C(0x00ffffffffffffff));
    qtest_writeq(qts, HEAD(0) + 0x20, 14);
    qtest_writeq(qts, HEAD(0) + 0x130, destination);
    qtest_writeq(qts, HEAD(0) + 0x118, 1);
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 1);
    qtest_writeq(qts, HEAD(0) + 0x108, 1);
    g_assert_cmphex(target_metric(qts, "x-test-packet-address"), ==,
                    address);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 14);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-size"), ==, 8);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_OK);
    qtest_memread(qts, TARGET_NODE1 + 0x90, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), set_value, sizeof(set_value));
    qtest_memread(qts, TARGET + 0x90, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), ingress_sentinel,
                    sizeof(ingress_sentinel));

    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 0);
    qtest_memread(qts, TARGET_NODE1 + 0x90, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), clear_value, sizeof(clear_value));
    qtest_memread(qts, TARGET + 0x90, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), ingress_sentinel,
                    sizeof(ingress_sentinel));

    qtest_writeq(qts, INGRESS_REGS + 0x118, UINT64_MAX ^ (UINT64_C(1) << 14));
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 1);
    qtest_writeq(qts, HEAD(0) + 0x108, 1);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_ACCESS_ERROR);
    qtest_memread(qts, TARGET_NODE1 + 0x90, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), clear_value, sizeof(clear_value));
    qtest_memread(qts, TARGET + 0x90, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), ingress_sentinel,
                    sizeof(ingress_sentinel));
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 0);

    qtest_writeq(qts, INGRESS_REGS + 0x118, UINT64_MAX);
    address = II_REGISTER | II_NODE(2) | II_HUB_REMOTE | 0x90;
    destination = (UINT64_C(0x34) << 48) | address;
    qtest_writeq(qts, HEAD(0) + 0x130, destination);
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 1);
    qtest_writeq(qts, HEAD(0) + 0x108, 1);
    g_assert_cmphex(target_metric(qts, "x-test-packet-address"), ==,
                    address);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_DECODE_ERROR);
    qtest_memread(qts, TARGET_NODE1 + 0x90, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), clear_value, sizeof(clear_value));
    qtest_memread(qts, TARGET + 0x90, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), ingress_sentinel,
                    sizeof(ingress_sentinel));
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 0);
    qtest_quit(qts);
}

static void test_bedrock_interrupt_level_bounds(void)
{
    static const uint16_t levels[] = { 0, 6, 7, 127, 128, UINT16_MAX };
    QTestState *qts = pic_linked_start(false,
        ",pic-bedrock-interrupts=on,packet-window-base=0x800001800000", "");
    uint64_t calls = 0;

    qtest_writeq(qts, HEAD(0) + 0x20, 14);
    qtest_writeq(qts, HEAD(0) + 0x118, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(levels); i++) {
        bool valid = levels[i] >= 7 && levels[i] <= 127;
        uint64_t destination = ((uint64_t)levels[i] << 48) |
                              0x800001800090;

        qtest_writeq(qts, HEAD(0) + 0x130, destination);
        qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 1);
        qtest_writeq(qts, HEAD(0) + 0x108, 1);
        if (valid) {
            calls++;
            g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                             calls);
            g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                             MEMTX_OK);
        } else {
            g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                             calls);
        }
        qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 0);
        if (valid) {
            calls++;
            g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                             calls);
        } else {
            g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==,
                             calls);
        }
    }
    g_assert_cmpuint(calls, ==, 4);
    qtest_quit(qts);
}

static void test_linked_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-linked-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_linked_start(false,
        ",packet-window-base=0x100000000000", "");
    QTestState *to = pic_linked_start(false,
        ",packet-window-base=0x100000000000", "-incoming defer");
    uint64_t destination = (0x1234ULL << 48) | (1ULL << 44) | 0x1000;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_writeq(from, HEAD(0) + 0x20, 3);
    qtest_writeq(from, HEAD(0) + 0x130, destination);
    qtest_writeq(from, HEAD(0) + 0x118, 1);
    qtest_set_irq_in(from, PIC_CHILD "/head-0", NULL, 0, 1);
    g_assert_cmphex(qtest_readq(from, HEAD(0) + 0x100), ==, 1);
    g_assert_cmphex(qtest_readq(from, HEAD(0) + 0x108), ==, 0);
    g_assert_cmpuint(target_metric(from, "x-test-packet-calls"), ==, 0);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x20), ==, 3);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x100), ==, 1);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x108), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x118), ==, 1);
    g_assert_cmpuint(target_metric(to, "x-test-packet-calls"), ==, 0);
    qtest_writeq(to, HEAD(0) + 0x108, 1);
    g_assert_cmpuint(pic_requests_at(to, PIC_CHILD, 0, 0, true), ==, 1);
    g_assert_cmphex(qtest_readq(to, TARGET + 0x1000), ==, 0x11234);
    g_assert_cmpuint(target_metric(to, "x-test-packet-widget"), ==, 3);
    qtest_set_irq_in(to, PIC_CHILD "/head-0", NULL, 0, 0);
    g_assert_cmphex(qtest_readq(to, TARGET + 0x1000), ==, 0x1234);
    g_assert_cmpuint(pic_requests_at(to, PIC_CHILD, 0, 0, false), ==, 1);
    qtest_writeq(to, HEAD(0) + 0x110, 1);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x100), ==, 0);
    qtest_qmp_assert_success(to, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x20), ==, 0);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_linked_migration_mismatch(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-mismatch-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_linked_start(false, "", "");
    QTestState *to = pic_linked_start(false, ",pic-linked=off",
                                      "-incoming defer");
    int64_t deadline;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_probe_child(to)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_bedrock_interrupt_mode_mismatch(void)
{
    GError *error = NULL;
    g_autofree char *dir =
        g_dir_make_tmp("sgi-pic-bedrock-mode-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_linked_start(false,
        ",pic-bedrock-interrupts=on", "");
    QTestState *to = pic_linked_start(false, "", "-incoming defer");
    int64_t deadline;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_probe_child(to)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_runtime_profile_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-runtime-migration-XXXXXX",
                                          &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_profile_migration_start(true, false, false);
    QTestState *to = pic_profile_migration_start(true, true, false);

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_writeq(from, HEAD(0) + 0x80, UINT64_C(5) << 20);
    qtest_writeq(from, HEAD(1) + 0x130,
                 (UINT64_C(0x34) << 48) | UINT64_C(0x800001800090));
    qtest_writel(from, PIC_SHORT_CFG(0, 1) + 0x10, 0x04000000);
    qtest_writel(from, PIC_SHORT_CFG(0, 1) + 4, PCI_COMMAND_MEMORY);
    qtest_writeq(from, CPU_HEAD(0) + 0x210, 0x40 | (1U << 12));
    qtest_writel(from, CPU_HEAD(0) + 0x600024, 0x12345678);
    g_assert_cmphex(qtest_readl(from, BASE + 0x44000024), ==, 0x12345678);
    qtest_writeq(from, HEAD(0) + 0x128,
                 UINT64_C(0x1234567800000055));
    qtest_writel(from, CPU_HEAD(1) + 0x12c, 0x66);
    qtest_writeq(from, HEAD(0) + 0x28, UINT64_C(0x0123456789abcdef));
    qtest_writeq(from, HEAD(1) + 0x28, UINT64_C(0xfedcba9876543210));
    qtest_writeq(from, HEAD(0) + 0x30, UINT64_C(0x1122334455667788));
    qtest_writeq(from, HEAD(1) + 0x30, UINT64_C(0x8877665544332211));
    qtest_writeq(from, HEAD(0) + 0x38, UINT64_C(0x13579bdf2468ace0));
    qtest_writeq(from, HEAD(1) + 0x38, UINT64_C(0x0eca8642fdb97531));
    assert_runtime_pic_identity(from);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    assert_runtime_pic_identity(to);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x80), ==,
                    UINT64_C(5) << 20);
    g_assert_cmphex(qtest_readq(to, HEAD(1) + 0x130), ==,
                    (UINT64_C(0x34) << 48) |
                    UINT64_C(0x800001800090));
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x128), ==,
                    UINT64_C(0x1234567800000055));
    g_assert_cmphex(qtest_readq(to, HEAD(1) + 0x128), ==, 0x66);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x28), ==,
                    UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(to, HEAD(1) + 0x28), ==,
                    UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readl(to, CPU_HEAD(0) + 0x2c), ==,
                    0x89abcdef);
    g_assert_cmphex(qtest_readl(to, CPU_HEAD(1) + 0x2c), ==,
                    0x76543210);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x30), ==,
                    UINT64_C(0x1122334455667788));
    g_assert_cmphex(qtest_readq(to, HEAD(1) + 0x30), ==,
                    UINT64_C(0x8877665544332211));
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x38), ==,
                    UINT64_C(0x13579bdf2468ace0));
    g_assert_cmphex(qtest_readq(to, HEAD(1) + 0x38), ==,
                    UINT64_C(0x0eca8642fdb97531));
    g_assert_cmphex(qtest_readl(to, CPU_HEAD(0) + 0x600024), ==,
                    0x12345678);
    g_assert_cmphex(qtest_readl(to, BASE + 0x44000024), ==, 0x12345678);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_runtime_old_identity_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from;
    QTestState *to;

    if (!g_getenv("QTEST_QEMU_BINARY_SRC")) {
        g_test_skip("QTEST_QEMU_BINARY_SRC is not set");
        return;
    }
    dir = g_dir_make_tmp("sgi-pic-runtime-old-id-XXXXXX", &error);
    g_assert_no_error(error);
    from = pic_profile_v7_migration_start(false, true);
    to = pic_profile_v7_migration_start(true, false);
    for (unsigned head = 0; head < 2; head++) {
        assert_pic_identity_word(qtest_readq(from, HEAD(head)), head,
                                 0x24, 0);
        assert_pic_identity_word(qtest_readl(from, CPU_HEAD(head) + 4), head,
                                 0x24, 0);
        qtest_writeq(from, HEAD(head) + 0x20, head + 5);
    }
    qtest_writeq(from, HEAD(0) + 0x80, UINT64_C(5) << 20);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    assert_runtime_pic_identity(to);
    for (unsigned head = 0; head < 2; head++) {
        g_assert_cmphex(qtest_readq(to, HEAD(head) + 0x20), ==, head + 5);
    }
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x80), ==,
                    UINT64_C(5) << 20);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_runtime_profile_direct64_migration_and_dma(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-direct64-migration-XXXXXX",
                                          &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    const uint64_t payload = 0x4ffc;
    const uint64_t widget = UINT64_C(0xa) << 60;
    uint8_t bytes[8];
    QTestState *from = pic_runtime_cpu_start(true, NULL);
    QTestState *to = pic_runtime_cpu_start(true, "-incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    for (unsigned head = 0; head < 2; head++) {
        direct64_card_enable(from, head);
    }
    direct64_card_write(from, 0, widget | (UINT64_C(1) << 55) | payload,
                        sizeof(bytes), 0x41);
    qtest_memread(from, TARGET + payload, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0x41 + i);
    }

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    qtest_memread(to, TARGET + payload, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0x41 + i);
    }

    direct64_card_write(to, 1, widget | payload, sizeof(bytes), 0x71);
    qtest_memread(to, TARGET + payload, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i ^ 3], ==, 0x71 + i);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_runtime_profile_migration_rejects_fixture_profile(void)
{
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 30 * G_TIME_SPAN_SECOND, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr(
            "*PIC migration compatibility profile does not match destination*");
        return;
    }

    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-profile-mismatch-XXXXXX",
                                          &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_profile_migration_start(true, false, false);
    QTestState *to = pic_profile_migration_start(false, true, false);
    int64_t deadline;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_probe_child(to)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_runtime_profile_v5_migration_rejects_cpu_pio_policy(void)
{
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 30 * G_TIME_SPAN_SECOND, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr(
            "*PIC migration CPU PIO policy does not match destination*");
        return;
    }

    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-v5-policy-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_profile_migration_start(true, false, true);
    QTestState *to = pic_profile_migration_start(true, true, false);
    int64_t deadline;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_probe_child(to)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_runtime_profile_v6_migration_rejects_direct64_policy(void)
{
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 30 * G_TIME_SPAN_SECOND, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr("*PIC direct64 policy mismatch*");
        return;
    }

    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-v6-direct64-XXXXXX",
                                          &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_profile_v6_migration_start(false);
    QTestState *to = pic_profile_migration_start(true, true, false);
    int64_t deadline;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_probe_child(to)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_runtime_profile_migration_rejects_requester_width_policy(void)
{
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 30 * G_TIME_SPAN_SECOND, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr(
            "*PIC PCI requester width policy mismatch*");
        return;
    }

    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-width-mismatch-XXXXXX",
                                          &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_profile_width_migration_start(false, false, 0x02);
    QTestState *to = pic_profile_width_migration_start(true, false, 0x04);
    int64_t deadline;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_probe_child(to)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void
test_runtime_profile_v8_migration_rejects_attribute_policy(void)
{
    if (!g_getenv("QTEST_QEMU_BINARY_SRC")) {
        g_test_skip("QTEST_QEMU_BINARY_SRC is needed for the version-8 source");
        return;
    }
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 30 * G_TIME_SPAN_SECOND, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr(
            "*PIC direct64 attribute policy mismatch*");
        return;
    }

    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-v8-policy-mismatch-XXXXXX",
                                          &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_profile_width_migration_start(false, true, 0);
    QTestState *to = pic_profile_width_migration_start(true, false, 0x02);
    int64_t deadline;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_probe_child(to)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_runtime_profile_v7_migration_rejects_attribute_policy(void)
{
    if (!g_getenv("QTEST_QEMU_BINARY_SRC")) {
        g_test_skip("QTEST_QEMU_BINARY_SRC is needed for the version-7 source");
        return;
    }
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 30 * G_TIME_SPAN_SECOND, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr(
            "*PIC direct64 attribute policy mismatch*");
        return;
    }

    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-v7-policy-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_profile_v7_migration_start(false, true);
    QTestState *to = pic_profile_v7_migration_start(true, false);
    int64_t deadline;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_probe_child(to)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_pic_v1_migration_defaults_generic_mode(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-v1-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_migration_version_start(true, false, false);
    QTestState *to = pic_migration_version_start(false, true, true);
    QDict *reply;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    reply = qtest_qmp(to,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'x-test-migration-bedrock-interrupts'}}", PIC);
    g_assert_true(qdict_haskey(reply, "return"));
    g_assert_false(qdict_get_bool(reply, "return"));
    qobject_unref(reply);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_direct_dma_head_targets(void)
{
    QTestState *qts = qtest_init("-M none -cpu R10000 -S "
        "-device sgi-xio-test-target,id=near,size=65536 "
        "-device sgi-xio-test-target,id=far,size=65536 "
        "-device sgi-pic,id=pic,test-fixture=on,"
        "target-0=/machine/peripheral/near/near.ram[0],"
        "target-1=/machine/unattached/system[0] "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1");
    uint8_t near[8];
    uint8_t far[8];
    const uint8_t zeros[8] = { 0 };
    unsigned i;

    qtest_sysbus_mmio_map(qts, PIC, 0, BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/near", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/far", 0, 0x80000000);
    for (i = 0; i < 2; i++) {
        qtest_writel(qts, HEAD(i) + 0x21010, 0x01000000);
        qtest_writel(qts, HEAD(i) + 0x21004, PCI_COMMAND_MEMORY |
                     PCI_COMMAND_MASTER);
        qtest_writel(qts, CARD(i) + 4, 8);
    }
    qtest_writeq(qts, HEAD(0) + 0x80, 0);
    qtest_writeq(qts, HEAD(1) + 0x80, (1ULL << 20) | 1);
    qtest_writel(qts, CARD(0), 0x80001000);
    qtest_writel(qts, CARD(0) + 8, 0x11);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_writel(qts, CARD(1), 0x80002000);
    qtest_writel(qts, CARD(1) + 8, 0x21);
    qtest_writel(qts, CARD(1) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x1000, near, sizeof(near));
    qtest_memread(qts, 0x80002000, far, sizeof(far));
    for (i = 0; i < sizeof(near); i++) {
        g_assert_cmpuint(near[i], ==, 0x11 + i);
        g_assert_cmpuint(far[i], ==, 0x21 + i);
    }
    qtest_memread(qts, 0x80001000, far, sizeof(far));
    g_assert_cmpmem(far, sizeof(far), zeros, sizeof(zeros));
    qtest_memread(qts, TARGET + 0x2000, near, sizeof(near));
    g_assert_cmpmem(near, sizeof(near), zeros, sizeof(zeros));
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x80), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x80), ==,
                    (1ULL << 20) | 1);

    qtest_writeq(qts, HEAD(0) + 0x80, (1ULL << 20) | 1);
    qtest_writel(qts, CARD(0), 0x80003000);
    qtest_writel(qts, CARD(0) + 8, 0x31);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x1000, near, sizeof(near));
    qtest_memread(qts, 0x80003000, far, sizeof(far));
    for (i = 0; i < sizeof(near); i++) {
        g_assert_cmpuint(near[i], ==, 0x11 + i);
        g_assert_cmpuint(far[i], ==, 0x31 + i);
    }
    qtest_memread(qts, TARGET + 0x3000, near, sizeof(near));
    g_assert_cmpmem(near, sizeof(near), zeros, sizeof(zeros));
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x80), ==,
                    (1ULL << 20) | 1);
    qtest_writel(qts, CARD(1) + 8, 0x41);
    qtest_writel(qts, CARD(1) + 0xc, 1);
    qtest_memread(qts, 0x80002000, far, sizeof(far));
    for (i = 0; i < sizeof(far); i++) {
        g_assert_cmpuint(far[i], ==, 0x41 + i);
    }
    qtest_quit(qts);
}

static void test_direct_dma_end(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-dma-XXXXXX", &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log_contents = NULL;
    QTestState *qts;
    uint8_t sentinel[4] = { 0xa5, 0xa5, 0xa5, 0xa5 };
    uint8_t after[4];
    uint8_t bytes[4];
    unsigned i;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors -D %s "
        "-device sgi-xio-test-target,id=last,size=4096 "
        "-device sgi-xio-test-target,id=outside,size=4096 "
        "-device sgi-pic,id=pic,test-fixture=on,"
        "target-0=/machine/unattached/system[0] "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1",
        log_path);

    qtest_sysbus_mmio_map(qts, PIC, 0, BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/last", 0, 0x7ffff000);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/outside", 0, 0x80000000);
    qtest_memwrite(qts, 0x80000000, sentinel, sizeof(sentinel));
    qtest_writel(qts, HEAD(0) + 0x21010, 0x01000000);
    qtest_writel(qts, HEAD(0) + 0x21004, 6);
    qtest_writeq(qts, HEAD(0) + 0x80, 0);
    qtest_writel(qts, CARD(0), 0xffffffff);
    qtest_writel(qts, CARD(0) + 4, 1);
    qtest_writel(qts, CARD(0) + 8, 0x5a);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    g_assert_cmphex(qtest_readb(qts, 0x7fffffff), ==, 0x5a);

    /* QEMU splits this at the IOMMU boundary; the first four bytes land. */
    qtest_writel(qts, CARD(0), 0xfffffffc);
    qtest_writel(qts, CARD(0) + 4, 8);
    qtest_writel(qts, CARD(0) + 8, 0x90);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, 0x7ffffffc, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x90 + i);
    }
    qtest_memread(qts, 0x80000000, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), sentinel, sizeof(sentinel));

    qtest_writel(qts, CARD(0) + 0x18, 1);
    qtest_writel(qts, CARD(0), 0x80000000);
    qtest_writel(qts, CARD(0) + 4, 1);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_writel(qts, CARD(0) + 0x18, 0);
    qtest_writel(qts, CARD(0), 0x40000000);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, 0x80000000, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), sentinel, sizeof(sentinel));
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log_contents,
                            "sgi-pic: head 0 unsupported DMA 0x100000000"));
    g_assert_nonnull(strstr(log_contents,
                            "sgi-pic: head 0 unsupported DMA 0x180000000"));
    g_assert_nonnull(strstr(log_contents,
                            "sgi-pic: head 0 unsupported DMA 0x40000000"));
    unlink(log_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_pending_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_start(false, "");
    QTestState *to = pic_start(false, "-incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_writeq(from, HEAD(0) + 0x130, 0x1234000000001000ULL);
    qtest_writeq(from, HEAD(0) + 0x20, 0x123456789abcdef3ULL);
    qtest_writeq(from, HEAD(0) + 0x108, 1);
    qtest_writeq(from, HEAD(0) + 0x118, 1);
    qtest_set_irq_in(from, PIC "/head-0", NULL, 0, 1);
    g_assert_cmphex(qtest_readq(from, TARGET + 0x1000), ==, 0x11234);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x100), ==, 1);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x20), ==, 3);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x108), ==, 1);
    qtest_writeq(to, TARGET + 0x1000, 0);
    qtest_writeq(to, HEAD(0) + 0x110, 1);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x100), ==, 1);
    g_assert_cmphex(qtest_readq(to, TARGET + 0x1000), ==, 0x11234);
    qtest_set_irq_in(to, PIC "/head-0", NULL, 0, 0);
    g_assert_cmphex(qtest_readq(to, TARGET + 0x1000), ==, 0x1234);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_unwired_pci_interrupt(void)
{
    QTestState *qts = pic_start(true, "");

    qtest_writel(qts, HEAD(0) + 0x21010, 0x01000000);
    qtest_writel(qts, HEAD(0) + 0x21004, 6);
    qtest_irq_intercept_out_named(qts, PIC "/head-0", "pci-intx");
    qtest_writel(qts, CARD(0) + 0x10, 1);
    g_assert_true(qtest_get_irq(qts, 4));
    /* The machine must connect that output to one of eight PIC pin inputs. */
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 0);
    qtest_writel(qts, CARD(0) + 0x10, 0);
    g_assert_false(qtest_get_irq(qts, 4));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi-pic/short-device-io-windows",
                   test_short_device_io_windows);
    qtest_add_func("/sgi-pic/short-device-io-attributes-boundaries",
                   test_short_device_io_attributes_and_boundaries);
    qtest_add_func("/sgi-pic/short-device-io-migration",
                   test_short_device_io_migration);
    qtest_add_func("/sgi-pic/cpu-pio-conventional-lanes",
                   test_cpu_pio_conventional_lanes);
    qtest_add_func("/sgi-pic/cpu-pio-order-follows-pci-mapping",
                   test_cpu_pio_order_follows_pci_mapping);
    qtest_add_func("/sgi-pic/cpu-pio-overlapping-bars-use-active-function",
                   test_cpu_pio_overlapping_bars_use_active_function);
    qtest_add_func("/sgi-pic/ioc4-pci-status-compatibility",
                   test_ioc4_pci_status_compatibility);
    qtest_add_func("/sgi-pic/ioc4-pci-status-migration",
                   test_ioc4_pci_status_migration);
    qtest_add_func("/sgi-pic/ioc4-pci-status-migration-mode-mismatch",
                   test_ioc4_pci_status_migration_mode_mismatch);
    qtest_add_func("/sgi-pic/cpu-pio-actual-ioc4",
                   test_cpu_pio_actual_ioc4);
    qtest_add_func("/sgi-pic/cpu-pio-isp12160",
                   test_cpu_pio_isp12160);
    qtest_add_func("/sgi-pic/direct64-isp-load", test_direct64_isp_load);
    qtest_add_func("/sgi-pic/direct64-card-write", test_direct64_card_write);
    qtest_add_func("/sgi-pic/direct64-requester-and-dir-map-controls",
                   test_direct64_requester_and_dir_map_controls);
    qtest_add_func("/sgi-pic/direct64-requester-width",
                   test_direct64_requester_width);
    qtest_add_func("/sgi-pic/direct64-attribute-matrix",
                   test_direct64_attribute_matrix);
    qtest_add_func("/sgi-pic/direct64-kernel-queue-addresses",
                   test_direct64_kernel_queue_addresses);
    qtest_add_func("/sgi-pic/direct64-range-boundary-is-split",
                   test_direct64_range_boundary_is_split);
    qtest_add_func("/sgi-pic/direct64-crossbow-widget-routing",
                   test_direct64_crossbow_widget_routing);
    qtest_add_func("/sgi-pic/direct64-rejects-unsupported-addresses",
                   test_direct64_rejects_unsupported_addresses);
    qtest_add_func("/sgi-pic/direct64-target-error-preserves-data",
                   test_direct64_target_error_preserves_data);
    qtest_add_func("/sgi-pic/direct64-bedrock-high-payload-source-denial",
                   test_direct64_bedrock_high_payload_source_denial);
    qtest_add_func("/sgi-pic/heads-and-widths", test_heads_and_widths);
    qtest_add_func("/sgi-pic/configuration", test_configuration);
    qtest_add_func("/sgi-pic/slot-bounds", test_slot_bounds);
    qtest_add_func("/sgi-pic/unresolved-default-identity",
                   test_unresolved_default_identity);
    qtest_add_func("/sgi-pic/unconnected-linked-transport",
                   test_unconnected_linked_transport);
    qtest_add_func("/sgi-pic/bedrock-profile-is-fixture-only",
                   test_bedrock_profile_is_fixture_only);
    qtest_add_func("/sgi-pic/runtime-profile-rejects-fixture-mode",
                   test_runtime_profile_rejects_fixture_mode);
    qtest_add_func("/sgi-pic/runtime-profile-non-qtest-static-sysbus-refusal",
                   test_runtime_profile_non_qtest_cli_refuses_static_sysbus);
    qtest_add_func("/sgi-pic/runtime-profile-derives-linked-transport",
                   test_runtime_profile_derives_linked_transport);
    qtest_add_func("/sgi-pic/runtime-profile-immutable-after-realize",
                   test_runtime_profile_is_immutable_after_realize);
    qtest_add_func("/sgi-pic/runtime-profile-reserved-device-and-dma",
                   test_runtime_profile_reserved_device_and_dma);
    qtest_add_func("/sgi-pic/runtime-short-device-attribute-refusal",
                   test_runtime_short_device_attribute_refusal);
    qtest_add_func("/sgi-pic/runtime-reset-compatibility-trial",
                   test_runtime_reset_compatibility_trial);
    qtest_add_func("/sgi-pic/tezro-widget-register-profile-trial",
                   test_tezro_widget_register_profile_trial);
    qtest_add_func("/sgi-pic/runtime-invalid-address-error-trial",
                   test_runtime_invalid_address_error_trial);
    qtest_add_func("/sgi-pic/runtime-dir-map-unsupported-fields",
                   test_runtime_dir_map_unsupported_fields);
    qtest_add_func("/sgi-pic/type1-configuration", test_type1_configuration);
    qtest_add_func("/sgi-pic/unsupported-registers",
                   test_unsupported_registers);
    qtest_add_func("/sgi-pic/identity-fields", test_identity_fields);
    qtest_add_func("/sgi-pic/runtime-identity-bit", test_runtime_identity_bit);
    qtest_add_func("/sgi-pic/runtime-host-error-field",
                   test_runtime_host_error_field);
    qtest_add_func("/sgi-pic/cpu-word-view-registers",
                   test_cpu_word_view_registers);
    qtest_add_func("/sgi-pic/cpu-word-view-profile-isolation",
                   test_cpu_word_view_profile_isolation);
    qtest_add_func("/sgi-pic/widget-registers", test_widget_registers);
    qtest_add_func("/sgi-pic/cpu-word-view-interrupt-effects",
                   test_cpu_word_view_interrupt_effects);
    qtest_add_func("/sgi-pic/current-identity-migration",
                   test_current_identity_migration);
    qtest_add_func("/sgi-pic/pin-and-reset", test_pin_and_reset);
    qtest_add_func("/sgi-pic/remaining-pin-edges",
                   test_remaining_pin_edges);
    qtest_add_func("/sgi-pic/direct-dma", test_direct_dma);
    qtest_add_func("/sgi-pic/linked-dma", test_linked_dma);
    qtest_add_func("/sgi-pic/linked-receiving-ii-denial",
                   test_linked_receiving_ii_denial);
    qtest_add_func("/sgi-pic/linked-serving-ii-denial",
                   test_linked_serving_ii_denial);
    qtest_add_func("/sgi-pic/linked-partial-callback",
                   test_linked_partial_callback);
    qtest_add_func("/sgi-pic/linked-route-rejection",
                   test_linked_route_rejection);
    qtest_add_func("/sgi-pic/linked-nested-dma", test_linked_nested_dma);
    qtest_add_func("/sgi-pic/linked-interrupts", test_linked_interrupts);
    qtest_add_func("/sgi-pic/linked-interrupt-window-bounds",
                   test_linked_interrupt_window_bounds);
    qtest_add_func("/sgi-pic/packet-window-rejects-small-base",
                   test_packet_window_rejects_small_base);
    qtest_add_func("/sgi-pic/packet-window-rejects-out-of-packet-base",
                   test_packet_window_rejects_out_of_packet_base);
    qtest_add_func("/sgi-pic/packet-window-rejects-overflow",
                   test_packet_window_rejects_overflow);
    qtest_add_func("/sgi-pic/packet-window-rejects-target-port",
                   test_packet_window_rejects_target_port);
    qtest_add_func("/sgi-pic/packet-window-exact-limit-fit",
                   test_packet_window_exact_limit_fit);
    qtest_add_func("/sgi-pic/linked-bedrock-interrupt",
                   test_linked_bedrock_interrupt);
    qtest_add_func("/sgi-pic/runtime-profile-bedrock-interrupt",
                   test_runtime_profile_bedrock_interrupt);
    qtest_add_func("/sgi-pic/runtime-profile-bedrock-level-mirror",
                   test_runtime_profile_bedrock_level_mirror);
    qtest_add_func("/sgi-pic/runtime-profile-bedrock-rearm-one-level",
                   test_runtime_profile_bedrock_rearm_one_level);
    qtest_add_func("/sgi-pic/runtime-profile-bedrock-rearm-staggered-levels",
                   test_runtime_profile_bedrock_rearm_staggered_levels);
    qtest_add_func("/sgi-pic/runtime-profile-bedrock-rearm-migration",
                   test_runtime_profile_bedrock_rearm_migration);
    qtest_add_func("/sgi-pic/linked-bedrock-interrupt-node-route",
                   test_linked_bedrock_interrupt_node_route);
    qtest_add_func("/sgi-pic/bedrock-interrupt-level-bounds",
                   test_bedrock_interrupt_level_bounds);
    qtest_add_func("/sgi-pic/linked-migration", test_linked_migration);
    qtest_add_func("/sgi-pic/linked-migration-mismatch",
                   test_linked_migration_mismatch);
    qtest_add_func("/sgi-pic/bedrock-interrupt-mode-mismatch",
                   test_bedrock_interrupt_mode_mismatch);
    qtest_add_func("/sgi-pic/runtime-profile-migration",
                   test_runtime_profile_migration);
    qtest_add_func("/sgi-pic/runtime-profile-direct64-migration-and-dma",
                   test_runtime_profile_direct64_migration_and_dma);
    qtest_add_func("/sgi-pic/runtime-profile-migration-profile-mismatch",
                   test_runtime_profile_migration_rejects_fixture_profile);
    qtest_add_func("/sgi-pic/runtime-profile-v5-migration-pio-policy-mismatch",
                   test_runtime_profile_v5_migration_rejects_cpu_pio_policy);
    qtest_add_func(
        "/sgi-pic/runtime-profile-v6-migration-direct64-policy-mismatch",
        test_runtime_profile_v6_migration_rejects_direct64_policy);
    qtest_add_func(
        "/sgi-pic/runtime-profile-migration-requester-width-policy-mismatch",
        test_runtime_profile_migration_rejects_requester_width_policy);
    qtest_add_func("/sgi-pic/v1-migration-defaults-generic-mode",
                   test_pic_v1_migration_defaults_generic_mode);
    qtest_add_func("/sgi-pic/direct-dma-head-targets",
                   test_direct_dma_head_targets);
    qtest_add_func("/sgi-pic/direct-dma-end", test_direct_dma_end);
    qtest_add_func("/sgi-pic/pending-migration", test_pending_migration);
    qtest_add_func("/sgi-pic/unwired-pci-interrupt",
                   test_unwired_pci_interrupt);
    qtest_add_func("/sgi-pic/bedrock-ioc4-serial-dma",
                   test_bedrock_ioc4_serial_dma);
    qtest_add_func("/sgi-pic/bedrock-ioc4-interrupt",
                   test_bedrock_ioc4_interrupt);
    qtest_add_func("/sgi-pic/bedrock-ioc4-pending-migration",
                   test_bedrock_ioc4_pending_migration);
    if (g_getenv("QTEST_QEMU_BINARY_SRC")) {
        qtest_add_func("/sgi-pic/old-identity-migration",
                       test_old_identity_migration);
        qtest_add_func("/sgi-pic/runtime-old-identity-migration",
                       test_runtime_old_identity_migration);
        qtest_add_func(
            "/sgi-pic/runtime-profile-v7-migration-attribute-policy-mismatch",
            test_runtime_profile_v7_migration_rejects_attribute_policy);
        qtest_add_func(
            "/sgi-pic/runtime-profile-v8-migration-direct64-attribute-policy-mismatch",
            test_runtime_profile_v8_migration_rejects_attribute_policy);
    }
    return g_test_run();
}
