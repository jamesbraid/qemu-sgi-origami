/* SPDX-License-Identifier: BSD-3-Clause */
/* PIC revision 3 compatibility rules, with synthetic PCI fixture identities. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
#include "qemu/bswap.h"
#include "hw/pci/pci_regs.h"
#include "system/memory.h"
#include <poll.h>
#include <sys/socket.h>

#define PIC "/machine/peripheral/pic"
#define PIC_CHILD "/machine/peripheral/fabric/pic"
#define BASE 0x100000000000ULL
#define HEAD(n) (BASE + (n) * 0x800000ULL)
#define TARGET 0x20000000ULL
#define II_REGISTER (1ULL << 47)
#define XIO_WIDGET(n) ((uint64_t)(n) << 48)
#define II_NODE(n) ((uint64_t)(n) << 39)
#define II_HUB_REMOTE 0x1800000ULL
#define INGRESS_REGS 0x50000000ULL
#define SERVING_REGS 0x51000000ULL
#define CARD(n) (BASE + (n ? 0xc0000000ULL : 0x40000000ULL) + 0x01000000)
#define IOC4_CONFIG (HEAD(0) + 0x21000)
#define IOC4_BAR 0x01000000ULL
#define IOC4_MEMORY (HEAD(0) + 0x40000000ULL + IOC4_BAR)
#define IOC4_UART0 (IOC4_MEMORY + 0x380)
#define IOC4_DMA_BASE 0x300
#define IOC4_DMA_CONTROL 0x310
#define IOC4_DMA_TX_PRODUCE 0x314
#define IOC4_DMA_RX_TIMER 0x324
#define IOC4_DMA_RX_PRODUCE 0x31c
#define IOC4_DMA_RX_CONSUME 0x320
#define IOC4_DMA_ENABLE (1U << 28)
#define IOC4_DMA_DRAIN (1U << 27)
#define IOC4_RX_DATA_VALID 0x80
#define IOC4_UART_PASS 0x40
#define IOC4_UART_DATA 0x41
#define IOC4_IRQ_DESTINATION \
    ((UINT64_C(0x1234) << 48) | II_REGISTER | II_NODE(1) | \
     II_HUB_REMOTE | 0x1000)

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

static QTestState *pic_bedrock_parent_start(bool map_io_registers)
{
    QTestState *qts = qtest_init(
        "-M none -cpu R10000 -m 256M -S -d unimp,guest_errors "
        "-device sgi-xio-test-target,id=memory,size=16777216 "
        "-device sgi-bedrock,id=bedrock,processor-count=1,"
        "dma-memory=/machine/peripheral/memory/memory.ram[0] "
        "-device sgi-xio-test-target,id=fabric,size=16777216,pic-peer=on,"
        "pic-linked=on,pic-switched=on,pic-bedrock-interrupts=on,"
        "target-port=/machine/peripheral/bedrock/io");

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/bedrock", 0,
                          0x10000000);
    if (map_io_registers) {
        qtest_sysbus_mmio_map(qts, "/machine/peripheral/bedrock", 4,
                              0x3000000000000ULL);
    }
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 3, BASE);
    return qts;
}

static QTestState *pic_bedrock_start(void)
{
    QTestState *qts = qtest_init(
        "-M none -cpu R10000 -S -d unimp,guest_errors "
        "-device sgi-xio-test-target,id=memory,size=16777216 "
        "-device sgi-bedrock-io,id=serving,node-number=1,"
        "device-mask-reset=0x0000ffffffffffff,"
        "dma-memory=/machine/peripheral/memory/memory.ram[0],"
        "hub-registers=/machine/peripheral/memory/memory.ram[0] "
        "-device sgi-bedrock-io,id=ingress,node-number=0,"
        "dma-memory=/machine/peripheral/memory/memory.ram[0],"
        "hub-registers=/machine/peripheral/memory/memory.ram[0] "
        "-device sgi-xio-test-target,id=fabric,size=16777216,pic-peer=on,"
        "pic-bedrock-interrupts=on,target-port=ingress,"
        "target-peer=serving,pic-switched=on "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1,"
        "vendor-id=0xfeed,device-id=0x3500 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1,"
        "vendor-id=0xfeed,device-id=0x3501");

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/memory", 0, TARGET);
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
    pic_ioc4_write_reg(qts, IOC4_DMA_RX_TIMER, 0x3a5);
    g_assert_cmphex(pic_ioc4_read_reg(qts, IOC4_DMA_RX_TIMER), ==, 0x3a5);
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
    g_assert_cmphex(entry[4], ==, IOC4_RX_DATA_VALID);
    g_assert_cmphex(entry[5], ==, IOC4_RX_DATA_VALID);
    g_assert_cmphex(entry[6], ==, IOC4_RX_DATA_VALID);
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

    g_assert_cmphex(qtest_readb(to, IOC4_UART0 + 2), ==, 2);
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
    g_assert_cmphex(entry[4], ==, IOC4_RX_DATA_VALID);
    g_assert_cmphex(entry[5], ==, IOC4_RX_DATA_VALID);
    g_assert_cmphex(entry[6], ==, IOC4_RX_DATA_VALID);

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
    QTestState *qts = pic_bedrock_start();
    uint8_t sentinel[8];
    uint8_t bytes[8];

    memset(sentinel, 0xa5, sizeof(sentinel));
    qtest_memwrite(qts, TARGET + 0x1000, sentinel, sizeof(sentinel));
    pic_bedrock_configure(qts, false);
    qtest_writeq(qts, INGRESS_REGS + 0x118,
                 qtest_readq(qts, INGRESS_REGS + 0x118) & ~(1ULL << 14));
    pic_bedrock_dma(qts, 0x61);
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
    QTestState *qts = pic_bedrock_start();
    uint8_t sentinel[8];
    uint8_t bytes[8];

    memset(sentinel, 0xa5, sizeof(sentinel));
    qtest_memwrite(qts, TARGET + 0x1000, sentinel, sizeof(sentinel));
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
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
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
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x81 + i);
    }
    g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 4);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_OK);
    qtest_writel(qts, CARD(1) + 8, 0x91);
    qtest_writel(qts, CARD(1) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
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

    g_assert_cmphex(qtest_readq(qts, HEAD(0)), ==, 0x3d102024);
    g_assert_cmphex(qtest_readq(qts, HEAD(1)), ==, 0x3d112024);
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

static void test_linked_bedrock_interrupt(void)
{
    uint64_t pending_bit = UINT64_C(1) << 52;
    uint64_t destination = (UINT64_C(0x34) << 48) | 0x800001800090;
    unsigned map_io_registers;

    for (map_io_registers = 0; map_io_registers < 2; map_io_registers++) {
        QTestState *qts = pic_bedrock_parent_start(map_io_registers);

        qtest_writeq(qts, HEAD(0) + 0x20, 14);
        qtest_writeq(qts, HEAD(0) + 0x130, destination);
        qtest_writeq(qts, HEAD(0) + 0x118, 1);
        qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 1);
        qtest_writeq(qts, HEAD(0) + 0x108, 1);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 1);
        g_assert_cmphex(target_metric(qts, "x-test-packet-address"), ==,
                        0x800001800090);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-widget"), ==, 14);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-size"), ==, 8);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-device-known"),
                         ==, 0);
        g_assert_cmphex(target_metric(qts, "x-test-packet-result"), ==,
                        MEMTX_OK);
        g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & pending_bit,
                        ==, pending_bit);

        qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 0);
        g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & pending_bit,
                        ==, 0);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                         MEMTX_OK);
        g_assert_cmpuint(target_metric(qts, "x-test-packet-calls"), ==, 2);

        qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 1);
        g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & pending_bit,
                        ==, pending_bit);

        qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
        g_assert_cmphex(qtest_readq(qts, 0x10000000 + 0x98) & pending_bit,
                        ==, 0);
        qtest_quit(qts);
    }
}

static void test_linked_bedrock_interrupt_node_route(void)
{
    static const uint8_t set_value[8] = { 0, 0, 0, 0, 0, 0, 1, 0x34 };
    static const uint8_t clear_value[8] = { 0, 0, 0, 0, 0, 0, 0, 0x34 };
    QTestState *qts = pic_bedrock_start();
    uint64_t address = II_REGISTER | II_NODE(1) | II_HUB_REMOTE | 0x90;
    uint64_t destination = (UINT64_C(0x34) << 48) | address;
    uint8_t bytes[8];

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
    qtest_memread(qts, TARGET + 0x90, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), set_value, sizeof(set_value));

    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 0);
    qtest_memread(qts, TARGET + 0x90, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), clear_value, sizeof(clear_value));

    qtest_writeq(qts, INGRESS_REGS + 0x118, UINT64_MAX ^ (UINT64_C(1) << 14));
    qtest_set_irq_in(qts, PIC_CHILD "/head-0", NULL, 0, 1);
    qtest_writeq(qts, HEAD(0) + 0x108, 1);
    g_assert_cmpuint(target_metric(qts, "x-test-packet-result"), ==,
                     MEMTX_ACCESS_ERROR);
    qtest_memread(qts, TARGET + 0x90, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), clear_value, sizeof(clear_value));
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
    qtest_memread(qts, TARGET + 0x90, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), clear_value, sizeof(clear_value));
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
    qtest_add_func("/sgi-pic/heads-and-widths", test_heads_and_widths);
    qtest_add_func("/sgi-pic/configuration", test_configuration);
    qtest_add_func("/sgi-pic/slot-bounds", test_slot_bounds);
    qtest_add_func("/sgi-pic/unresolved-default-identity",
                   test_unresolved_default_identity);
    qtest_add_func("/sgi-pic/unconnected-linked-transport",
                   test_unconnected_linked_transport);
    qtest_add_func("/sgi-pic/type1-configuration", test_type1_configuration);
    qtest_add_func("/sgi-pic/unsupported-registers",
                   test_unsupported_registers);
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
    qtest_add_func("/sgi-pic/linked-bedrock-interrupt-node-route",
                   test_linked_bedrock_interrupt_node_route);
    qtest_add_func("/sgi-pic/bedrock-interrupt-level-bounds",
                   test_bedrock_interrupt_level_bounds);
    qtest_add_func("/sgi-pic/linked-migration", test_linked_migration);
    qtest_add_func("/sgi-pic/linked-migration-mismatch",
                   test_linked_migration_mismatch);
    qtest_add_func("/sgi-pic/bedrock-interrupt-mode-mismatch",
                   test_bedrock_interrupt_mode_mismatch);
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
    return g_test_run();
}
