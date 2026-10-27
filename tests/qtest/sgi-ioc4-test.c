/*
 * IOC4 revision 0x53 serial register tests through a standalone XBridge.
 * This fixture tests the PCI function without assuming an IO9 board route.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-migrate.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include <poll.h>
#include <sys/socket.h>

#define CHIP "/machine/peripheral/xbridge"
#define WIDGET (UINT64_C(0x100000000000) + (UINT64_C(0xe) << 44))
#define CFG (WIDGET + 0x20000)
#define BRIDGE_DEVICE0 (WIDGET + 0x204)
#define PCI_COMMAND_MASTER (1u << 2)
#define BAR (WIDGET + 0x200000)
#define ATA_READBACK 0x148
#define PCI_ADDRESS 0x01000000u
#define UART(n) (BAR + 0x380 + 8 * (n))
#define RTC(n) (BAR + 0x87ff8 + (n))
#define KBC_CONTROL 0x200
#define KBC_KBD_RECEIVE 0x204
#define KBC_AUX_RECEIVE 0x208
#define KBC_KBD_TRANSMIT 0x20c
#define KBC_AUX_TRANSMIT 0x210
#define KBC_OTHER_IRQ 0x40
#define KBC_WRITE_PENDING 0x03
#define PASS(n) (0x40u << (8 * (n)))
#define DMA_RAM_PATH "/machine/peripheral/fabric"
#define DMA_RAM_BASE UINT64_C(0x20000000)
#define DMA_HOST_WIDGET (UINT64_C(0xb) << 60)
#define DMA_SWAP_BIT (UINT64_C(1) << 55)
#define DMA_RAM_OFFSET 0x4000u
#define DMA_PAIR_BASE (DMA_HOST_WIDGET | DMA_SWAP_BIT | DMA_RAM_OFFSET)
#define DMA_BAD_BASE (DMA_HOST_WIDGET + 0x100000)
#define SDMA_BASE_LO 0x300
#define SDMA_BASE_HI 0x304
#define SIO_CR 0x20
#define SIO_DIAG_IDLE 0x80
#define SDMA_PORT_BASE(n) (0x310 + 0x1c * (n))
#define SDMA_CONTROL(n) (SDMA_PORT_BASE(n) + 0x00)
#define SDMA_TX_PRODUCE(n) (SDMA_PORT_BASE(n) + 0x04)
#define SDMA_TX_CONSUME(n) (SDMA_PORT_BASE(n) + 0x08)
#define SDMA_RX_PRODUCE(n) (SDMA_PORT_BASE(n) + 0x0c)
#define SDMA_RX_CONSUME(n) (SDMA_PORT_BASE(n) + 0x10)
#define SDMA_RX_TIMER(n) (SDMA_PORT_BASE(n) + 0x14)
#define SDMA_SHADOW(n) (SDMA_PORT_BASE(n) + 0x18)
#define SSCR_DMA_EN (1u << 28)
#define SSCR_PAUSE (1u << 29)
#define SSCR_PAUSE_STATE (1u << 30)
#define SSCR_RESET (1u << 31)
#define SSCR_DRAIN (1u << 27)
#define SSCR_THRESHOLD(n) ((n) & 0x1ff)
#define SDMA_SHADOW_OUTPUTS 0x1f000000
#define SDMA_SHADOW_LOOPBACK 0x10000000
#define SRCIR_TIMER_ARM (1u << 31)
#define TX_VALID 0x40
#define TX_EXPLICIT 0x20
#define TX_MCR 0x80
#define TX_DELAY 0xc0
#define RX_DATA_VALID 0x80
#define RX_LOOPBACK_VALID 0xc0
#define RX_RING_FULL(n) (1u << (8 * (n) + 1))
#define RX_THRESHOLD(n) (1u << (8 * (n) + 2))
#define RX_TIMER_EXPIRED(n) (1u << (8 * (n) + 3))
#define TX_RING_EMPTY(n) (1u << (8 * (n) + 0))
#define TX_COMPLETION(n) (1u << (8 * (n) + 7))
#define DMA_ERROR(n) (1u << (n + 2))
#define ERROR_VALID 1u
#define ERROR_MULTIPLE (1u << 5)
#define ERROR_REQUESTER(n) ((n) << 1)
#define DMA_TEST_LOG_OPTIONS "-d guest_errors,unimp,invalid_mem -D "

static uint32_t reg_read(QTestState *qts, unsigned offset);
static void reg_write(QTestState *qts, unsigned offset, uint32_t value);
static char *ioc4_log_file_new(void);
static char *ioc4_log_file_take(const char *path);
static void ioc4_wait_migration_failed(QTestState *qts);

static uint64_t ioc4_target_property(QTestState *qts, const char *property)
{
    QDict *reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        "/machine/peripheral/fabric", property);
    uint64_t value;

    g_assert_true(qdict_haskey(reply, "return"));
    value = qdict_get_int(reply, "return");
    qobject_unref(reply);
    return value;
}

static void ioc4_map(QTestState *qts)
{
    qtest_writel(qts, CFG + 0x10, PCI_ADDRESS);
    qtest_writel(qts, CFG + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE_DEVICE0, 0x03010);
}

static QTestState *ioc4_start_full(unsigned clock_hz, bool incoming,
                                   bool rtc)
{
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S %s -d unimp,guest_errors,invalid_mem "
        "-device sgi-xio-test-target,id=fabric "
        "-device sgi-xbridge,id=xbridge,host-port=0xb,xio-port=0xc,"
        "fabric=/machine/peripheral/fabric/fabric.ram[0] "
        "%s"
        "-device sgi-ioc4,bus=head-e.0,addr=0,clock-hz=%u%s",
        incoming ? "-incoming defer" : "",
        rtc ? "-device ds1742,id=clock " : "", clock_hz,
        rtc ? ",clock=/machine/peripheral/clock/memory-region" : "");

    qtest_sysbus_mmio_map(qts, CHIP, 0, UINT64_C(0x100000000000));
    ioc4_map(qts);
    return qts;
}

static QTestState *ioc4_start(unsigned clock_hz)
{
    return ioc4_start_full(clock_hz, false, false);
}

static QTestState *ioc4_start_ps2(bool incoming, const char *log)
{
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -d unimp,guest_errors,invalid_mem -D %s "
        "-device sgi-xio-test-target,id=fabric "
        "-device sgi-xbridge,id=xbridge,host-port=0xb,xio-port=0xc,"
        "fabric=/machine/peripheral/fabric/fabric.ram[0] %s"
        "-device sgi-ioc4,id=ioc4,bus=head-e.0,addr=0",
        log, incoming ? "-incoming defer " : "");

    qtest_sysbus_mmio_map(qts, CHIP, 0, UINT64_C(0x100000000000));
    ioc4_map(qts);
    return qts;
}

static void ioc4_send_ps2_key(QTestState *qts, const char *qcode)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'key','data':{'down':true,'key':"
        "{'type':'qcode','data':%s}}}]}}", qcode);

    if (!qdict_haskey(response, "return")) {
        g_autoptr(GString) text = qobject_to_json(QOBJECT(response));
        g_error("IOC4 PS/2 key event failed: %s", text->str);
    }
    qobject_unref(response);
}

static void test_ps2_adapter(void)
{
    g_autofree char *log = ioc4_log_file_new();
    g_autofree char *contents = NULL;
    QTestState *qts = ioc4_start_ps2(false, log);

    qtest_irq_intercept_out_named(qts, CHIP, "head-e-pci-intx");
    g_assert_cmphex(reg_read(qts, KBC_CONTROL), ==, 0);
    g_assert_cmphex(reg_read(qts, KBC_KBD_RECEIVE), ==, 0);
    g_assert_cmphex(reg_read(qts, KBC_AUX_RECEIVE), ==, 0);
    reg_write(qts, KBC_CONTROL, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, KBC_CONTROL), ==, 0x00fcfffc);
    reg_write(qts, KBC_CONTROL, 0);
    qtest_readb(qts, BAR + KBC_CONTROL);
    qtest_writew(qts, BAR + KBC_KBD_TRANSMIT, 0xff);
    g_assert_cmphex(reg_read(qts, KBC_KBD_RECEIVE), ==, 0);

    reg_write(qts, KBC_KBD_TRANSMIT, 0xff000000);
    g_assert_cmphex(reg_read(qts, KBC_KBD_RECEIVE), ==, 0x80fa0000);
    reg_write(qts, KBC_KBD_TRANSMIT, 0xff);
    g_assert_cmphex(reg_read(qts, KBC_KBD_RECEIVE), ==, 0xc0faaa00);
    g_assert_cmphex(reg_read(qts, KBC_CONTROL) & KBC_WRITE_PENDING, ==, 0);
    reg_write(qts, 0x0c, KBC_OTHER_IRQ);

    ioc4_send_ps2_key(qts, "a");
    g_assert_cmphex(reg_read(qts, 0x0c) & KBC_OTHER_IRQ, ==, KBC_OTHER_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));
    reg_write(qts, 0x14, KBC_OTHER_IRQ);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(reg_read(qts, KBC_KBD_RECEIVE), ==, 0x801c0000);
    g_assert_cmphex(reg_read(qts, KBC_KBD_RECEIVE) & 0x1f000000, ==, 0);
    g_assert_cmphex(reg_read(qts, 0x0c) & KBC_OTHER_IRQ, ==, KBC_OTHER_IRQ);

    qtest_writeb(qts, UART(0) + 3, 0x80);
    qtest_writeb(qts, UART(0), 0x34);
    qtest_writeb(qts, UART(0) + 1, 0x12);
    qtest_writeb(qts, UART(0) + 3, 0x03);
    qtest_writeb(qts, UART(0) + 2, 0x01);
    qtest_writeb(qts, UART(0) + 1, 0x02);
    reg_write(qts, 0x10, PASS(0));
    g_assert_cmphex(reg_read(qts, 0x08) & PASS(0), ==, PASS(0));
    g_assert_true(qtest_get_irq(qts, 0));

    reg_write(qts, 0x1c, KBC_OTHER_IRQ);
    g_assert_true(qtest_get_irq(qts, 0));
    reg_write(qts, 0x14, KBC_OTHER_IRQ);
    g_assert_true(qtest_get_irq(qts, 0));
    reg_write(qts, 0x0c, KBC_OTHER_IRQ);
    g_assert_cmphex(reg_read(qts, 0x0c) & KBC_OTHER_IRQ, ==, 0);
    g_assert_cmphex(reg_read(qts, 0x08) & PASS(0), ==, PASS(0));
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writeb(qts, UART(0) + 1, 0);
    reg_write(qts, 0x08, PASS(0));
    g_assert_false(qtest_get_irq(qts, 0));

    reg_write(qts, KBC_AUX_TRANSMIT, 0xff);
    g_assert_cmphex(reg_read(qts, KBC_AUX_RECEIVE), ==, 0xe0faaa00);
    g_assert_cmphex(reg_read(qts, KBC_CONTROL) & KBC_WRITE_PENDING, ==, 0);
    reg_write(qts, 0x0c, KBC_OTHER_IRQ);
    reg_write(qts, KBC_AUX_TRANSMIT, 0xf4);
    g_assert_cmphex(reg_read(qts, KBC_AUX_RECEIVE), ==, 0x80fa0000);
    reg_write(qts, 0x0c, KBC_OTHER_IRQ);
    {
        QDict *response = qtest_qmp(qts,
        "{'execute':'input-send-event','arguments':{'events':["
        "{'type':'rel','data':{'axis':'x','value':4}},"
        "{'type':'btn','data':{'down':true,'button':'left'}}]}}");
        if (!qdict_haskey(response, "return")) {
            g_autoptr(GString) text = qobject_to_json(QOBJECT(response));
            g_error("IOC4 PS/2 mouse event failed: %s", text->str);
        }
        qobject_unref(response);
    }
    qtest_clock_step(qts, 10 * 1000000ULL);
    g_assert_cmphex(reg_read(qts, KBC_AUX_RECEIVE), ==, 0xe0090400);

    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    ioc4_map(qts);
    g_assert_cmphex(reg_read(qts, KBC_CONTROL), ==, 0);
    g_assert_cmphex(reg_read(qts, KBC_KBD_RECEIVE), ==, 0);
    g_assert_cmphex(reg_read(qts, KBC_AUX_RECEIVE), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x0c) & KBC_OTHER_IRQ, ==, 0);
    g_assert_cmphex(reg_read(qts, 0x14) & KBC_OTHER_IRQ, ==, 0);
    reg_write(qts, KBC_KBD_TRANSMIT, 0xff);
    g_assert_cmphex(reg_read(qts, KBC_KBD_RECEIVE), ==, 0xc0faaa00);

    qtest_quit(qts);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
    g_assert_nonnull(strstr(contents, "unsupported KBC read width 1"));
    g_assert_nonnull(strstr(contents, "unsupported KBC write width 2"));
}

/*
 * The keyboard and mouse ports are always there: every IO9 brings them
 * out, and the device has no switch to leave them unplugged.
 */
static void test_ps2_ports_present(void)
{
    g_autofree char *log = ioc4_log_file_new();
    QTestState *qts = ioc4_start_ps2(false, log);
    QDict *reply;
    const QListEntry *e;

    reply = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                      "{'path':'/machine/peripheral/ioc4/kbc/kbd',"
                      "'property':'type'}}");
    g_assert_cmpstr(qdict_get_try_str(reply, "return"), ==, "ps2-kbd");
    qobject_unref(reply);
    reply = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                      "{'path':'/machine/peripheral/ioc4/kbc/mouse',"
                      "'property':'type'}}");
    g_assert_cmpstr(qdict_get_try_str(reply, "return"), ==, "ps2-mouse");
    qobject_unref(reply);
    reply = qtest_qmp(qts, "{'execute':'device-list-properties',"
                      "'arguments':{'typename':'sgi-ioc4'}}");
    QLIST_FOREACH_ENTRY(qdict_get_qlist(reply, "return"), e) {
        QDict *prop = qobject_to(QDict, qlist_entry_obj(e));

        g_assert_cmpstr(qdict_get_str(prop, "name"), !=, "ps2-input");
    }
    qobject_unref(reply);
    qtest_quit(qts);
    g_free(ioc4_log_file_take(log));
}

static QTestState *ioc4_start_readback(bool incoming, bool enabled)
{
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S %s -device sgi-xio-test-target,id=fabric "
        "-device sgi-xbridge,id=xbridge,host-port=0xb,xio-port=0xc,"
        "fabric=/machine/peripheral/fabric/fabric.ram[0] "
        "-device sgi-ioc4,bus=head-e.0,addr=0,ata-readback-compat=%s",
        incoming ? "-incoming defer" : "", enabled ? "on" : "off");

    qtest_sysbus_mmio_map(qts, CHIP, 0, UINT64_C(0x100000000000));
    ioc4_map(qts);
    return qts;
}

static char *ioc4_log_file_new(void)
{
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("sgi-ioc4-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    return g_steal_pointer(&path);
}

static char *ioc4_log_file_take(const char *path)
{
    char *contents = NULL;

    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_assert_cmpint(g_unlink(path), ==, 0);
    return contents;
}

static QTestState *ioc4_start_dma_full(bool incoming, bool enabled,
                                       unsigned serial_port, int serial_fd,
                                       const char *log, bool observer,
                                       bool fail_read, bool fail_write,
                                       bool nested)
{
    g_autofree char *args = NULL;
    g_autofree char *chardev = serial_fd >= 0 ?
        g_strdup_printf("-chardev socket,id=serial,fd=%d ", serial_fd) :
        g_strdup("");
    g_autofree char *attachment = serial_fd >= 0 ?
        g_strdup_printf(",chardev%u=serial", serial_port + 1) : g_strdup("");
    const char *profile = enabled ? ",serial-dma-experimental=on" : "";
    const char *observer_prop = observer ?
        ",dma-observer=on" : "";
    const char *fail_read_prop = fail_read ?
        ",dma-observer-fail-read=on" : "";
    const char *fail_write_prop = fail_write ?
        ",dma-observer-fail-write=on" : "";
    const char *nested_prop = nested ? ",dma-observer-nested=on" : "";
    const char *ioc4_id = observer ? ",id=ioc4" : "";
    const char *fabric = observer ?
        "/machine/peripheral/fabric/dma-observer[0]" :
        "/machine/peripheral/fabric/fabric.ram[0]";
    QTestState *qts;

    g_assert_cmpuint(serial_port, <, 4);

    args = g_strdup_printf(
        "-M none -cpu R10000 " DMA_TEST_LOG_OPTIONS "%s "
        "-device sgi-xio-test-target,id=fabric,size=0x10000%s%s%s%s "
        "-device sgi-xbridge,id=xbridge,host-port=0xb,xio-port=0xc,"
        "fabric=%s "
        "%s%s-device sgi-ioc4%s,bus=head-e.0,addr=0,"
        "clock-hz=33333333%s%s",
        log, observer_prop, fail_read_prop, fail_write_prop, nested_prop,
        fabric, chardev,
        incoming ? "-incoming defer " : "", ioc4_id, profile, attachment);
    qts = qtest_init(args);

    qtest_sysbus_mmio_map(qts, CHIP, 0, UINT64_C(0x100000000000));
    qtest_sysbus_mmio_map(qts, DMA_RAM_PATH, 0, DMA_RAM_BASE);
    ioc4_map(qts);
    qtest_writel(qts, CFG + 0x04, 0x00000006);
    return qts;
}

static QTestState *ioc4_start_dma(bool incoming, bool enabled,
                                  unsigned serial_port, int serial_fd,
                                  const char *log)
{
    return ioc4_start_dma_full(incoming, enabled, serial_port, serial_fd,
                               log, false, false, false, false);
}

static void ioc4_set_pair_base(QTestState *qts, unsigned pair,
                               uint64_t base, unsigned ring_size)
{
    unsigned offset = pair ? 0x308 : SDMA_BASE_LO;
    uint32_t size_bit = ring_size == 0x1000 ? 1 : 0;

    g_assert_true(ring_size == 0x400 || ring_size == 0x1000);
    reg_write(qts, offset + 4, base >> 32);
    reg_write(qts, offset, (uint32_t)base | size_bit);
}

static void ioc4_set_pair_base_default(QTestState *qts, unsigned pair,
                                      unsigned ring_size)
{
    ioc4_set_pair_base(qts, pair, DMA_PAIR_BASE, ring_size);
}

static void ioc4_dma_write(QTestState *qts, uint64_t offset,
                           const void *bytes, size_t size)
{
    qtest_memwrite(qts, DMA_RAM_BASE + offset, bytes, size);
}

static void ioc4_dma_read(QTestState *qts, uint64_t offset,
                          void *bytes, size_t size)
{
    qtest_memread(qts, DMA_RAM_BASE + offset, bytes, size);
}

static uint64_t ioc4_tx_ring_offset(unsigned port, unsigned ring_size)
{
    return DMA_RAM_OFFSET + ((port & 1) ? 2 : 0) * ring_size;
}

static uint64_t ioc4_rx_ring_offset(unsigned port, unsigned ring_size)
{
    return DMA_RAM_OFFSET + ((port & 1) ? 3 : 1) * ring_size;
}

static void ioc4_write_tx_entry(QTestState *qts, unsigned port,
                                unsigned ring_size, uint32_t offset,
                                const uint8_t data[4],
                                const uint8_t control[4])
{
    uint64_t ring = ioc4_tx_ring_offset(port, ring_size) + offset;

    ioc4_dma_write(qts, ring, data, 4);
    ioc4_dma_write(qts, ring + 4, control, 4);
}

static void ioc4_fill_loopback_tx_ring(QTestState *qts,
                                       uint8_t expected_rx[0x3f8])
{
    unsigned i, j;

    for (i = 0; i < 127; i++) {
        uint8_t data[4];
        const uint8_t control[4] = {
            TX_VALID, TX_VALID, TX_VALID, TX_VALID,
        };

        for (j = 0; j < 4; j++) {
            data[j] = (i * 4 + j) & 0xff;
            expected_rx[i * 8 + j] = data[j];
            expected_rx[i * 8 + 4 + j] = RX_LOOPBACK_VALID;
        }
        ioc4_write_tx_entry(qts, 0, 0x400, i * 8, data, control);
    }
    reg_write(qts, SDMA_TX_PRODUCE(0), 0x3f8);
}

static void ioc4_expect_bytes(QTestState *qts, int fd,
                              const uint8_t *expected, size_t size)
{
    size_t received = 0;

    while (received < size) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        uint8_t bytes[32];
        ssize_t n;

        g_assert_cmpint(poll(&pfd, 1, 1000), ==, 1);
        n = read(fd, bytes, MIN(sizeof(bytes), size - received));
        g_assert_cmpint(n, >, 0);
        if (memcmp(bytes, expected + received, n)) {
            char actual_hex[sizeof(bytes) * 3 + 1] = { 0 };
            char expected_hex[sizeof(bytes) * 3 + 1] = { 0 };
            size_t j;
            size_t actual_len = 0;
            size_t expected_len = 0;

            for (j = 0; j < (size_t)n; j++) {
                actual_len += g_snprintf(actual_hex + actual_len,
                                         sizeof(actual_hex) - actual_len,
                                         "%s%02x", j ? " " : "", bytes[j]);
                expected_len += g_snprintf(expected_hex + expected_len,
                                           sizeof(expected_hex) - expected_len,
                                           "%s%02x", j ? " " : "",
                                           expected[received + j]);
            }
            g_printerr("UART output mismatch at byte %zu: "
                       "received [%s], expected [%s]; "
                       "port 0 control=0x%08x TX producer=0x%08x "
                       "TX consumer=0x%08x DMA error low=0x%08x "
                       "high=0x%08x other pending=0x%08x\n",
                       received, actual_hex, expected_hex,
                       reg_read(qts, SDMA_CONTROL(0)),
                       reg_read(qts, SDMA_TX_PRODUCE(0)),
                       reg_read(qts, SDMA_TX_CONSUME(0)),
                       reg_read(qts, 0x00), reg_read(qts, 0x04),
                       reg_read(qts, 0x0c));
        }
        g_assert_cmpmem(bytes, n, expected + received, n);
        received += n;
    }
}

static void ioc4_wait_reg(QTestState *qts, unsigned offset, uint32_t mask,
                          uint32_t expected)
{
    unsigned i;

    for (i = 0; i < 1000; i++) {
        if ((reg_read(qts, offset) & mask) == expected) {
            return;
        }
        g_usleep(1000);
    }
    g_test_message("timeout at IOC4 BAR + 0x%x: value=0x%08x mask=0x%08x "
                   "expected=0x%08x; control=0x%08x TX producer=0x%08x "
                   "TX consumer=0x%08x pending=0x%08x shadow=0x%08x",
                   offset, reg_read(qts, offset), mask, expected,
                   reg_read(qts, SDMA_CONTROL(0)),
                   reg_read(qts, SDMA_TX_PRODUCE(0)),
                   reg_read(qts, SDMA_TX_CONSUME(0)),
                   reg_read(qts, 0x08), reg_read(qts, SDMA_SHADOW(0)));
    g_assert_cmphex(reg_read(qts, offset) & mask, ==, expected);
}

static void ioc4_wait_reg_with_log(QTestState *qts, unsigned offset,
                                   uint32_t mask, uint32_t expected,
                                   const char *log)
{
    g_autofree char *contents = NULL;
    unsigned i;

    for (i = 0; i < 1000; i++) {
        if ((reg_read(qts, offset) & mask) == expected) {
            return;
        }
        g_usleep(1000);
    }
    contents = ioc4_log_file_take(log);
    g_test_message("QEMU device log before timeout: %s", contents);
    g_assert_cmphex(reg_read(qts, offset) & mask, ==, expected);
}

static void ioc4_wait_rx_produce(QTestState *qts, uint32_t expected,
                                const char *log)
{
    g_autofree char *contents = NULL;
    gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    uint32_t produce;

    while (g_get_monotonic_time() < deadline) {
        produce = reg_read(qts, SDMA_RX_PRODUCE(0));
        if (produce == expected) {
            return;
        }
        qtest_clock_step(qts, 62500);
        g_usleep(1000);
    }
    produce = reg_read(qts, SDMA_RX_PRODUCE(0));
    if (produce == expected) {
        return;
    }

    g_assert_true(g_file_get_contents(log, &contents, NULL, NULL));
    g_test_message("RX diagnostic log: %s", contents);
    g_assert_cmphex(produce, ==, expected);
}

/*
 * Run the virtual clock a millisecond at a time until a register reads a
 * value. In loopback each character spends a character time on the line,
 * 1.04 ms at the 16550's reset rate, so a ring takes virtual time to cross.
 */
static void ioc4_run_line_until(QTestState *qts, unsigned offset,
                                uint32_t mask, uint32_t expected)
{
    unsigned i;

    for (i = 0; i < 2000 && (reg_read(qts, offset) & mask) != expected; i++) {
        qtest_clock_step(qts, 1000000);
    }
    g_assert_cmphex(reg_read(qts, offset) & mask, ==, expected);
}

static void ioc4_socketpair(int sv[2])
{
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
}

static void test_dma_property_defaults_off(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t data[4] = { 0x51, 0, 0, 0 };
    const uint8_t control[4] = { TX_VALID, 0, 0, 0 };
    struct pollfd pfd;
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, false, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, data, control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);

    pfd = (struct pollfd) { .fd = sv[0], .events = POLLIN };
    g_assert_cmpint(poll(&pfd, 1, 50), ==, 0);
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(0)), ==, 0);
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_tx_direct64_swap_on(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t data[4] = { 0x41, 0x42, 0x43, 0x44 };
    const uint8_t control[4] = { TX_VALID, TX_VALID, 0, 0 };
    const uint8_t expected[2] = { 0x41, 0x42 };
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    g_assert_cmphex(reg_read(qts, 0x08) & TX_RING_EMPTY(0), ==, 0);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, data, control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);

    ioc4_expect_bytes(qts, sv[0], expected, sizeof(expected));
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(0), 0xff8, 8);
    ioc4_wait_reg(qts, 0x08, TX_RING_EMPTY(0), TX_RING_EMPTY(0));
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_tx_direct64_swap_off(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t data[4] = { 0x41, 0x42, 0x43, 0x44 };
    /*
     * Every slot valid: uncorrected, each word's bytes arrive reversed, and
     * an empty slot reversed to the front would end the entry at once.
     */
    const uint8_t control[4] = { TX_VALID, TX_VALID, TX_VALID, TX_VALID };
    const uint8_t expected[4] = { 0x44, 0x43, 0x42, 0x41 };
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base(qts, 0, DMA_PAIR_BASE & ~DMA_SWAP_BIT, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, data, control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);

    ioc4_expect_bytes(qts, sv[0], expected, sizeof(expected));
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(0), 0xff8, 8);
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_tx_requires_bus_master(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t data[4] = { 0x41, 0x42, 0x43, 0x44 };
    const uint8_t control[4] = { TX_VALID, TX_VALID, 0, 0 };
    const uint8_t expected_ring[8] = {
        0x41, 0x42, 0x43, 0x44, TX_VALID, TX_VALID, 0, 0,
    };
    uint8_t ring[sizeof(expected_ring)];
    struct pollfd pfd;
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    qtest_writel(qts, CFG + 0x04, 0x00000002);
    g_assert_cmphex(qtest_readl(qts, CFG + 0x04) & PCI_COMMAND_MASTER,
                    ==, 0);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, data, control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);

    ioc4_dma_read(qts, ioc4_tx_ring_offset(0, 0x1000), ring,
                  sizeof(ring));
    g_assert_cmpmem(ring, sizeof(ring), expected_ring, sizeof(expected_ring));
    g_assert_cmphex(reg_read(qts, SDMA_TX_PRODUCE(0)), ==, 8);
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(0)), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x00) & ERROR_VALID, ==, ERROR_VALID);
    g_assert_cmphex(reg_read(qts, 0x0c) & DMA_ERROR(0), ==, DMA_ERROR(0));
    pfd = (struct pollfd) { .fd = sv[0], .events = POLLIN };
    g_assert_cmpint(poll(&pfd, 1, 50), ==, 0);
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_tx_control_entries(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t data[4] = { 0x1f, 3, 0, 0 };
    const uint8_t control[4] = { TX_MCR | TX_EXPLICIT,
                                  TX_DELAY | TX_EXPLICIT, 0, 0 };
    int64_t paused_clock;
    int64_t resumed_clock;
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, data, control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    ioc4_wait_reg(qts, SDMA_SHADOW(0), 0x1f000000, 0x1f000000);
    ioc4_wait_reg(qts, 0x08, TX_COMPLETION(0), TX_COMPLETION(0));
    reg_write(qts, 0x08, TX_COMPLETION(0));
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(0)), ==, 0);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_PAUSE);
    g_test_message("paused TX: control=0x%08x producer=0x%08x "
                   "consumer=0x%08x pending=0x%08x",
                   reg_read(qts, SDMA_CONTROL(0)),
                   reg_read(qts, SDMA_TX_PRODUCE(0)),
                   reg_read(qts, SDMA_TX_CONSUME(0)), reg_read(qts, 0x08));
    g_assert_cmphex(reg_read(qts, SDMA_CONTROL(0)) & SSCR_PAUSE_STATE,
                    ==, SSCR_PAUSE_STATE);
    paused_clock = qtest_clock_step(qts, 5 * 1000000);
    g_test_message("virtual clock after paused interval: %" PRId64,
                   paused_clock);
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(0)), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x08) & TX_COMPLETION(0), ==, 0);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    g_test_message("resumed TX: control=0x%08x producer=0x%08x "
                   "consumer=0x%08x pending=0x%08x",
                   reg_read(qts, SDMA_CONTROL(0)),
                   reg_read(qts, SDMA_TX_PRODUCE(0)),
                   reg_read(qts, SDMA_TX_CONSUME(0)), reg_read(qts, 0x08));
    resumed_clock = qtest_clock_step(qts, 3 * 1000000);
    g_test_message("virtual clock after resumed interval: %" PRId64,
                   resumed_clock);
    g_assert_cmpint(resumed_clock - paused_clock, ==, 3 * 1000000);
    ioc4_wait_reg_with_log(qts, SDMA_TX_CONSUME(0), 0xff8, 8, log);
    ioc4_wait_reg(qts, 0x08, TX_COMPLETION(0), TX_COMPLETION(0));
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_tx_pause_reject_preserves_delay(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t data[4] = { 3, 0, 0, 0 };
    const uint8_t control[4] = { TX_DELAY | TX_EXPLICIT, 0, 0, 0 };
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, data, control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(0)), ==, 0);

    reg_write(qts, SDMA_CONTROL(0), SSCR_PAUSE);
    g_assert_cmphex(reg_read(qts, SDMA_CONTROL(0)) &
                    (SSCR_DMA_EN | SSCR_PAUSE | SSCR_PAUSE_STATE),
                    ==, SSCR_DMA_EN);
    qtest_clock_step(qts, 2999999);
    g_assert_cmphex(reg_read(qts, 0x08) & TX_COMPLETION(0), ==, 0);
    qtest_clock_step(qts, 1);
    ioc4_wait_reg(qts, 0x08, TX_COMPLETION(0), TX_COMPLETION(0));
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(0), 0xff8, 8);
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_rx_drain_and_threshold(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t input[] = { 0x61, 0x62, 0x63 };
    uint8_t entry[8];
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_THRESHOLD(1));
    /* A long reload, so the drain and not the timer posts the entry. */
    reg_write(qts, SDMA_RX_TIMER(0), 0xfff);
    reg_write(qts, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    g_assert_cmpint(write(sv[0], input, sizeof(input)), ==,
                    (ssize_t)sizeof(input));
    g_usleep(10000);
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 0);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_THRESHOLD(1) |
                                    SSCR_DRAIN);
    ioc4_wait_reg(qts, SDMA_RX_PRODUCE(0), 0xff8, 8);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x1000), entry,
                  sizeof(entry));
    g_assert_cmpmem(entry, 3, input, sizeof(input));
    g_assert_cmphex(entry[3], ==, 0);
    g_assert_cmphex(entry[4], ==, RX_DATA_VALID);
    g_assert_cmphex(entry[5], ==, RX_DATA_VALID);
    g_assert_cmphex(entry[6], ==, RX_DATA_VALID);
    g_assert_cmphex(entry[7], ==, 0);
    ioc4_wait_reg(qts, 0x08, RX_THRESHOLD(0), RX_THRESHOLD(0));
    reg_write(qts, 0x08, RX_THRESHOLD(0));
    g_assert_cmphex(reg_read(qts, 0x08) & RX_THRESHOLD(0), ==,
                    RX_THRESHOLD(0));
    reg_write(qts, SDMA_RX_CONSUME(0), 8);
    reg_write(qts, 0x08, RX_THRESHOLD(0));
    g_assert_cmphex(reg_read(qts, 0x08) & RX_THRESHOLD(0), ==, 0);
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_loopback_pio_mcr(void)
{
    g_autofree char *control_log = ioc4_log_file_new();
    g_autofree char *loopback_log = ioc4_log_file_new();
    int control_sv[2], loopback_sv[2];
    QTestState *qts;
    const uint8_t control_data[4] = { 0x51, 0x52, 0x53, 0x54 };
    const uint8_t valid[4] = { TX_VALID, TX_VALID, TX_VALID, TX_VALID };
    const uint8_t expected_control[4] = { 0x51, 0x52, 0x53, 0x54 };
    uint8_t rx_ring[0x3f8], expected_ring[0x3f8];
    uint8_t prom_rx[8], last_rx[8];
    struct pollfd pfd;
    g_autofree char *contents = NULL;

    ioc4_socketpair(control_sv);
    qts = ioc4_start_dma(false, true, 0, control_sv[1], control_log);
    close(control_sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x400);
    qtest_writeb(qts, UART(0) + 4, 0);
    ioc4_write_tx_entry(qts, 0, 0x400, 0, control_data, valid);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_THRESHOLD(1));
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    ioc4_expect_bytes(qts, control_sv[0], expected_control,
                      sizeof(expected_control));
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(0), 0x3f8, 8);
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 0);
    qtest_quit(qts);
    close(control_sv[0]);
    contents = ioc4_log_file_take(control_log);
    g_assert_nonnull(contents);
    g_clear_pointer(&contents, g_free);

    ioc4_socketpair(loopback_sv);
    qts = ioc4_start_dma(false, true, 0, loopback_sv[1], loopback_log);
    close(loopback_sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x400);
    qtest_writeb(qts, UART(0) + 4, 0x13);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 4), ==, 0x13);
    g_assert_cmphex(reg_read(qts, SDMA_SHADOW(0)) & 0x1f000000, ==,
                    0x13000000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_THRESHOLD(1));
    ioc4_fill_loopback_tx_ring(qts, expected_ring);
    ioc4_run_line_until(qts, SDMA_TX_CONSUME(0), 0x3f8, 0x3f8);
    ioc4_run_line_until(qts, SDMA_RX_PRODUCE(0), 0xff8, 0x3f8);
    g_assert_cmphex(reg_read(qts, SDMA_TX_PRODUCE(0)), ==, 0x3f8);
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(0)), ==, 0x3f8);
    g_assert_cmphex(reg_read(qts, SDMA_RX_CONSUME(0)), ==, 0);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x400), rx_ring,
                  sizeof(rx_ring));
    g_assert_cmpmem(rx_ring, sizeof(rx_ring), expected_ring,
                    sizeof(expected_ring));
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x400) + 0x3e8,
                  prom_rx, sizeof(prom_rx));
    g_assert_cmpmem(prom_rx, sizeof(prom_rx),
                    ((const uint8_t[]) {
                        0xf4, 0xf5, 0xf6, 0xf7,
                        RX_LOOPBACK_VALID, RX_LOOPBACK_VALID,
                        RX_LOOPBACK_VALID, RX_LOOPBACK_VALID,
                    }), sizeof(prom_rx));
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x400) + 0x3f0,
                  last_rx, sizeof(last_rx));
    g_assert_cmpmem(last_rx, sizeof(last_rx),
                    ((const uint8_t[]) {
                        0xf8, 0xf9, 0xfa, 0xfb,
                        RX_LOOPBACK_VALID, RX_LOOPBACK_VALID,
                        RX_LOOPBACK_VALID, RX_LOOPBACK_VALID,
                    }), sizeof(last_rx));
    pfd = (struct pollfd) { .fd = loopback_sv[0], .events = POLLIN };
    g_assert_cmpint(poll(&pfd, 1, 50), ==, 0);
    qtest_quit(qts);
    close(loopback_sv[0]);
    contents = ioc4_log_file_take(loopback_log);
    g_assert_nonnull(contents);
}

static void test_dma_loopback_tx_mcr(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    uint8_t data[4], control[4];
    uint8_t rx_ring[0x3f8], expected_ring[0x3f8];
    struct pollfd pfd;
    unsigned i, j, rx_count = 0;
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x400);
    qtest_writeb(qts, UART(0) + 4, 0);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 4), ==, 0);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_THRESHOLD(1));
    memset(expected_ring, 0, sizeof(expected_ring));
    for (i = 0; i < 127; i++) {
        if (i == 0) {
            data[0] = 0x13;
            data[1] = 0;
            data[2] = 1;
            data[3] = 2;
            control[0] = TX_MCR;
            control[1] = TX_VALID;
            control[2] = TX_VALID;
            control[3] = TX_VALID;
            for (j = 1; j < 4; j++) {
                expected_ring[(rx_count / 4) * 8 + rx_count % 4] = data[j];
                expected_ring[(rx_count / 4) * 8 + 4 + rx_count % 4] =
                    RX_LOOPBACK_VALID;
                rx_count++;
            }
        } else {
            for (j = 0; j < 4; j++) {
                data[j] = (i * 4 + j - 1) & 0xff;
                control[j] = TX_VALID;
                expected_ring[(rx_count / 4) * 8 + rx_count % 4] = data[j];
                expected_ring[(rx_count / 4) * 8 + 4 + rx_count % 4] =
                    RX_LOOPBACK_VALID;
                rx_count++;
            }
        }
        ioc4_write_tx_entry(qts, 0, 0x400, i * 8, data, control);
    }
    g_assert_cmpuint(rx_count, ==, 507);
    reg_write(qts, SDMA_TX_PRODUCE(0), 0x3f8);
    ioc4_run_line_until(qts, SDMA_TX_CONSUME(0), 0x3f8, 0x3f8);
    /* The last character is still on the line when its slot is consumed. */
    qtest_clock_step(qts, 2000000);
    ioc4_run_line_until(qts, SDMA_RX_PRODUCE(0), 0xff8, 0x3f0);
    /* The modem-control entry wrote the UART's own register. */
    g_assert_cmphex(qtest_readb(qts, UART(0) + 4), ==, 0x13);
    g_assert_cmphex(reg_read(qts, SDMA_SHADOW(0)) & SDMA_SHADOW_OUTPUTS,
                    ==, 0x13000000);
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 0x3f0);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_THRESHOLD(1) |
                                    SSCR_DRAIN);
    ioc4_wait_rx_produce(qts, 0x3f8, log);
    g_assert_cmphex(reg_read(qts, SDMA_TX_PRODUCE(0)), ==, 0x3f8);
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(0)), ==, 0x3f8);
    g_assert_cmphex(reg_read(qts, SDMA_RX_CONSUME(0)), ==, 0);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x400), rx_ring,
                  sizeof(rx_ring));
    g_assert_cmpmem(rx_ring, sizeof(rx_ring), expected_ring,
                    sizeof(expected_ring));
    pfd = (struct pollfd) { .fd = sv[0], .events = POLLIN };
    g_assert_cmpint(poll(&pfd, 1, 50), ==, 0);
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_loopback_shadow_reset(void)
{
    g_autofree char *log = ioc4_log_file_new();
    QTestState *qts = ioc4_start_dma(false, true, 0, -1, log);
    g_autofree char *contents = NULL;

    qtest_writeb(qts, UART(0) + 4, 0);
    reg_write(qts, SDMA_SHADOW(0), SDMA_SHADOW_OUTPUTS);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 4), ==, 0);
    qtest_writeb(qts, UART(0) + 4, 0x13);
    reg_write(qts, SDMA_SHADOW(0), 0x13000000);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 4), ==, 0x13);

    /* The channel reset leaves the outputs, a setting, as they were. */
    reg_write(qts, SDMA_CONTROL(0), SSCR_RESET);
    g_assert_cmphex(reg_read(qts, SDMA_SHADOW(0)) & SDMA_SHADOW_OUTPUTS,
                    ==, 0x13000000);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 4), ==, 0x13);
    qtest_quit(qts);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

/*
 * In loopback the UART's serial input is disconnected, as on a PC16550D:
 * input from outside is discarded on arrival and logged, not kept for later.
 */
static void test_dma_loopback_discards_external_input(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t input[4] = { 0xa1, 0xa2, 0xa3, 0xa4 };
    uint8_t entry[8];
    unsigned i;
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x400);
    qtest_writeb(qts, UART(0) + 4, 0x10);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_THRESHOLD(1));
    g_assert_cmpint(write(sv[0], input, sizeof(input)), ==, sizeof(input));
    g_usleep(20000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_THRESHOLD(1) |
                                    SSCR_DRAIN);
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 0);

    /* Out of loopback nothing comes back: it was discarded, not held. */
    qtest_writeb(qts, UART(0) + 4, 0);
    g_usleep(20000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_THRESHOLD(1) |
                                    SSCR_DRAIN);
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 0);

    /* New input now arrives as usual. */
    g_assert_cmpint(write(sv[0], input, sizeof(input)), ==, sizeof(input));
    for (i = 0; i < 1000 && reg_read(qts, SDMA_RX_PRODUCE(0)) != 8; i++) {
        g_usleep(1000);
    }
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 8);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x400), entry,
                  sizeof(entry));
    g_assert_cmpmem(entry, sizeof(input), input, sizeof(input));
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(strstr(contents, "while in loopback"));
}

static void test_dma_loopback_full_ring_recovery(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t first_data[4] = { 0xa1, 0xa2, 0xa3, 0xa4 };
    const uint8_t second_data[4] = { 0xb1, 0xb2, 0xb3, 0xb4 };
    const uint8_t valid[4] = { TX_VALID, TX_VALID, TX_VALID, TX_VALID };
    const uint8_t first_rx[8] = {
        0xa1, 0xa2, 0xa3, 0xa4,
        RX_LOOPBACK_VALID, RX_LOOPBACK_VALID,
        RX_LOOPBACK_VALID, RX_LOOPBACK_VALID,
    };
    const uint8_t second_rx[8] = {
        0xb1, 0xb2, 0xb3, 0xb4,
        RX_LOOPBACK_VALID, RX_LOOPBACK_VALID,
        RX_LOOPBACK_VALID, RX_LOOPBACK_VALID,
    };
    uint8_t rx_ring[0x3f8], expected_ring[0x3f8], entry[8];
    struct pollfd pfd;
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x400);
    /* The 16550's FIFO on, to hold what the full ring cannot take. */
    qtest_writeb(qts, UART(0) + 2, 0x01);
    qtest_writeb(qts, UART(0) + 4, 0x10);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_THRESHOLD(1));
    ioc4_fill_loopback_tx_ring(qts, expected_ring);
    ioc4_run_line_until(qts, SDMA_TX_CONSUME(0), 0x3f8, 0x3f8);
    ioc4_run_line_until(qts, SDMA_RX_PRODUCE(0), 0xff8, 0x3f8);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x400), rx_ring,
                  sizeof(rx_ring));
    g_assert_cmpmem(rx_ring, sizeof(rx_ring), expected_ring,
                    sizeof(expected_ring));

    ioc4_write_tx_entry(qts, 0, 0x400, 0x3f8, first_data, valid);
    ioc4_write_tx_entry(qts, 0, 0x400, 0, second_data, valid);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    /*
     * The full receive ring does not stall the transmit walk: both entries
     * go out, and the eight characters wait in the packer and the 16550's
     * FIFO, which holds them without overrunning.
     */
    ioc4_run_line_until(qts, SDMA_TX_CONSUME(0), 0xff8, 8);
    qtest_clock_step(qts, 2000000);
    g_assert_cmphex(reg_read(qts, 0x08) & RX_RING_FULL(0), ==,
                    RX_RING_FULL(0));
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 0x3f8);

    reg_write(qts, SDMA_RX_CONSUME(0), 8);
    ioc4_wait_rx_produce(qts, 0, log);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x400) + 0x3f8,
                  entry, sizeof(entry));
    g_assert_cmpmem(entry, sizeof(entry), first_rx, sizeof(first_rx));
    g_assert_cmphex(reg_read(qts, SDMA_RX_CONSUME(0)), ==, 8);

    reg_write(qts, SDMA_RX_CONSUME(0), 16);
    ioc4_wait_rx_produce(qts, 8, log);
    /* Filled exactly, with nothing left waiting: the source falls. */
    g_assert_cmphex(reg_read(qts, 0x08) & RX_RING_FULL(0), ==, 0);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x400), entry,
                  sizeof(entry));
    g_assert_cmpmem(entry, sizeof(entry), second_rx, sizeof(second_rx));
    g_assert_cmphex(reg_read(qts, SDMA_RX_CONSUME(0)), ==, 16);
    pfd = (struct pollfd) { .fd = sv[0], .events = POLLIN };
    g_assert_cmpint(poll(&pfd, 1, 50), ==, 0);
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_tx_wrap_1k(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t data[4] = { 0x5e, 0, 0, 0 };
    const uint8_t control[4] = { TX_VALID, 0, 0, 0 };
    const uint8_t expected = data[0];
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x400);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    reg_write(qts, SDMA_TX_PRODUCE(0), 0x3f8);
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(0), 0x3f8, 0x3f8);
    ioc4_write_tx_entry(qts, 0, 0x400, 0x3f8, data, control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 0);
    ioc4_expect_bytes(qts, sv[0], &expected, sizeof(expected));
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(0), 0x3f8, 0);
    ioc4_wait_reg(qts, 0x08, TX_RING_EMPTY(0), TX_RING_EMPTY(0));
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_tx_wrap_4k(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t data[4] = { 0x6e, 0, 0, 0 };
    const uint8_t control[4] = { TX_VALID, 0, 0, 0 };
    const uint8_t expected = data[0];
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    reg_write(qts, SDMA_TX_PRODUCE(0), 0xff8);
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(0), 0xff8, 0xff8);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0xff8, data, control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 0);
    ioc4_expect_bytes(qts, sv[0], &expected, sizeof(expected));
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(0), 0xff8, 0);
    ioc4_wait_reg(qts, 0x08, TX_RING_EMPTY(0), TX_RING_EMPTY(0));
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_rx_full_and_staged_recovery(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t fill[4] = { 0x31, 0x32, 0x33, 0x34 };
    const uint8_t incoming[5] = { 0xa1, 0xa2, 0xa3, 0xa4, 0xb5 };
    uint8_t entry[8];
    unsigned i;
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x400);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    for (i = 0; i < 127; i++) {
        g_assert_cmpint(write(sv[0], fill, sizeof(fill)), ==, sizeof(fill));
        ioc4_wait_reg(qts, SDMA_RX_PRODUCE(0), 0x3f8,
                      ((i + 1) * 8) & 0x3f8);
    }
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 0x3f8);

    reg_write(qts, SDMA_RX_TIMER(0), 1);
    reg_write(qts, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    g_assert_cmpint(write(sv[0], incoming, sizeof(incoming)), ==,
                    sizeof(incoming));
    ioc4_wait_reg(qts, 0x08, RX_RING_FULL(0), RX_RING_FULL(0));
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 0x3f8);
    /* Ring-full is a level: it stays while the entry waits for room. */
    reg_write(qts, 0x08, RX_RING_FULL(0));
    g_assert_cmphex(reg_read(qts, 0x08) & RX_RING_FULL(0), ==,
                    RX_RING_FULL(0));

    reg_write(qts, SDMA_RX_CONSUME(0), 16 | SRCIR_TIMER_ARM);
    ioc4_wait_reg(qts, SDMA_RX_PRODUCE(0), 0x3f8, 0);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x400) + 0x3f8,
                  entry, sizeof(entry));
    g_assert_cmpmem(entry, 4, incoming, 4);
    g_assert_cmphex(entry[4], ==, RX_DATA_VALID);
    g_assert_cmphex(entry[7], ==, RX_DATA_VALID);
    g_assert_cmphex(reg_read(qts, 0x08) & RX_RING_FULL(0), ==, 0);

    ioc4_wait_rx_produce(qts, 8, log);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x400), entry,
                  sizeof(entry));
    g_assert_cmphex(entry[0], ==, incoming[4]);
    g_assert_cmphex(entry[1], ==, 0);
    g_assert_cmphex(entry[4], ==, RX_DATA_VALID);
    g_assert_cmphex(entry[5], ==, 0);
    /* Full again, but nothing waits for room: the source stays down. */
    g_assert_cmphex(reg_read(qts, 0x08) & RX_RING_FULL(0), ==, 0);

    reg_write(qts, SDMA_RX_TIMER(0), 1);
    reg_write(qts, SDMA_RX_CONSUME(0), 16 | SRCIR_TIMER_ARM);
    qtest_clock_step(qts, 62500);
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 8);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_DRAIN);
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 8);
    g_assert_cmphex(reg_read(qts, SDMA_CONTROL(0)) & SSCR_DRAIN, ==, 0);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x400), entry,
                  sizeof(entry));
    g_assert_cmphex(entry[0], ==, incoming[4]);
    g_assert_cmphex(entry[1], ==, 0);
    g_assert_cmphex(entry[4], ==, RX_DATA_VALID);
    g_assert_cmphex(entry[5], ==, 0);
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_rx_timer_expiry(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t input = 0x6d;
    uint8_t entry[8];
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_THRESHOLD(1));
    reg_write(qts, SDMA_RX_TIMER(0), 2);
    reg_write(qts, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    g_assert_cmpint(write(sv[0], &input, 1), ==, 1);
    g_usleep(10000);
    qtest_clock_step(qts, 124999);
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 0);
    qtest_clock_step(qts, 1);
    ioc4_wait_reg(qts, SDMA_RX_PRODUCE(0), 0xff8, 8);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x1000), entry,
                  sizeof(entry));
    g_assert_cmphex(entry[0], ==, input);
    g_assert_cmphex(entry[4], ==, RX_DATA_VALID);
    g_assert_cmphex(reg_read(qts, 0x08) & RX_TIMER_EXPIRED(0), ==,
                    RX_TIMER_EXPIRED(0));
    g_assert_cmphex(reg_read(qts, 0x08) & RX_THRESHOLD(0), ==,
                    RX_THRESHOLD(0));
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

/*
 * An expiry already due is applied before a register access, whether or not
 * the main loop has run the timer yet. Stopping the VM keeps timer callbacks
 * from running while the qtest clock still moves.
 */
static void test_dma_rx_timer_due_before_access(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t input = 0x64;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    reg_write(qts, SDMA_RX_TIMER(0), 2);
    reg_write(qts, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    g_assert_cmpint(write(sv[0], &input, 1), ==, 1);
    g_usleep(10000);
    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");
    qtest_clock_step(qts, 125001);
    g_assert_cmphex(reg_read(qts, 0x08) & RX_TIMER_EXPIRED(0), ==,
                    RX_TIMER_EXPIRED(0));
    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The receive timer register reads its live count in bits 16-27. A reload
 * write loads the count too, and an idle timer holds it; a running timer
 * counts down from its reload. The IOC3 board test checks the same fields.
 */
static void test_dma_rx_timer_count_reads_back(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t input = 0x63;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_RX_TIMER(0), 0xfff);
    g_assert_cmphex(reg_read(qts, SDMA_RX_TIMER(0)) & 0x0fff0fff, ==,
                    0x0fff0fff);
    qtest_clock_step(qts, 0x10 * 62500);
    g_assert_cmphex(reg_read(qts, SDMA_RX_TIMER(0)) & 0x0fff0fff, ==,
                    0x0fff0fff);

    reg_write(qts, SDMA_RX_TIMER(0), 0x140);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    reg_write(qts, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    g_assert_cmpint(write(sv[0], &input, 1), ==, 1);
    g_usleep(10000);
    qtest_clock_step(qts, 0x40 * 62500);
    g_assert_cmphex((reg_read(qts, SDMA_RX_TIMER(0)) >> 16) & 0xfff, ==,
                    0x100);
    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The control register keeps every writable bit as written, including
 * controls the model does not act on, such as hardware flow control and
 * bit 20; status and command bits do not store.
 */
static void test_dma_control_stores_its_bits(void)
{
    g_autofree char *log = ioc4_log_file_new();
    QTestState *qts = ioc4_start_dma(false, true, 0, -1, log);
    const uint32_t written = (1u << 17) | (1u << 20) | SSCR_THRESHOLD(0x1ff);

    reg_write(qts, SDMA_CONTROL(0), written);
    g_assert_cmphex(reg_read(qts, SDMA_CONTROL(0)), ==, written);
    qtest_quit(qts);
}

/*
 * The two indices the engine advances still take a write of their pointer
 * field: the IOC3 board test writes and reads back the transmit consume
 * index, and IOC4 has the same register.
 */
static void test_dma_engine_indices_take_writes(void)
{
    g_autofree char *log = ioc4_log_file_new();
    QTestState *qts = ioc4_start_dma(false, true, 0, -1, log);

    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_TX_CONSUME(0), 0xffffffff);
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(0)), ==, 0xff8);
    reg_write(qts, SDMA_RX_PRODUCE(0), 0x18);
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 0x18);
    qtest_quit(qts);
}

/*
 * A host-owned index written with bits outside its pointer field keeps the
 * pointer field and logs the rest; the port does not stop.
 */
static void test_dma_host_index_masks_its_pointer(void)
{
    g_autofree char *log = ioc4_log_file_new();
    QTestState *qts = ioc4_start_dma(false, true, 0, -1, log);
    g_autofree char *contents = NULL;

    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_TX_PRODUCE(0), 0x10014);
    g_assert_cmphex(reg_read(qts, SDMA_TX_PRODUCE(0)), ==, 0x10);
    reg_write(qts, SDMA_RX_CONSUME(0), 0x2c);
    g_assert_cmphex(reg_read(qts, SDMA_RX_CONSUME(0)) & 0xff8, ==, 0x28);
    qtest_quit(qts);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(strstr(contents, "outside the pointer field"));
}

/*
 * The receive threshold source is a level: it follows whether the ring holds
 * the mark's worth of entries, a write cannot clear it, and it falls when
 * software consumes below the mark. The IOC3 model is measured against
 * IRIX's ring console, which never acknowledges it.
 */
static void test_dma_threshold_is_a_level(void)
{
    g_autofree char *log = ioc4_log_file_new();
    QTestState *qts = ioc4_start_dma(false, true, 0, -1, log);

    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_THRESHOLD(1));
    reg_write(qts, SDMA_RX_PRODUCE(0), 8);
    g_assert_cmphex(reg_read(qts, 0x08) & RX_THRESHOLD(0), ==,
                    RX_THRESHOLD(0));
    reg_write(qts, 0x08, RX_THRESHOLD(0));
    g_assert_cmphex(reg_read(qts, 0x08) & RX_THRESHOLD(0), ==,
                    RX_THRESHOLD(0));
    reg_write(qts, SDMA_RX_CONSUME(0), 8);
    g_assert_cmphex(reg_read(qts, 0x08) & RX_THRESHOLD(0), ==, 0);
    qtest_quit(qts);
}

/*
 * An empty transmit slot ends its entry wherever it sits: the slots behind
 * it are not sent. The IOC3 board test's expected data says so of IOC3;
 * IOC4 takes it from its sibling.
 */
static void test_dma_empty_slot_ends_the_entry(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t first[4] = { 'a', 'b', 'c', 'd' };
    const uint8_t first_control[4] = { TX_VALID, 0, TX_VALID, TX_VALID };
    const uint8_t second[4] = { 'e', 0, 0, 0 };
    const uint8_t second_control[4] = { TX_VALID, 0, 0, 0 };
    const uint8_t expected[2] = { 'a', 'e' };

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, first, first_control);
    ioc4_write_tx_entry(qts, 0, 0x1000, 8, second, second_control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 16);
    ioc4_expect_bytes(qts, sv[0], expected, sizeof(expected));
    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A control byte is decoded by its type field and bit 5; undefined low bits
 * are logged and do not stop the port. A slot of type none ends the entry
 * and raises nothing, even with bit 5 set. No source settles either case.
 */
static void test_dma_tx_control_decodes_type_and_bit5(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t first[4] = { 'a', 'b', 'c', 0 };
    const uint8_t first_control[4] = { TX_VALID | 0x10, TX_EXPLICIT,
                                        TX_VALID, 0 };
    const uint8_t second[4] = { 'd', 0, 0, 0 };
    const uint8_t second_control[4] = { TX_VALID, 0, 0, 0 };
    const uint8_t expected[2] = { 'a', 'd' };
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, first, first_control);
    ioc4_write_tx_entry(qts, 0, 0x1000, 8, second, second_control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 16);
    ioc4_expect_bytes(qts, sv[0], expected, sizeof(expected));
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(0), 0xff8, 16);
    g_assert_cmphex(reg_read(qts, 0x08) & TX_COMPLETION(0), ==, 0);
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(strstr(contents, "undefined TX control bits"));
}

/*
 * Control bit 16, "TX timer in progress" in both Linux headers, reads set
 * while a transmit delay entry is running and clear once it has ended.
 */
static void test_dma_tx_delay_sets_the_busy_bit(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t data[4] = { 3, 0, 0, 0 };
    const uint8_t control[4] = { TX_DELAY, 0, 0, 0 };

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, data, control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    g_assert_cmphex(reg_read(qts, SDMA_CONTROL(0)) & (1u << 16), ==,
                    1u << 16);
    qtest_clock_step(qts, 3 * 1000000);
    g_assert_cmphex(reg_read(qts, SDMA_CONTROL(0)) & (1u << 16), ==, 0);
    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The channel reset returns the indices and the engine's work in progress
 * to the start and leaves the settings: the timer reload, the shadow's
 * outputs and the control bits written with it, as on IOC3.
 */
static void test_dma_channel_reset_keeps_settings(void)
{
    g_autofree char *log = ioc4_log_file_new();
    QTestState *qts = ioc4_start_dma(false, true, 0, -1, log);

    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_RX_TIMER(0), 0x140);
    qtest_writeb(qts, UART(0) + 4, 0x03);
    reg_write(qts, SDMA_TX_PRODUCE(0), 0x40);
    reg_write(qts, SDMA_CONTROL(0), SSCR_RESET | SSCR_THRESHOLD(5));
    g_assert_cmphex(reg_read(qts, SDMA_TX_PRODUCE(0)), ==, 0);
    g_assert_cmphex(reg_read(qts, SDMA_RX_TIMER(0)) & 0xfff, ==, 0x140);
    g_assert_cmphex(reg_read(qts, SDMA_SHADOW(0)) & SDMA_SHADOW_OUTPUTS, ==,
                    0x03000000);
    g_assert_cmphex(reg_read(qts, SDMA_CONTROL(0)), ==, SSCR_THRESHOLD(5));
    qtest_quit(qts);
}

/*
 * Transmit-ring-empty is a level: set while ring DMA is on and nothing is
 * left to send, clear while an entry is still in the engine, and deaf to an
 * acknowledgement. Linux's IOC3 driver treats it so, and IOC4 follows.
 */
static void test_dma_tx_empty_follows_the_ring(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t data[4] = { 3, 0, 0, 0 };
    const uint8_t control[4] = { TX_DELAY, 0, 0, 0 };

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    g_assert_cmphex(reg_read(qts, 0x08) & TX_RING_EMPTY(0), ==, 0);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    g_assert_cmphex(reg_read(qts, 0x08) & TX_RING_EMPTY(0), ==,
                    TX_RING_EMPTY(0));
    reg_write(qts, 0x08, TX_RING_EMPTY(0));
    g_assert_cmphex(reg_read(qts, 0x08) & TX_RING_EMPTY(0), ==,
                    TX_RING_EMPTY(0));

    ioc4_write_tx_entry(qts, 0, 0x1000, 0, data, control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    g_assert_cmphex(reg_read(qts, 0x08) & TX_RING_EMPTY(0), ==, 0);
    qtest_clock_step(qts, 3 * 1000000);
    g_assert_cmphex(reg_read(qts, 0x08) & TX_RING_EMPTY(0), ==,
                    TX_RING_EMPTY(0));
    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The receive timer's arm is a latch: a consume write with bit 31 clear
 * leaves it, the register reads it back, and the next character still
 * starts the timer. Both Linux ring drivers arm as a read starts and end
 * the read with bit 31 clear.
 */
static void test_dma_consume_without_arm_keeps_it(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t input = 0x6b;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    reg_write(qts, SDMA_RX_TIMER(0), 2);
    reg_write(qts, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    reg_write(qts, SDMA_RX_CONSUME(0), 0);
    g_assert_cmphex(reg_read(qts, SDMA_RX_CONSUME(0)), ==, SRCIR_TIMER_ARM);
    g_assert_cmpint(write(sv[0], &input, 1), ==, 1);
    g_usleep(10000);
    qtest_clock_step(qts, 125001);
    ioc4_wait_reg(qts, 0x08, RX_TIMER_EXPIRED(0), RX_TIMER_EXPIRED(0));
    g_assert_cmphex(reg_read(qts, SDMA_RX_CONSUME(0)), ==, 0);
    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A zero reload expires as soon as the timer starts, as the IOC3 model
 * has it; no source says what the part does, and Linux never writes zero.
 */
static void test_dma_zero_reload_expires_at_once(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t input = 0x7a;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    reg_write(qts, SDMA_RX_TIMER(0), 0);
    reg_write(qts, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    g_assert_cmpint(write(sv[0], &input, 1), ==, 1);
    ioc4_wait_reg(qts, 0x08, RX_TIMER_EXPIRED(0), RX_TIMER_EXPIRED(0));
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 8);
    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A UART pass-through source follows the UART's own interrupt level: it
 * falls when the UART is quieted, with no write to the status register,
 * and a write cannot clear it while the UART still asserts. Linux's IOC3
 * header treats these bits as levels, and IOC4 follows its sibling.
 */
static void test_uart_pass_through_is_a_level(void)
{
    QTestState *qts = ioc4_start(33333333);

    reg_write(qts, 0x08, 0xffffffff);
    qtest_writeb(qts, UART(0) + 1, 2);
    g_assert_cmphex(reg_read(qts, 0x08) & PASS(0), ==, PASS(0));
    reg_write(qts, 0x08, PASS(0));
    g_assert_cmphex(reg_read(qts, 0x08) & PASS(0), ==, PASS(0));

    /* Quieted by the UART alone, unacknowledged, it falls. */
    qtest_writeb(qts, UART(1) + 1, 2);
    g_assert_cmphex(reg_read(qts, 0x08) & PASS(1), ==, PASS(1));
    g_assert_cmphex(qtest_readb(qts, UART(1) + 2) & 0xf, ==, 2);
    g_assert_cmphex(reg_read(qts, 0x08) & PASS(1), ==, 0);
    qtest_quit(qts);
}

static void test_dma_rx_timer_expiry_after_full_entry(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t input[] = { 0x31, 0x32, 0x33, 0x34 };
    uint8_t entry[8];
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    reg_write(qts, SDMA_RX_TIMER(0), 2);
    reg_write(qts, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    g_assert_cmpint(write(sv[0], input, sizeof(input)), ==, sizeof(input));
    g_usleep(10000);
    ioc4_wait_reg(qts, SDMA_RX_PRODUCE(0), 0xff8, 8);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x1000), entry,
                  sizeof(entry));
    g_assert_cmpmem(entry, sizeof(input), input, sizeof(input));
    g_assert_cmphex(entry[4], ==, RX_DATA_VALID);
    g_assert_cmphex(entry[7], ==, RX_DATA_VALID);

    qtest_clock_step(qts, 124999);
    g_assert_cmphex(reg_read(qts, 0x08) & RX_TIMER_EXPIRED(0), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(reg_read(qts, 0x08) & RX_TIMER_EXPIRED(0), ==,
                    RX_TIMER_EXPIRED(0));
    reg_write(qts, 0x08, RX_TIMER_EXPIRED(0));
    qtest_clock_step(qts, 2 * 125000);
    g_assert_cmphex(reg_read(qts, 0x08) & RX_TIMER_EXPIRED(0), ==, 0);

    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_error_capture_and_reset(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t tx_data[4] = { 0x72, 0, 0, 0 };
    const uint8_t tx_status[4] = { TX_VALID, 0, 0, 0 };
    const uint8_t rx_input = 0x73;
    uint32_t expected_low = (uint32_t)DMA_BAD_BASE | ERROR_VALID;
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base(qts, 0, DMA_BAD_BASE, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, tx_data, tx_status);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(0)), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x00), ==, expected_low);
    g_assert_cmphex(reg_read(qts, 0x04), ==, DMA_BAD_BASE >> 32);
    g_assert_cmphex(reg_read(qts, 0x0c) & DMA_ERROR(0), ==,
                    DMA_ERROR(0));

    /*
     * The channel reset recovers the port but leaves the pending source,
     * which software acknowledges itself, as Linux's driver does.
     */
    reg_write(qts, SDMA_CONTROL(0), SSCR_RESET);
    g_assert_cmphex(reg_read(qts, 0x00), ==, expected_low);
    g_assert_cmphex(reg_read(qts, 0x0c) & DMA_ERROR(0), ==, DMA_ERROR(0));
    reg_write(qts, 0x0c, DMA_ERROR(0));
    g_assert_cmphex(reg_read(qts, 0x0c) & DMA_ERROR(0), ==, 0);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    g_assert_cmpint(write(sv[0], &rx_input, 1), ==, 1);
    g_usleep(10000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_DRAIN);
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x00), ==,
                    expected_low | ERROR_MULTIPLE);
    g_assert_cmphex(reg_read(qts, 0x0c) & DMA_ERROR(0), ==,
                    DMA_ERROR(0));
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_dma_requester_ids(void)
{
    unsigned port;

    for (port = 0; port < 4; port++) {
        g_autofree char *log = ioc4_log_file_new();
        g_autofree char *contents = NULL;
        const uint8_t data[4] = { 0x7a, 0x7a, 0x7a, 0x7a };
        const uint8_t control[4] = { TX_VALID, 0, 0, 0 };
        int sv[2];
        QTestState *qts;
        uint32_t requester;

        ioc4_socketpair(sv);
        qts = ioc4_start_dma(false, true, port, sv[1], log);
        close(sv[1]);
        ioc4_set_pair_base(qts, port / 2, DMA_BAD_BASE, 0x400);
        reg_write(qts, SDMA_CONTROL(port), SSCR_DMA_EN);
        ioc4_write_tx_entry(qts, port, 0x400, 0, data, control);
        reg_write(qts, SDMA_TX_PRODUCE(port), 8);
        ioc4_wait_reg(qts, 0x0c, DMA_ERROR(port), DMA_ERROR(port));
        requester = 2 * port;
        g_assert_cmphex(reg_read(qts, 0x00) & 0x1e, ==, requester << 1);
        g_assert_cmphex(reg_read(qts, 0x00) & ERROR_VALID, ==, ERROR_VALID);
        g_assert_cmphex(reg_read(qts, 0x04), ==, DMA_BAD_BASE >> 32);

        qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
        ioc4_map(qts);
        qtest_writel(qts, CFG + 0x04, 0x00000006);
        ioc4_set_pair_base(qts, port / 2, DMA_BAD_BASE, 0x400);
        reg_write(qts, SDMA_CONTROL(port), SSCR_DMA_EN);
        g_assert_cmpint(write(sv[0], data, sizeof(data)), ==, sizeof(data));
        ioc4_wait_reg(qts, 0x0c, DMA_ERROR(port), DMA_ERROR(port));
        requester = 2 * port + 1;
        g_assert_cmphex(reg_read(qts, 0x00) & 0x1e, ==, requester << 1);
        g_assert_cmphex(reg_read(qts, 0x00) & ERROR_VALID, ==, ERROR_VALID);
        g_assert_cmphex(reg_read(qts, 0x04), ==, DMA_BAD_BASE >> 32);
        g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(port)), ==, 0);
        qtest_quit(qts);
        close(sv[0]);
        contents = ioc4_log_file_take(log);
        g_assert_nonnull(contents);
    }
}

static void test_dma_port_fault_isolation_and_recovery(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t fault_data[4] = { 0x61, 0, 0, 0 };
    const uint8_t fault_status[4] = { TX_VALID, 0, 0, 0 };
    const uint8_t peer_output = fault_data[0];
    const uint8_t recovered_data[4] = { 0x15, 0, 0, 0 };
    const uint8_t recovered_status[4] = { TX_MCR, 0, 0, 0 };
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);
    ioc4_set_pair_base(qts, 0, DMA_BAD_BASE, 0x400);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    reg_write(qts, SDMA_CONTROL(1), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 1, 0x400, 0, fault_data, fault_status);
    reg_write(qts, SDMA_TX_PRODUCE(1), 8);
    ioc4_wait_reg(qts, 0x0c, DMA_ERROR(1), DMA_ERROR(1));
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(1)), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x00) & 0x1e, ==, ERROR_REQUESTER(2));
    g_assert_cmphex(reg_read(qts, 0x00) & ERROR_VALID, ==, ERROR_VALID);
    g_assert_cmphex(reg_read(qts, 0x04), ==, DMA_BAD_BASE >> 32);

    /* The pair base is shared, so quiesce both ports before changing it. */
    reg_write(qts, SDMA_CONTROL(0), 0);
    reg_write(qts, SDMA_CONTROL(1), SSCR_RESET);
    g_assert_cmphex(reg_read(qts, SDMA_TX_PRODUCE(1)), ==, 0);
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(1)), ==, 0);
    ioc4_set_pair_base_default(qts, 0, 0x400);
    g_assert_cmphex(reg_read(qts, SDMA_BASE_LO), ==,
                    (uint32_t)DMA_PAIR_BASE);
    g_assert_cmphex(reg_read(qts, SDMA_BASE_HI), ==,
                    DMA_PAIR_BASE >> 32);
    g_assert_cmphex(reg_read(qts, 0x00) & ERROR_VALID, ==, ERROR_VALID);
    g_assert_cmphex(reg_read(qts, 0x04), ==, DMA_BAD_BASE >> 32);

    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 0, 0x400, 0, fault_data, fault_status);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    ioc4_expect_bytes(qts, sv[0], &peer_output, 1);
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(0), 0x3f8, 8);
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(1)), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x00) & ERROR_VALID, ==, ERROR_VALID);
    g_assert_cmphex(reg_read(qts, 0x04), ==, DMA_BAD_BASE >> 32);

    reg_write(qts, SDMA_CONTROL(1), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 1, 0x400, 0, recovered_data,
                        recovered_status);
    reg_write(qts, SDMA_TX_PRODUCE(1), 8);
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(1), 0x3f8, 8);
    /* The shadow mirrors the UART: the outputs, beside its line status. */
    g_assert_cmphex(reg_read(qts, SDMA_SHADOW(1)) & SDMA_SHADOW_OUTPUTS, ==,
                    0x15000000);
    g_assert_cmphex(reg_read(qts, 0x00) & ERROR_VALID, ==, ERROR_VALID);
    g_assert_cmphex(reg_read(qts, 0x04), ==, DMA_BAD_BASE >> 32);

    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(strstr(contents,
                            "serial DMA read failed for port 1"));
}

static void test_dma_rejects_bad_base_and_pointer(void)
{
    g_autofree char *log = ioc4_log_file_new();
    QTestState *qts = ioc4_start_dma(false, true, 0, -1, log);
    g_autofree char *contents = NULL;

    ioc4_set_pair_base(qts, 0, DMA_PAIR_BASE + 8, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    g_assert_cmphex(reg_read(qts, SDMA_CONTROL(0)) & SSCR_DMA_EN, ==, 0);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_set_pair_base(qts, 0, DMA_PAIR_BASE + 0x4000, 0x1000);
    g_assert_cmphex(reg_read(qts, SDMA_BASE_LO), ==,
                    (uint32_t)DMA_PAIR_BASE | 1);
    reg_write(qts, SDMA_CONTROL(0), SSCR_RESET);
    reg_write(qts, SDMA_TX_PRODUCE(0), 4);
    g_assert_cmphex(reg_read(qts, SDMA_TX_PRODUCE(0)), ==, 0);
    qtest_quit(qts);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(strstr(contents, "misaligned DMA pair base"));
    g_assert_nonnull(strstr(contents, "DMA base change rejected"));
    g_assert_nonnull(strstr(contents, "outside the pointer field"));
}

static void test_dma_full_reset_clears_shadow(void)
{
    g_autofree char *log = ioc4_log_file_new();
    QTestState *qts = ioc4_start_dma(false, true, 0, -1, log);
    g_autofree char *contents = NULL;

    qtest_writeb(qts, UART(0) + 4, 0x13);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 4), ==, 0x13);
    g_assert_cmphex(reg_read(qts, SDMA_SHADOW(0)) & SDMA_SHADOW_OUTPUTS,
                    ==, 0x13000000);
    /* A shadow write changes nothing: the shadow mirrors the UART. */
    reg_write(qts, SDMA_SHADOW(0), 0x1f000000);
    g_assert_cmphex(reg_read(qts, SDMA_SHADOW(0)) & SDMA_SHADOW_OUTPUTS,
                    ==, 0x13000000);
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    ioc4_map(qts);
    /* After reset it shows the UART's reset modem control, OUT2 alone. */
    g_assert_cmphex(qtest_readb(qts, UART(0) + 4), ==, 0x08);
    g_assert_cmphex(reg_read(qts, SDMA_SHADOW(0)) & SDMA_SHADOW_OUTPUTS,
                    ==, 0x08000000);
    qtest_quit(qts);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_serial_idle_status(void)
{
    g_autofree char *log = ioc4_log_file_new();
    g_autofree char *contents = NULL;
    int sv[2];
    QTestState *qts;
    const uint8_t byte = 0x5a;
    const uint8_t tx_data[4] = { 0x5a, 0, 0, 0 };
    const uint8_t mcr_data[4] = { 0x15, 0, 0, 0 };
    const uint8_t mcr_control[4] = { TX_MCR | TX_EXPLICIT, 0, 0, 0 };
    uint32_t serial_pending, other_pending;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma_full(false, true, 0, sv[1], log,
                              false, false, false, false);
    close(sv[1]);

    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);
    reg_write(qts, SIO_CR, SIO_DIAG_IDLE);
    qtest_readb(qts, BAR + SIO_CR);
    qtest_readl(qts, BAR + SIO_CR + 1);
    qtest_writeb(qts, BAR + SIO_CR, SIO_DIAG_IDLE);
    qtest_writel(qts, BAR + SIO_CR + 1, SIO_DIAG_IDLE);
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);
    g_assert_cmphex(reg_read(qts, 0x100), ==, 0);
    reg_write(qts, 0x100, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);

    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_PAUSE);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, tx_data,
                        (const uint8_t[4]) { TX_VALID, 0, 0, 0 });
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    g_assert_cmphex(reg_read(qts, SDMA_CONTROL(0)) & SSCR_PAUSE_STATE, !=, 0);
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);

    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    ioc4_map(qts);
    qtest_writeb(qts, UART(0), byte);
    ioc4_expect_bytes(qts, sv[0], &byte, sizeof(byte));
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    ioc4_map(qts);
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);
    qtest_writel(qts, CFG + 0x04, 0x00000006);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    ioc4_write_tx_entry(qts, 1, 0x1000, 0, mcr_data, mcr_control);
    reg_write(qts, SDMA_CONTROL(1), SSCR_DMA_EN);
    reg_write(qts, SDMA_TX_PRODUCE(1), 8);
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(1), 0xff8, 8);
    serial_pending = reg_read(qts, 0x08);
    other_pending = reg_read(qts, 0x0c);
    g_assert_cmphex(serial_pending, !=, 0);
    reg_write(qts, SIO_CR, SIO_DIAG_IDLE);
    reg_write(qts, 0x100, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x08), ==, serial_pending);
    g_assert_cmphex(reg_read(qts, 0x0c), ==, other_pending);
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
    g_assert_nonnull(strstr(contents, "read-only serial status"));
    g_assert_nonnull(strstr(contents, "Invalid read"));
    g_assert_nonnull(strstr(contents, "Invalid write"));
    g_assert_nonnull(strstr(contents, "unsupported BAR offset 0x100"));
}


static void test_serial_idle_status_backend_backpressure(void)
{
    g_autofree char *log = ioc4_log_file_new();
    const uint8_t tx_data[4] = { 0x41, 0x42, 0x43, 0x44 };
    const uint8_t tx_control[4] = { TX_VALID, 0, 0, 0 };
    QTestState *qts = ioc4_start_dma_full(false, true, 0, -1, log,
                                           true, false, false, false);
    g_autofree char *contents = NULL;

    ioc4_set_pair_base_default(qts, 0, 0x1000);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, tx_data, tx_control);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    g_usleep(10000);
    g_assert_cmphex(ioc4_target_property(qts, "x-test-dma-observer-calls"),
                    ==, 1);
    /*
     * With no backend the 16550 sends the byte anyway, as the line would,
     * so the entry completes; a connected backend that cannot take it is
     * held, for up to two seconds, by the 16550 core.
     */
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(0)), ==, 8);
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);
    qtest_quit(qts);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_serial_idle_status_default_pio(void)
{
    g_autofree char *log = ioc4_log_file_new();
    QTestState *qts = ioc4_start_dma(false, false, 0, -1, log);
    g_autofree char *contents = NULL;

    g_assert_cmphex(reg_read(qts, SIO_CR), ==, 0);
    reg_write(qts, SIO_CR, SIO_DIAG_IDLE);
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x100), ==, 0);
    reg_write(qts, 0x100, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, 0);
    qtest_quit(qts);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(strstr(contents, "unsupported BAR offset 0x20"));
    g_assert_nonnull(strstr(contents, "unsupported BAR offset 0x100"));
}

static void test_serial_idle_status_dma_callbacks(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t input[] = { 0x61, 0x62, 0x63 };
    const uint8_t tx_data[4] = { 0x41, 0x42, 0x43, 0x44 };
    const uint8_t tx_control[4] = { TX_VALID, 0, 0, 0 };
    uint8_t entry[8];
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma_full(false, true, 0, sv[1], log,
                              true, false, false, false);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    /* A long reload: a zero one now expires at once, posting early. */
    reg_write(qts, SDMA_RX_TIMER(0), 0xfff);
    reg_write(qts, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    g_assert_cmpint(write(sv[0], input, sizeof(input)), ==, sizeof(input));
    g_usleep(10000);
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_DRAIN);
    ioc4_wait_reg(qts, SDMA_RX_PRODUCE(0), 0xff8, 8);
    g_assert_cmphex(ioc4_target_property(qts, "x-test-dma-observer-calls"),
                    ==, 1);
    g_assert_cmphex(ioc4_target_property(qts, "x-test-dma-observer-status"),
                    ==, 0);
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x1000), entry,
                  sizeof(entry));
    g_assert_cmpmem(entry, sizeof(input), input, sizeof(input));

    ioc4_write_tx_entry(qts, 0, 0x1000, 0, tx_data, tx_control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    ioc4_expect_bytes(qts, sv[0], tx_data, 1);
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(0), 0xff8, 8);
    g_assert_cmphex(ioc4_target_property(qts, "x-test-dma-observer-calls"),
                    ==, 2);
    g_assert_cmphex(ioc4_target_property(qts, "x-test-dma-observer-status"),
                    ==, 0);
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_serial_idle_status_nested_dma(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t input[] = { 0x71, 0x72, 0x73 };
    const uint8_t nested_tx_data[4] = { 0x15, 0, 0, 0 };
    const uint8_t nested_tx_control[4] = { TX_MCR, 0, 0, 0 };
    uint8_t entry[8];
    g_autofree char *contents = NULL;

    ioc4_socketpair(sv);
    qts = ioc4_start_dma_full(false, true, 0, sv[1], log,
                              true, false, false, true);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    ioc4_write_tx_entry(qts, 1, 0x1000, 0, nested_tx_data,
                        nested_tx_control);
    reg_write(qts, SDMA_CONTROL(1), SSCR_DMA_EN);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    /* A long reload: a zero one now expires at once, posting early. */
    reg_write(qts, SDMA_RX_TIMER(0), 0xfff);
    reg_write(qts, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    g_assert_cmpint(write(sv[0], input, sizeof(input)), ==, sizeof(input));
    g_usleep(10000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_DRAIN);
    ioc4_wait_reg(qts, SDMA_RX_PRODUCE(0), 0xff8, 8);
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(1)), ==, 8);
    /* The shadow mirrors the UART: the outputs, beside its line status. */
    g_assert_cmphex(reg_read(qts, SDMA_SHADOW(1)) & SDMA_SHADOW_OUTPUTS, ==,
                    0x15000000);
    g_assert_cmphex(ioc4_target_property(qts, "x-test-dma-observer-calls"),
                    >=, 2);
    g_assert_cmphex(ioc4_target_property(qts,
                    "x-test-dma-observer-status"), ==, 0);
    g_assert_cmphex(ioc4_target_property(qts,
                    "x-test-dma-observer-after-nested-status"), ==, 0);
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x1000), entry,
                  sizeof(entry));
    g_assert_cmpmem(entry, sizeof(input), input, sizeof(input));
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_serial_idle_status_dma_failures(void)
{
    g_autofree char *log = ioc4_log_file_new();
    int sv[2];
    QTestState *qts;
    const uint8_t input[] = { 0x61, 0x62, 0x63 };
    const uint8_t tx_data[4] = { 0x41, 0x42, 0x43, 0x44 };
    const uint8_t tx_control[4] = { TX_VALID, 0, 0, 0 };
    const uint8_t zero[8] = { 0 };
    uint8_t entry[8];
    g_autofree char *contents = NULL;
    g_autofree char *rx_log = NULL;

    qts = ioc4_start_dma_full(false, true, 0, -1, log,
                              true, true, false, false);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, tx_data, tx_control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    ioc4_wait_reg(qts, 0x0c, DMA_ERROR(0), DMA_ERROR(0));
    g_assert_cmphex(ioc4_target_property(qts, "x-test-dma-observer-calls"),
                    ==, 1);
    g_assert_cmphex(ioc4_target_property(qts, "x-test-dma-observer-status"),
                    ==, 0);
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(0)), ==, 0);
    ioc4_dma_read(qts, ioc4_tx_ring_offset(0, 0x1000), entry,
                  sizeof(entry));
    g_assert_cmpmem(entry, sizeof(tx_data), tx_data, sizeof(tx_data));
    g_assert_cmpmem(entry + 4, sizeof(tx_control), tx_control,
                    sizeof(tx_control));
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);
    qtest_quit(qts);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);

    rx_log = ioc4_log_file_new();
    ioc4_socketpair(sv);
    qts = ioc4_start_dma_full(false, true, 0, sv[1], rx_log,
                              true, false, true, false);
    close(sv[1]);
    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    reg_write(qts, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    g_assert_cmpint(write(sv[0], input, sizeof(input)), ==, sizeof(input));
    g_usleep(10000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_DRAIN);
    ioc4_wait_reg(qts, 0x0c, DMA_ERROR(0), DMA_ERROR(0));
    g_assert_cmphex(ioc4_target_property(qts, "x-test-dma-observer-calls"),
                    ==, 1);
    g_assert_cmphex(ioc4_target_property(qts, "x-test-dma-observer-status"),
                    ==, 0);
    g_assert_cmphex(reg_read(qts, SDMA_RX_PRODUCE(0)), ==, 0);
    ioc4_dma_read(qts, ioc4_rx_ring_offset(0, 0x1000), entry,
                  sizeof(entry));
    g_assert_cmpmem(entry, sizeof(entry), zero, sizeof(zero));
    g_assert_cmphex(reg_read(qts, SIO_CR), ==, SIO_DIAG_IDLE);
    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(rx_log);
    g_assert_nonnull(contents);
}

static uint32_t reg_read(QTestState *qts, unsigned offset)
{
    return qtest_readl(qts, BAR + offset);
}

static void reg_write(QTestState *qts, unsigned offset, uint32_t value)
{
    qtest_writel(qts, BAR + offset, value);
}

static void test_identity_and_reset(void)
{
    QTestState *qts = ioc4_start(33333333);
    unsigned n;

    g_assert_cmphex(qtest_readl(qts, CFG), ==, 0x100a10a9);
    g_assert_cmphex(qtest_readl(qts, CFG + 8), ==, 0x00ff0053);
    g_assert_cmphex(qtest_readl(qts, CFG + 0x3c) & 0xff00, ==, 0x0100);
    qtest_writel(qts, CFG + 0x10, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, CFG + 0x10), ==, 0xfff00000);
    qtest_writel(qts, CFG + 0x10, PCI_ADDRESS);
    g_assert_cmphex(reg_read(qts, 0x08), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x0c), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x10), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x14), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x24) & 1, ==, 0);
    for (n = 0; n < 4; n++) {
        g_assert_cmphex(qtest_readb(qts, UART(n) + 5) & 0x60, ==, 0x60);
    }
    reg_write(qts, 0x10, PASS(0));
    g_assert_cmphex(reg_read(qts, 0x10), ==, PASS(0));
    reg_write(qts, 0x14, 0x40);
    g_assert_cmphex(reg_read(qts, 0x14), ==, 0x40);
    qtest_irq_intercept_out_named(qts, CHIP, "head-e-pci-intx");
    qtest_writeb(qts, UART(0) + 3, 0x80);
    qtest_writeb(qts, UART(0), 0x34);
    qtest_writeb(qts, UART(0) + 1, 0x12);
    qtest_writeb(qts, UART(0) + 3, 0x03);
    qtest_writeb(qts, UART(0) + 2, 0x01);
    qtest_writeb(qts, UART(0) + 1, 0x02);
    g_assert_cmphex(reg_read(qts, 0x08) & PASS(0), ==, PASS(0));
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    ioc4_map(qts);
    g_assert_cmphex(reg_read(qts, 0x10), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x14), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x24) & 1, ==, 0);
    g_assert_cmphex(reg_read(qts, 0x08) & PASS(0), ==, 0);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 1), ==, 0);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 2), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 2) & 0xc0, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_writeb(qts, UART(0) + 3, 0x80);
    g_assert_cmphex(qtest_readb(qts, UART(0)), ==, 0x0c);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 1), ==, 0);
    qtest_writeb(qts, UART(0) + 3, 0);
    qtest_writeb(qts, UART(0) + 1, 2);
    g_assert_cmphex(reg_read(qts, 0x08) & PASS(0), ==, PASS(0));
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_ata_readback_latch(void)
{
    QTestState *qts = ioc4_start_readback(false, true);
    uint8_t before[8], after[8];
    uint32_t bit;

    qtest_memset(qts, DMA_RAM_BASE + DMA_RAM_OFFSET, 0xa5, sizeof(before));
    qtest_memread(qts, DMA_RAM_BASE + DMA_RAM_OFFSET, before, sizeof(before));
    for (bit = 1; bit; bit <<= 1) {
        reg_write(qts, ATA_READBACK, bit);
        g_assert_cmphex(reg_read(qts, ATA_READBACK), ==, bit);
    }
    reg_write(qts, ATA_READBACK, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, ATA_READBACK), ==, UINT32_MAX);
    qtest_memread(qts, DMA_RAM_BASE + DMA_RAM_OFFSET, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), before, sizeof(before));
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_system_reset(qts);
    g_assert_cmphex(reg_read(qts, ATA_READBACK), ==, 0);
    qtest_quit(qts);
}

static void test_ata_readback_default_unsupported(void)
{
    QTestState *qts = ioc4_start(33333333);

    reg_write(qts, ATA_READBACK, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, ATA_READBACK), ==, 0);
    qtest_quit(qts);
}

static void test_ata_readback_rejects_width(void)
{
    QTestState *qts = ioc4_start_readback(false, true);

    reg_write(qts, ATA_READBACK, 0x12345678);
    qtest_readb(qts, BAR + ATA_READBACK);
    g_assert_cmphex(reg_read(qts, ATA_READBACK), ==, 0x12345678);
    qtest_readw(qts, BAR + ATA_READBACK);
    g_assert_cmphex(reg_read(qts, ATA_READBACK), ==, 0x12345678);
    qtest_writeb(qts, BAR + ATA_READBACK, 0xaa);
    g_assert_cmphex(reg_read(qts, ATA_READBACK), ==, 0x12345678);
    qtest_writew(qts, BAR + ATA_READBACK, 0xbbcc);
    g_assert_cmphex(reg_read(qts, ATA_READBACK), ==, 0x12345678);

    g_assert_cmphex(reg_read(qts, 0x144), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x14c), ==, 0);
    reg_write(qts, 0x144, 0xaabbccdd);
    reg_write(qts, 0x14c, 0x55667788);
    g_assert_cmphex(reg_read(qts, ATA_READBACK), ==, 0x12345678);
    qtest_quit(qts);
}

static void test_ata_readback_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-ioc4-ata-migration-XXXXXX",
                                         &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = ioc4_start_readback(false, true);
    to = ioc4_start_readback(true, true);
    reg_write(from, ATA_READBACK, 0x89abcdef);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(reg_read(to, ATA_READBACK), ==, 0x89abcdef);
    qtest_quit(from);
    qtest_quit(to);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_ata_readback_migration_mode_mismatch(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-ioc4-ata-profile-XXXXXX",
                                         &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = ioc4_start_readback(false, true);
    to = ioc4_start_readback(true, false);
    reg_write(from, ATA_READBACK, 0x89abcdef);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s,"
        "'exit-on-error':false}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    ioc4_wait_migration_failed(to);
    {
        QDict *reply = qtest_qmp_assert_success_ref(to,
            "{'execute':'query-migrate'}");
        const char *error_desc = qdict_get_str(reply, "error-desc");

        g_assert_nonnull(strstr(error_desc, "sgi-ioc4"));
        g_assert_nonnull(strstr(error_desc, "0 != 1"));
        qobject_unref(reply);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_uart_and_clock(void)
{
    unsigned clock, n, timeout_ms[2];

    for (clock = 0; clock < 2; clock++) {
        QTestState *qts = ioc4_start(clock ? 66666667 : 33333333);

        qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
        g_assert_cmphex(reg_read(qts, 0x24) & 1, ==, clock);
        reg_write(qts, 0x24, !clock);
        g_assert_cmphex(reg_read(qts, 0x24) & 1, ==, clock);
        for (n = 0; n < 4; n++) {
            qtest_writeb(qts, UART(n) + 3, 0x80);
            qtest_writeb(qts, UART(n), 0x31 + n);
            qtest_writeb(qts, UART(n) + 1, 0x12 + n);
            g_assert_cmphex(qtest_readb(qts, UART(n)), ==, 0x31 + n);
            g_assert_cmphex(qtest_readb(qts, UART(n) + 1), ==, 0x12 + n);
            qtest_writeb(qts, UART(n) + 3, 3);
            qtest_writeb(qts, UART(n) + 2, 1);
            g_assert_cmphex(qtest_readb(qts, UART(n) + 2) & 0xc0, ==, 0xc0);
            qtest_writeb(qts, UART(n) + 2, 7);
            g_assert_cmphex(qtest_readb(qts, UART(n) + 5) & 0x60, ==, 0x60);
        }
        qtest_writeb(qts, UART(0) + 3, 0x83);
        qtest_writeb(qts, UART(0), 0xe8);
        qtest_writeb(qts, UART(0) + 1, 3);
        qtest_writeb(qts, UART(0) + 3, 3);
        qtest_writeb(qts, UART(0) + 2, 0x41);
        qtest_writeb(qts, UART(0) + 4, 0x10);
        qtest_writeb(qts, UART(0) + 1, 1);
        qtest_writeb(qts, UART(0), 0x5a);
        g_assert_cmphex(qtest_readb(qts, UART(0) + 2) & 0xf, ==, 1);
        for (timeout_ms[clock] = 1; timeout_ms[clock] <= 200;
             timeout_ms[clock]++) {
            qtest_clock_step(qts, 1000000);
            if ((qtest_readb(qts, UART(0) + 2) & 0xf) == 0xc) {
                break;
            }
        }
        g_test_message("clock=%u timeout_ms=%u"
                       " iir=%02x lsr=%02x ier=%02x lcr=%02x",
                       clock, timeout_ms[clock],
                       qtest_readb(qts, UART(0) + 2),
                       qtest_readb(qts, UART(0) + 5),
                       qtest_readb(qts, UART(0) + 1),
                       qtest_readb(qts, UART(0) + 3));
        g_assert_cmpuint(timeout_ms[clock], <=, 200);
        qtest_quit(qts);
    }
    g_assert_cmpuint(timeout_ms[0], >, timeout_ms[1] * 3 / 2);
}

static void test_interrupts(void)
{
    QTestState *qts = ioc4_start(33333333);
    unsigned n;

    qtest_irq_intercept_out_named(qts, CHIP, "head-e-pci-intx");
    reg_write(qts, 0x18, 0xffffffff);
    g_assert_cmphex(reg_read(qts, 0x10), ==, 0);
    reg_write(qts, 0x1c, 0xffffffff);
    g_assert_cmphex(reg_read(qts, 0x14), ==, 0);
    reg_write(qts, 0x08, 0xffffffff);
    reg_write(qts, 0x0c, 0xffffffff);
    for (n = 0; n < 4; n++) {
        qtest_writeb(qts, UART(n) + 1, 2);
        g_assert_cmphex(reg_read(qts, 0x08) & PASS(n), ==, PASS(n));
        g_assert_false(qtest_get_irq(qts, 0));
        reg_write(qts, 0x10, PASS(n));
        g_assert_cmphex(reg_read(qts, 0x10), ==, PASS(n));
        g_assert_true(qtest_get_irq(qts, 0));
        reg_write(qts, 0x08, PASS(n));
        g_assert_cmphex(reg_read(qts, 0x08) & PASS(n), ==, PASS(n));
        g_assert_true(qtest_get_irq(qts, 0));
        g_assert_cmphex(qtest_readb(qts, UART(n) + 2) & 0xf, ==, 2);
        reg_write(qts, 0x08, PASS(n));
        g_assert_cmphex(reg_read(qts, 0x08) & PASS(n), ==, 0);
        g_assert_false(qtest_get_irq(qts, 0));
        reg_write(qts, 0x18, PASS(n));
        g_assert_cmphex(reg_read(qts, 0x10), ==, 0);
        qtest_writeb(qts, UART(n) + 1, 0);
    }
    for (n = 0; n < 4; n++) {
        qtest_writeb(qts, UART(n) + 1, 2);
        reg_write(qts, 0x10, PASS(n));
    }
    g_assert_cmphex(reg_read(qts, 0x08) & 0x40404040, ==, 0x40404040);
    qtest_readb(qts, UART(0) + 2);
    reg_write(qts, 0x08, PASS(0));
    g_assert_cmphex(reg_read(qts, 0x08) & 0x40404040, ==, 0x40404000);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(reg_read(qts, 0x0c), ==, 0);
    reg_write(qts, 0x14, 0x40);
    g_assert_cmphex(reg_read(qts, 0x14), ==, 0x40);
    reg_write(qts, 0x1c, 0x40);
    g_assert_cmphex(reg_read(qts, 0x14), ==, 0);
    qtest_quit(qts);
}

static void test_pio_fifo(void)
{
    QTestState *qts = ioc4_start(33333333);
    unsigned n, i;

    for (n = 0; n < 4; n++) {
        qtest_writeb(qts, UART(n) + 3, 3);
        qtest_writeb(qts, UART(n) + 2, 1);
        qtest_writeb(qts, UART(n) + 4, 0x10);
        for (i = 0; i < 16; i++) {
            qtest_writeb(qts, UART(n), 0x40 + i);
        }
        g_assert_cmphex(qtest_readb(qts, UART(n) + 5) & 1, ==, 1);
        qtest_writel(qts, UART(n), 0x11223344);
        g_assert_cmphex(qtest_readb(qts, UART(n) + 3), ==, 3);
        g_assert_cmphex(qtest_readb(qts, UART(n) + 1), ==, 0);
        qtest_readl(qts, UART(n));
        for (i = 0; i < 16; i++) {
            g_assert_cmphex(qtest_readb(qts, UART(n)), ==, 0x40 + i);
        }
        g_assert_cmphex(qtest_readb(qts, UART(n) + 5) & 1, ==, 0);
        qtest_writeb(qts, UART(n), 0xaa);
        g_assert_cmphex(qtest_readb(qts, UART(n) + 5) & 1, ==, 1);
        qtest_writeb(qts, UART(n) + 2, 3);
        g_assert_cmphex(qtest_readb(qts, UART(n) + 5) & 1, ==, 0);
    }
    qtest_quit(qts);
}

static void test_dma_chardev_reconnect(void)
{
    g_autofree char *log = ioc4_log_file_new();
    QTestState *qts;
    QDict *reply;
    const uint8_t pio_data = 0x70;
    const uint8_t dma_data[4] = { 0x61, 0x62, 0, 0 };
    const uint8_t dma_control[4] = { TX_VALID, TX_VALID, 0, 0 };
    g_autofree char *contents = NULL;
    int sv[2];

    ioc4_socketpair(sv);
    qts = ioc4_start_dma(false, true, 0, sv[1], log);
    close(sv[1]);

    qtest_qmp_assert_success(qts,
        "{'execute':'chardev-change','arguments':{'id':'serial',"
        "'backend':{'type':'ringbuf','data':{'size':4096}}}}");
    qtest_qmp_assert_success(qts,
        "{'execute':'chardev-change','arguments':{'id':'serial',"
        "'backend':{'type':'ringbuf','data':{'size':4096}}}}");

    qtest_writeb(qts, UART(0), pio_data);
    qtest_clock_step(qts, 2 * 1000000);
    reply = qtest_qmp(qts,
        "{'execute':'ringbuf-read','arguments':{'device':'serial',"
        "'size':16}}");
    g_assert_cmpstr(qdict_get_str(reply, "return"), ==, "p");
    qobject_unref(reply);

    ioc4_set_pair_base_default(qts, 0, 0x1000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(qts, 0, 0x1000, 0, dma_data, dma_control);
    reg_write(qts, SDMA_TX_PRODUCE(0), 8);
    ioc4_wait_reg(qts, SDMA_TX_CONSUME(0), 0xff8, 8);
    reply = qtest_qmp(qts,
        "{'execute':'ringbuf-read','arguments':{'device':'serial',"
        "'size':16}}");
    g_assert_cmpstr(qdict_get_str(reply, "return"), ==, "ab");
    qobject_unref(reply);

    qtest_quit(qts);
    close(sv[0]);
    contents = ioc4_log_file_take(log);
    g_assert_nonnull(contents);
}

static void test_uart_rejects_aligned_wide_write(void)
{
    QTestState *qts = ioc4_start(33333333);

    qtest_writeb(qts, UART(0) + 4, 0x10);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 4), ==, 0x10);
    qtest_writel(qts, UART(0), 0x5a5a5a5a);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 5) & 1, ==, 0);

    qtest_writeb(qts, UART(0), 0x5a);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 5) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(qts, UART(0)), ==, 0x5a);
    qtest_quit(qts);
}

static void test_xbridge_unaligned_split(void)
{
    QTestState *qts = ioc4_start(33333333);

    qtest_writeb(qts, UART(0) + 3, 3);
    qtest_writeb(qts, UART(0) + 1, 0);
    /* XBridge splits this cycle before IOC4 sees it. */
    qtest_writel(qts, UART(0) + 1, 0x55667788);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 1), ==, 5);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 3), ==, 3);
    qtest_quit(qts);
}

static void rtc_write_time(QTestState *qts, uint8_t second, uint8_t minute,
                           uint8_t hour, uint8_t date, uint8_t month,
                           uint8_t year, uint8_t century)
{
    qtest_writeb(qts, RTC(0), 0x80);
    qtest_writeb(qts, RTC(1), second);
    qtest_writeb(qts, RTC(2), minute);
    qtest_writeb(qts, RTC(3), hour);
    qtest_writeb(qts, RTC(4), 0x02);
    qtest_writeb(qts, RTC(5), date);
    qtest_writeb(qts, RTC(6), month);
    qtest_writeb(qts, RTC(7), year);
    qtest_writeb(qts, RTC(0), century);
}

static void test_rtc_absent(void)
{
    QTestState *qts = ioc4_start(33333333);

    g_assert_cmphex(qtest_readb(qts, RTC(0)), ==, 0);
    qtest_writeb(qts, RTC(0), 0x55);
    g_assert_cmphex(qtest_readb(qts, RTC(0)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, BAR + 0x87ff7), ==, 0);
    g_assert_cmphex(qtest_readb(qts, BAR + 0x88000), ==, 0);
    g_assert_cmphex(qtest_readb(qts, BAR + 0x80000), ==, 0);
    qtest_quit(qts);
}

static void test_rtc_alias(void)
{
    QTestState *qts = ioc4_start_full(33333333, false, true);
    static const uint8_t frozen[] = {
        0x60, 0x59, 0x59, 0x23, 0x82, 0x31, 0x12, 0x24,
    };
    unsigned i;

    qtest_irq_intercept_out_named(qts, CHIP, "head-e-pci-intx");
    rtc_write_time(qts, 0x59, 0x59, 0x23, 0x31, 0x12, 0x24, 0x20);
    qtest_writeb(qts, RTC(0), 0x40);
    qtest_clock_step(qts, 2 * 1000000000ULL);
    for (i = 0; i < ARRAY_SIZE(frozen); i++) {
        g_assert_cmphex(qtest_readb(qts, RTC(i)), ==, frozen[i]);
    }
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_writeb(qts, RTC(0), 0);
    g_assert_cmphex(qtest_readb(qts, RTC(1)), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, RTC(5)), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, RTC(6)), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, RTC(7)), ==, 0x25);
    g_assert_false(qtest_get_irq(qts, 0));

    /* The XBridge splits this boundary-crossing word before IOC4 dispatch. */
    g_assert_cmphex(qtest_readw(qts, BAR + 0x87ff7), ==, 0x0020);
    g_assert_cmphex(qtest_readb(qts, BAR + 0x87ff7), ==, 0);
    g_assert_cmphex(qtest_readb(qts, BAR + 0x88000), ==, 0);
    g_assert_cmphex(qtest_readb(qts, BAR + 0x88008), ==, 0);
    g_assert_cmphex(qtest_readb(qts, BAR + 0x80000), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_nvram_window(void)
{
    QTestState *qts = qtest_init(
        "-M none -cpu R10000 -S -device sgi-xio-test-target,id=fabric "
        "-device sgi-xbridge,id=xbridge,host-port=0xb,xio-port=0xc,"
        "fabric=/machine/peripheral/fabric/fabric.ram[0] "
        "-device ds1744,id=clock "
        "-device sgi-ioc4,bus=head-e.0,addr=0,"
        "clock=/machine/peripheral/clock/memory-region");
    static const uint8_t frozen[] = {
        0x60, 0x59, 0x59, 0x23, 0x82, 0x31, 0x12, 0x24,
    };
    static const uint32_t firmware_offsets[] = {
        0x8000e, 0x804bc, 0x87fcb, 0x87fd0,
    };
    unsigned i;

    qtest_sysbus_mmio_map(qts, CHIP, 0, UINT64_C(0x100000000000));
    ioc4_map(qts);
    qtest_irq_intercept_out_named(qts, CHIP, "head-e-pci-intx");

    qtest_writeb(qts, BAR + 0x80000, 0xa5);
    qtest_writeb(qts, BAR + 0x87ff7, 0x5a);
    g_assert_cmphex(qtest_readb(qts, BAR + 0x80000), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, BAR + 0x87ff7), ==, 0x5a);
    for (i = 0; i < ARRAY_SIZE(firmware_offsets); i++) {
        qtest_writeb(qts, BAR + firmware_offsets[i], 0x31 + i);
        g_assert_cmphex(qtest_readb(qts, BAR + firmware_offsets[i]), ==,
                        0x31 + i);
    }

    rtc_write_time(qts, 0x59, 0x59, 0x23, 0x31, 0x12, 0x24, 0x20);
    qtest_writeb(qts, RTC(0), 0x40);
    for (i = 0; i < ARRAY_SIZE(frozen); i++) {
        g_assert_cmphex(qtest_readb(qts, RTC(i)), ==, frozen[i]);
    }
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    ioc4_map(qts);
    g_assert_cmphex(qtest_readb(qts, BAR + 0x80000), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, BAR + 0x87ff7), ==, 0x5a);
    for (i = 0; i < ARRAY_SIZE(firmware_offsets); i++) {
        g_assert_cmphex(qtest_readb(qts, BAR + firmware_offsets[i]), ==,
                        0x31 + i);
    }
    for (i = 0; i < ARRAY_SIZE(frozen); i++) {
        g_assert_cmphex(qtest_readb(qts, RTC(i)), ==, frozen[i]);
    }
    qtest_quit(qts);
}

static void test_nvram_rejects_unsupported_size(void)
{
    const char *args =
        "-M none -cpu R10000 -S "
        "-device sgi-xio-test-target,id=fabric "
        "-device sgi-xbridge,id=xbridge,host-port=0xb,xio-port=0xc,"
        "fabric=/machine/peripheral/fabric/fabric.ram[0] "
        "-device sgi-ioc4,bus=head-e.0,addr=0,"
        "clock=/machine/peripheral/fabric/fabric.ram[0]";

    if (g_test_subprocess()) {
        qtest_quit(qtest_init(args));
        g_assert_not_reached();
    }

    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(
        "*clock region must be 0x800 or 0x8000 bytes*");
}

static void test_rtc_rejects_wide_access(void)
{
    QTestState *qts = ioc4_start_full(33333333, false, true);

    rtc_write_time(qts, 0x12, 0x34, 0x05, 0x15, 0x01, 0x24, 0x20);
    qtest_writeb(qts, RTC(0), 0x80);
    qtest_writeb(qts, RTC(1), 0x11);
    qtest_writeb(qts, RTC(2), 0x22);
    qtest_writew(qts, RTC(2), 0x4433);
    g_assert_cmphex(qtest_readb(qts, RTC(2)), ==, 0x22);
    g_assert_cmphex(qtest_readb(qts, RTC(3)), ==, 0x05);
    qtest_writeb(qts, RTC(0), 0x20);
    g_assert_cmphex(qtest_readb(qts, RTC(2)), ==, 0x22);
    g_assert_cmphex(qtest_readb(qts, RTC(3)), ==, 0x05);
    qtest_quit(qts);
}

static void test_rtc_reset(void)
{
    QTestState *qts = ioc4_start_full(33333333, false, true);

    rtc_write_time(qts, 0x12, 0x34, 0x05, 0x15, 0x01, 0x24, 0x20);
    qtest_writeb(qts, RTC(0), 0x40);
    qtest_clock_step(qts, 2 * 1000000000ULL);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    ioc4_map(qts);
    g_assert_cmphex(qtest_readb(qts, RTC(0)), ==, 0x60);
    g_assert_cmphex(qtest_readb(qts, RTC(1)), ==, 0x12);
    qtest_writeb(qts, RTC(0), 0);
    g_assert_cmphex(qtest_readb(qts, RTC(1)), ==, 0x14);
    qtest_quit(qts);
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-ioc4-migration-XXXXXX",
                                         &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = ioc4_start_full(66666667, false, false);
    to = ioc4_start_full(66666667, true, false);
    qtest_irq_intercept_out_named(to, CHIP, "head-e-pci-intx");
    reg_write(from, 0x10, PASS(1));
    qtest_writeb(from, UART(1) + 3, 3);
    qtest_writeb(from, UART(1) + 2, 1);
    qtest_writeb(from, UART(1) + 4, 0x10);
    qtest_writeb(from, UART(1), 0x73);
    qtest_writeb(from, UART(1) + 1, 1);
    g_assert_cmphex(qtest_readb(from, UART(1) + 5) & 1, ==, 1);
    g_assert_cmphex(reg_read(from, 0x08) & PASS(1), ==, PASS(1));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(reg_read(to, 0x24) & 1, ==, 1);
    g_assert_cmphex(reg_read(to, 0x08) & PASS(1), ==, PASS(1));
    g_assert_cmphex(reg_read(to, 0x10) & PASS(1), ==, PASS(1));
    g_assert_true(qtest_get_irq(to, 0));
    g_assert_cmphex(qtest_readb(to, UART(1) + 5) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(to, UART(1)), ==, 0x73);
    reg_write(to, 0x08, PASS(1));
    g_assert_false(qtest_get_irq(to, 0));
    qtest_quit(from);
    qtest_quit(to);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_dma_tx_delay_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-ioc4-dma-migration-XXXXXX",
                                         &error);
    g_autofree char *socket = NULL, *uri = NULL;
    g_autofree char *from_log = ioc4_log_file_new();
    g_autofree char *to_log = ioc4_log_file_new();
    g_autofree char *from_contents = NULL, *to_contents = NULL;
    const uint8_t data[4] = { 3, 0, 0, 0 };
    const uint8_t control[4] = { TX_DELAY | TX_EXPLICIT, 0, 0, 0 };
    QTestState *from, *to;
    int64_t now;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = ioc4_start_dma(false, true, 0, -1, from_log);
    to = ioc4_start_dma(true, true, 0, -1, to_log);
    ioc4_set_pair_base_default(from, 0, 0x1000);
    reg_write(from, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(from, 0, 0x1000, 0, data, control);
    reg_write(from, SDMA_TX_PRODUCE(0), 8);
    now = qtest_clock_step(from, 1000000);
    g_assert_cmphex(reg_read(from, 0x08) & TX_COMPLETION(0), ==, 0);
    g_assert_cmphex(reg_read(from, SDMA_TX_CONSUME(0)), ==, 0);

    /*
     * The delay migrates as its expiry time on the virtual clock, which a
     * real migration carries; qtest clocks are per process, so bring the
     * far side's up to this one first.
     */
    qtest_clock_set(to, now);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(reg_read(to, SDMA_TX_CONSUME(0)), ==, 0);
    g_assert_cmphex(reg_read(to, 0x08) & TX_COMPLETION(0), ==, 0);
    qtest_clock_step(to, 1999999);
    g_assert_cmphex(reg_read(to, 0x08) & TX_COMPLETION(0), ==, 0);
    qtest_clock_step(to, 1);
    ioc4_wait_reg(to, 0x08, TX_COMPLETION(0), TX_COMPLETION(0));
    ioc4_wait_reg(to, SDMA_TX_CONSUME(0), 0xff8, 8);
    qtest_quit(from);
    qtest_quit(to);
    from_contents = ioc4_log_file_take(from_log);
    to_contents = ioc4_log_file_take(to_log);
    g_assert_nonnull(from_contents);
    g_assert_nonnull(to_contents);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_dma_paused_state_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-ioc4-paused-dma-XXXXXX",
                                         &error);
    g_autofree char *socket = NULL, *uri = NULL;
    g_autofree char *from_log = ioc4_log_file_new();
    g_autofree char *to_log = ioc4_log_file_new();
    g_autofree char *from_contents = NULL, *to_contents = NULL;
    const uint8_t rx_data[4] = { 0x11, 0x22, 0x33, 0x44 };
    const uint8_t tx_data[4] = { 0x41, 3, 0x42, 0 };
    const uint8_t tx_status[4] = { TX_VALID, TX_DELAY | TX_EXPLICIT,
                                   TX_VALID, 0 };
    const uint8_t first_output = 0x41, second_output = 0x42;
    int from_sv[2], to_sv[2];
    uint8_t entry[8];
    QTestState *from, *to;

    g_assert_no_error(error);
    ioc4_socketpair(from_sv);
    ioc4_socketpair(to_sv);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = ioc4_start_dma(false, true, 0, from_sv[1], from_log);
    to = ioc4_start_dma(true, true, 0, to_sv[1], to_log);
    close(from_sv[1]);
    close(to_sv[1]);
    ioc4_set_pair_base_default(from, 0, 0x1000);
    ioc4_set_pair_base_default(to, 0, 0x1000);
    reg_write(from, SDMA_CONTROL(0), SSCR_DMA_EN);
    reg_write(from, SDMA_RX_TIMER(0), 2);
    reg_write(from, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    g_assert_cmpint(write(from_sv[0], rx_data, sizeof(rx_data)), ==,
                    sizeof(rx_data));
    ioc4_wait_reg(from, SDMA_RX_PRODUCE(0), 0xff8, 8);
    ioc4_dma_read(from, ioc4_rx_ring_offset(0, 0x1000), entry,
                  sizeof(entry));
    g_assert_cmpmem(entry, 4, rx_data, sizeof(rx_data));

    ioc4_write_tx_entry(from, 0, 0x1000, 0, tx_data, tx_status);
    reg_write(from, SDMA_TX_PRODUCE(0), 8);
    ioc4_expect_bytes(from, from_sv[0], &first_output, 1);
    g_assert_cmphex(reg_read(from, SDMA_TX_CONSUME(0)), ==, 0);
    g_assert_cmphex(reg_read(from, 0x08) & TX_COMPLETION(0), ==, 0);

    reg_write(from, 0x10, PASS(0));
    qtest_writeb(from, UART(0) + 1, 2);
    g_assert_cmphex(reg_read(from, 0x08) & PASS(0), ==, PASS(0));
    reg_write(from, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_PAUSE);
    g_assert_cmphex(reg_read(from, SDMA_CONTROL(0)) & SSCR_PAUSE_STATE, ==,
                    SSCR_PAUSE_STATE);

    qtest_irq_intercept_out_named(to, CHIP, "head-e-pci-intx");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(reg_read(to, SDMA_CONTROL(0)) &
                    (SSCR_DMA_EN | SSCR_PAUSE_STATE), ==,
                    SSCR_DMA_EN | SSCR_PAUSE_STATE);
    g_assert_cmphex(reg_read(to, SDMA_TX_CONSUME(0)), ==, 0);
    g_assert_cmphex(reg_read(to, SDMA_RX_PRODUCE(0)), ==, 8);
    ioc4_dma_read(to, ioc4_rx_ring_offset(0, 0x1000), entry,
                  sizeof(entry));
    g_assert_cmpmem(entry, 4, rx_data, sizeof(rx_data));
    g_assert_cmphex(reg_read(to, 0x08) & PASS(0), ==, PASS(0));
    g_assert_cmphex(reg_read(to, 0x08) & RX_TIMER_EXPIRED(0), ==, 0);
    g_assert_cmphex(reg_read(to, 0x08) & TX_COMPLETION(0), ==, 0);
    g_assert_true(qtest_get_irq(to, 0));

    reg_write(to, 0x08, PASS(0));
    g_assert_cmphex(reg_read(to, 0x08) & PASS(0), ==, PASS(0));
    g_assert_true(qtest_get_irq(to, 0));
    g_assert_cmphex(qtest_readb(to, UART(0) + 2) & 0xf, ==, 2);
    reg_write(to, 0x08, PASS(0));
    g_assert_cmphex(reg_read(to, 0x08) & PASS(0), ==, 0);
    g_assert_false(qtest_get_irq(to, 0));

    reg_write(to, SDMA_CONTROL(0), SSCR_DMA_EN);
    qtest_clock_step(to, 124999);
    g_assert_cmphex(reg_read(to, 0x08) & RX_TIMER_EXPIRED(0), ==, 0);
    qtest_clock_step(to, 1);
    ioc4_wait_reg(to, 0x08, RX_TIMER_EXPIRED(0), RX_TIMER_EXPIRED(0));
    qtest_clock_step(to, 2874999);
    g_assert_cmphex(reg_read(to, 0x08) & TX_COMPLETION(0), ==, 0);
    qtest_clock_step(to, 1);
    ioc4_wait_reg(to, 0x08, TX_COMPLETION(0), TX_COMPLETION(0));
    ioc4_wait_reg(to, SDMA_TX_CONSUME(0), 0xff8, 8);
    ioc4_expect_bytes(to, to_sv[0], &second_output, 1);
    {
        struct pollfd pfd = { .fd = to_sv[0], .events = POLLIN };

        g_assert_cmpint(poll(&pfd, 1, 50), ==, 0);
    }

    qtest_qmp_assert_success(to, "{'execute':'system_reset'}");
    ioc4_map(to);
    g_assert_cmphex(reg_read(to, 0x08), ==, 0);
    g_assert_cmphex(reg_read(to, SDMA_TX_CONSUME(0)), ==, 0);
    qtest_quit(from);
    qtest_quit(to);
    close(from_sv[0]);
    close(to_sv[0]);
    from_contents = ioc4_log_file_take(from_log);
    to_contents = ioc4_log_file_take(to_log);
    g_assert_nonnull(from_contents);
    g_assert_nonnull(to_contents);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_dma_active_state_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-ioc4-active-dma-XXXXXX",
                                         &error);
    g_autofree char *socket = NULL, *uri = NULL;
    g_autofree char *from_log = ioc4_log_file_new();
    g_autofree char *to_log = ioc4_log_file_new();
    g_autofree char *from_contents = NULL, *to_contents = NULL;
    const uint8_t fill[4] = { 0x10, 0x20, 0x30, 0x40 };
    const uint8_t rx_data[4] = { 0xa1, 0xa2, 0xa3, 0xa4 };
    const uint8_t queued_input = 0xb5;
    const uint8_t tx_data[4] = { 0x41, 3, 0x42, 0 };
    const uint8_t tx_status[4] = { TX_VALID, TX_DELAY | TX_EXPLICIT,
                                   TX_VALID, 0 };
    const uint8_t first_output = tx_data[0], second_output = tx_data[2];
    const uint32_t rx_timer_ns = 2 * 62500;
    const uint32_t tx_delay_ns = 3000000;
    const uint32_t queued_timer_ns = 62500;
    int from_sv[2], to_sv[2];
    uint8_t entry[8];
    unsigned i;
    QTestState *from, *to;

    g_assert_no_error(error);
    ioc4_socketpair(from_sv);
    ioc4_socketpair(to_sv);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = ioc4_start_dma(false, true, 0, from_sv[1], from_log);
    to = ioc4_start_dma(true, true, 0, to_sv[1], to_log);
    close(from_sv[1]);
    close(to_sv[1]);
    ioc4_set_pair_base_default(from, 0, 0x400);
    ioc4_set_pair_base_default(to, 0, 0x400);
    reg_write(from, SDMA_CONTROL(0), SSCR_DMA_EN);
    for (i = 0; i < 127; i++) {
        g_assert_cmpint(write(from_sv[0], fill, sizeof(fill)), ==,
                        sizeof(fill));
        ioc4_wait_reg(from, SDMA_RX_PRODUCE(0), 0x3f8,
                      ((i + 1) * 8) & 0x3f8);
    }
    g_assert_cmphex(reg_read(from, SDMA_RX_PRODUCE(0)), ==, 0x3f8);
    reg_write(from, SDMA_RX_TIMER(0), 2);
    reg_write(from, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    g_assert_cmpint(write(from_sv[0], rx_data, sizeof(rx_data)), ==,
                    sizeof(rx_data));
    ioc4_wait_reg(from, 0x08, RX_RING_FULL(0), RX_RING_FULL(0));
    g_assert_cmphex(reg_read(from, SDMA_RX_PRODUCE(0)), ==, 0x3f8);
    g_assert_cmphex(reg_read(from, 0x08) & RX_TIMER_EXPIRED(0), ==, 0);
    reg_write(from, 0x10, RX_RING_FULL(0) | RX_TIMER_EXPIRED(0));

    ioc4_write_tx_entry(from, 0, 0x400, 0, tx_data, tx_status);
    reg_write(from, SDMA_TX_PRODUCE(0), 8);
    ioc4_expect_bytes(from, from_sv[0], &first_output, 1);
    g_assert_cmphex(reg_read(from, SDMA_TX_CONSUME(0)), ==, 0);
    g_assert_cmphex(reg_read(from, 0x08) & TX_COMPLETION(0), ==, 0);

    qtest_irq_intercept_out_named(to, CHIP, "head-e-pci-intx");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(reg_read(to, SDMA_CONTROL(0)) &
                    (SSCR_DMA_EN | SSCR_PAUSE_STATE), ==, SSCR_DMA_EN);
    g_assert_cmphex(reg_read(to, SDMA_RX_PRODUCE(0)), ==, 0x3f8);
    /* The consume index reads the pointer with the arm, still set. */
    g_assert_cmphex(reg_read(to, SDMA_RX_CONSUME(0)), ==, SRCIR_TIMER_ARM);
    g_assert_cmphex(reg_read(to, SDMA_TX_PRODUCE(0)), ==, 8);
    g_assert_cmphex(reg_read(to, SDMA_TX_CONSUME(0)), ==, 0);
    g_assert_cmphex(reg_read(to, 0x08) & RX_RING_FULL(0), ==,
                    RX_RING_FULL(0));
    g_assert_cmphex(reg_read(to, 0x08) & RX_TIMER_EXPIRED(0), ==, 0);
    g_assert_cmphex(reg_read(to, 0x08) & TX_COMPLETION(0), ==, 0);
    g_assert_true(qtest_get_irq(to, 0));

    qtest_clock_step(to, rx_timer_ns - 1);
    g_assert_cmphex(reg_read(to, 0x08) & RX_TIMER_EXPIRED(0), ==, 0);
    g_assert_cmphex(reg_read(to, SDMA_RX_PRODUCE(0)), ==, 0x3f8);
    qtest_clock_step(to, 1);
    ioc4_wait_reg(to, 0x08, RX_TIMER_EXPIRED(0), RX_TIMER_EXPIRED(0));
    g_assert_cmphex(reg_read(to, 0x08) & RX_RING_FULL(0), ==,
                    RX_RING_FULL(0));
    g_assert_true(qtest_get_irq(to, 0));

    /* Ring-full is a level: it stays while the entry waits for room. */
    reg_write(to, 0x08, RX_RING_FULL(0) | RX_TIMER_EXPIRED(0));
    g_assert_cmphex(reg_read(to, 0x08) & (RX_RING_FULL(0) |
                                          RX_TIMER_EXPIRED(0)), ==,
                    RX_RING_FULL(0));
    g_assert_true(qtest_get_irq(to, 0));
    g_assert_cmpint(write(to_sv[0], &queued_input, 1), ==, 1);
    reg_write(to, SDMA_RX_TIMER(0), 1);
    reg_write(to, SDMA_RX_CONSUME(0), 16 | SRCIR_TIMER_ARM);
    ioc4_wait_reg(to, SDMA_RX_PRODUCE(0), 0x3f8, 0);
    ioc4_dma_read(to, ioc4_rx_ring_offset(0, 0x400) + 0x3f8,
                  entry, sizeof(entry));
    g_assert_cmpmem(entry, 4, rx_data, sizeof(rx_data));
    g_assert_cmphex(reg_read(to, 0x08) & RX_TIMER_EXPIRED(0), ==, 0);

    qtest_clock_step(to, queued_timer_ns - 1);
    g_assert_cmphex(reg_read(to, 0x08) & RX_TIMER_EXPIRED(0), ==, 0);
    qtest_clock_step(to, 1);
    ioc4_wait_reg(to, 0x08, RX_TIMER_EXPIRED(0), RX_TIMER_EXPIRED(0));
    ioc4_wait_reg(to, SDMA_RX_PRODUCE(0), 0x3f8, 8);
    ioc4_dma_read(to, ioc4_rx_ring_offset(0, 0x400), entry,
                  sizeof(entry));
    g_assert_cmphex(entry[0], ==, queued_input);
    g_assert_cmphex(entry[1], ==, 0);
    g_assert_cmphex(entry[4], ==, RX_DATA_VALID);
    g_assert_cmphex(entry[5], ==, 0);
    g_assert_true(qtest_get_irq(to, 0));

    qtest_clock_step(to, tx_delay_ns - rx_timer_ns - queued_timer_ns - 1);
    g_assert_cmphex(reg_read(to, 0x08) & TX_COMPLETION(0), ==, 0);
    qtest_clock_step(to, 1);
    ioc4_wait_reg(to, 0x08, TX_COMPLETION(0), TX_COMPLETION(0));
    ioc4_wait_reg(to, SDMA_TX_CONSUME(0), 0x3f8, 8);
    ioc4_expect_bytes(to, to_sv[0], &second_output, 1);

    /*
     * The ring is full again, but nothing waits for room, so ring-full stays
     * down and acknowledging the timer drops the line.
     */
    reg_write(to, 0x08, RX_TIMER_EXPIRED(0));
    g_assert_cmphex(reg_read(to, 0x08) & RX_RING_FULL(0), ==, 0);
    g_assert_false(qtest_get_irq(to, 0));

    qtest_quit(from);
    qtest_quit(to);
    close(from_sv[0]);
    close(to_sv[0]);
    from_contents = ioc4_log_file_take(from_log);
    to_contents = ioc4_log_file_take(to_log);
    g_assert_nonnull(from_contents);
    g_assert_nonnull(to_contents);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void ioc4_wait_migration_failed(QTestState *qts)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    for (;;) {
        QDict *reply = qtest_qmp_assert_success_ref(qts,
            "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(reply, "status");
        bool failed = g_str_equal(status, "failed");
        bool completed = g_str_equal(status, "completed");
        bool expired = g_get_monotonic_time() >= deadline;

        qobject_unref(reply);
        if (failed) {
            return;
        }
        g_assert_false(completed);
        g_assert_false(expired);
        g_usleep(1000);
    }
}

static void test_ps2_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-ioc4-ps2-migration-XXXXXX",
                                         &error);
    g_autofree char *socket = NULL, *uri = NULL;
    g_autofree char *from_log = ioc4_log_file_new();
    g_autofree char *to_log = ioc4_log_file_new();
    g_autofree char *from_contents = NULL, *to_contents = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = ioc4_start_ps2(false, from_log);
    to = ioc4_start_ps2(true, to_log);
    qtest_irq_intercept_out_named(to, CHIP, "head-e-pci-intx");
    ioc4_send_ps2_key(from, "a");
    reg_write(from, 0x14, KBC_OTHER_IRQ);
    g_assert_cmphex(reg_read(from, 0x0c) & KBC_OTHER_IRQ, ==, KBC_OTHER_IRQ);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_true(qtest_get_irq(to, 0));
    g_assert_cmphex(reg_read(to, 0x0c) & KBC_OTHER_IRQ, ==, KBC_OTHER_IRQ);
    g_assert_cmphex(reg_read(to, KBC_KBD_RECEIVE), ==, 0x801c0000);
    g_assert_true(qtest_get_irq(to, 0));
    reg_write(to, 0x0c, KBC_OTHER_IRQ);
    g_assert_false(qtest_get_irq(to, 0));

    qtest_quit(from);
    qtest_quit(to);
    from_contents = ioc4_log_file_take(from_log);
    to_contents = ioc4_log_file_take(to_log);
    g_assert_nonnull(from_contents);
    g_assert_nonnull(to_contents);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_dma_migration_profile_mismatch(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-ioc4-profile-migration-XXXXXX",
                                         &error);
    g_autofree char *socket = NULL, *uri = NULL;
    g_autofree char *from_log = ioc4_log_file_new();
    g_autofree char *to_log = ioc4_log_file_new();
    g_autofree char *from_contents = NULL, *to_contents = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = ioc4_start_dma(false, true, 0, -1, from_log);
    to = ioc4_start_dma(true, false, 0, -1, to_log);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s,"
        "'exit-on-error':false}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    ioc4_wait_migration_failed(to);
    {
        QDict *reply = qtest_qmp_assert_success_ref(to,
            "{'execute':'query-migrate'}");
        const char *error_desc = qdict_get_str(reply, "error-desc");

        g_assert_nonnull(strstr(error_desc, "sgi-ioc4"));
        g_assert_nonnull(strstr(error_desc, "0 != 1"));
        qobject_unref(reply);
    }
    qtest_quit(from);
    qtest_quit(to);
    from_contents = ioc4_log_file_take(from_log);
    to_contents = ioc4_log_file_take(to_log);
    g_assert_nonnull(from_contents);
    g_assert_nonnull(to_contents);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_rtc_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-ioc4-rtc-migration-XXXXXX",
                                         &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = ioc4_start_full(33333333, false, true);
    to = ioc4_start_full(33333333, true, true);
    qtest_irq_intercept_out_named(to, CHIP, "head-e-pci-intx");
    rtc_write_time(from, 0x12, 0x34, 0x05, 0x15, 0x01, 0x24, 0x20);
    qtest_writeb(from, RTC(0), 0x40);
    qtest_clock_step(from, 4 * 1000000000ULL);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readb(to, RTC(0)), ==, 0x60);
    g_assert_cmphex(qtest_readb(to, RTC(1)), ==, 0x12);
    qtest_writeb(to, RTC(0), 0);
    g_assert_cmphex(qtest_readb(to, RTC(1)), ==, 0x16);
    g_assert_false(qtest_get_irq(to, 0));
    qtest_quit(from);
    qtest_quit(to);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi-ioc4/dma-default-off",
                   test_dma_property_defaults_off);
    qtest_add_func("/sgi-ioc4/dma-tx-direct64-swap-on",
                   test_dma_tx_direct64_swap_on);
    qtest_add_func("/sgi-ioc4/dma-tx-direct64-swap-off",
                   test_dma_tx_direct64_swap_off);
    qtest_add_func("/sgi-ioc4/dma-tx-requires-bus-master",
                   test_dma_tx_requires_bus_master);
    qtest_add_func("/sgi-ioc4/dma-tx-control-entries",
                   test_dma_tx_control_entries);
    qtest_add_func("/sgi-ioc4/dma-tx-pause-reject-preserves-delay",
                   test_dma_tx_pause_reject_preserves_delay);
    qtest_add_func("/sgi-ioc4/dma-rx-drain-threshold",
                   test_dma_rx_drain_and_threshold);
    qtest_add_func("/sgi-ioc4/dma-loopback-pio-mcr",
                   test_dma_loopback_pio_mcr);
    qtest_add_func("/sgi-ioc4/dma-loopback-tx-mcr",
                   test_dma_loopback_tx_mcr);
    qtest_add_func("/sgi-ioc4/dma-loopback-shadow-reset",
                   test_dma_loopback_shadow_reset);
    qtest_add_func("/sgi-ioc4/dma-loopback-discards-external-input",
                   test_dma_loopback_discards_external_input);
    qtest_add_func("/sgi-ioc4/dma-loopback-full-ring-recovery",
                   test_dma_loopback_full_ring_recovery);
    qtest_add_func("/sgi-ioc4/dma-tx-wrap-1k", test_dma_tx_wrap_1k);
    qtest_add_func("/sgi-ioc4/dma-tx-wrap-4k", test_dma_tx_wrap_4k);
    qtest_add_func("/sgi-ioc4/dma-rx-full-staged-recovery",
                   test_dma_rx_full_and_staged_recovery);
    qtest_add_func("/sgi-ioc4/dma-rx-timer-expiry", test_dma_rx_timer_expiry);
    qtest_add_func("/sgi-ioc4/uart-pass-through-is-a-level",
                   test_uart_pass_through_is_a_level);
    qtest_add_func("/sgi-ioc4/dma-zero-reload-expires-at-once",
                   test_dma_zero_reload_expires_at_once);
    qtest_add_func("/sgi-ioc4/dma-consume-without-arm-keeps-it",
                   test_dma_consume_without_arm_keeps_it);
    qtest_add_func("/sgi-ioc4/dma-tx-empty-follows-the-ring",
                   test_dma_tx_empty_follows_the_ring);
    qtest_add_func("/sgi-ioc4/dma-channel-reset-keeps-settings",
                   test_dma_channel_reset_keeps_settings);
    qtest_add_func("/sgi-ioc4/dma-tx-delay-sets-the-busy-bit",
                   test_dma_tx_delay_sets_the_busy_bit);
    qtest_add_func("/sgi-ioc4/dma-tx-control-decodes-type-and-bit5",
                   test_dma_tx_control_decodes_type_and_bit5);
    qtest_add_func("/sgi-ioc4/dma-empty-slot-ends-the-entry",
                   test_dma_empty_slot_ends_the_entry);
    qtest_add_func("/sgi-ioc4/dma-threshold-is-a-level",
                   test_dma_threshold_is_a_level);
    qtest_add_func("/sgi-ioc4/dma-host-index-masks-its-pointer",
                   test_dma_host_index_masks_its_pointer);
    qtest_add_func("/sgi-ioc4/dma-engine-indices-take-writes",
                   test_dma_engine_indices_take_writes);
    qtest_add_func("/sgi-ioc4/dma-control-stores-its-bits",
                   test_dma_control_stores_its_bits);
    qtest_add_func("/sgi-ioc4/dma-rx-timer-count-reads-back",
                   test_dma_rx_timer_count_reads_back);
    qtest_add_func("/sgi-ioc4/dma-rx-timer-due-before-access",
                   test_dma_rx_timer_due_before_access);
    qtest_add_func("/sgi-ioc4/dma-rx-timer-expiry-after-full-entry",
                   test_dma_rx_timer_expiry_after_full_entry);
    qtest_add_func("/sgi-ioc4/dma-error-capture-reset",
                   test_dma_error_capture_and_reset);
    qtest_add_func("/sgi-ioc4/dma-requester-ids", test_dma_requester_ids);
    qtest_add_func("/sgi-ioc4/dma-port-fault-isolation-recovery",
                   test_dma_port_fault_isolation_and_recovery);
    qtest_add_func("/sgi-ioc4/dma-full-reset-shadow",
                    test_dma_full_reset_clears_shadow);
    qtest_add_func("/sgi-ioc4/serial-idle-status",
                    test_serial_idle_status);
    qtest_add_func("/sgi-ioc4/serial-idle-status-default-pio",
                    test_serial_idle_status_default_pio);
    qtest_add_func("/sgi-ioc4/serial-idle-status-backend-backpressure",
                    test_serial_idle_status_backend_backpressure);
    qtest_add_func("/sgi-ioc4/serial-idle-status-dma-callbacks",
                    test_serial_idle_status_dma_callbacks);
    qtest_add_func("/sgi-ioc4/serial-idle-status-nested-dma",
                    test_serial_idle_status_nested_dma);
    qtest_add_func("/sgi-ioc4/serial-idle-status-dma-failures",
                    test_serial_idle_status_dma_failures);
    qtest_add_func("/sgi-ioc4/dma-rejects-bad-base-pointer",
                   test_dma_rejects_bad_base_and_pointer);
    qtest_add_func("/sgi-ioc4/identity-reset", test_identity_and_reset);
    qtest_add_func("/sgi-ioc4/ata-readback-latch",
                   test_ata_readback_latch);
    qtest_add_func("/sgi-ioc4/ata-readback-default-unsupported",
                   test_ata_readback_default_unsupported);
    qtest_add_func("/sgi-ioc4/ata-readback-rejects-width",
                   test_ata_readback_rejects_width);
    qtest_add_func("/sgi-ioc4/ata-readback-migration",
                   test_ata_readback_migration);
    qtest_add_func("/sgi-ioc4/ata-readback-migration-mode-mismatch",
                   test_ata_readback_migration_mode_mismatch);
    qtest_add_func("/sgi-ioc4/uart-clock", test_uart_and_clock);
    qtest_add_func("/sgi-ioc4/ps2-adapter", test_ps2_adapter);
    qtest_add_func("/sgi-ioc4/ps2-ports-present", test_ps2_ports_present);
    qtest_add_func("/sgi-ioc4/ps2-migration", test_ps2_migration);
    qtest_add_func("/sgi-ioc4/interrupts", test_interrupts);
    qtest_add_func("/sgi-ioc4/pio-fifo", test_pio_fifo);
    qtest_add_func("/sgi-ioc4/dma-chardev-reconnect",
                   test_dma_chardev_reconnect);
    qtest_add_func("/sgi-ioc4/uart-rejects-aligned-wide-write",
                   test_uart_rejects_aligned_wide_write);
    qtest_add_func("/sgi-ioc4/xbridge-unaligned-split",
                   test_xbridge_unaligned_split);
    qtest_add_func("/sgi-ioc4/rtc-absent", test_rtc_absent);
    qtest_add_func("/sgi-ioc4/rtc-alias", test_rtc_alias);
    qtest_add_func("/sgi-ioc4/nvram-window", test_nvram_window);
    qtest_add_func("/sgi-ioc4/nvram-rejects-unsupported-size",
                   test_nvram_rejects_unsupported_size);
    qtest_add_func("/sgi-ioc4/rtc-rejects-wide-access",
                   test_rtc_rejects_wide_access);
    qtest_add_func("/sgi-ioc4/rtc-reset", test_rtc_reset);
    qtest_add_func("/sgi-ioc4/migration", test_migration);
    qtest_add_func("/sgi-ioc4/dma-migration-profile-mismatch",
                   test_dma_migration_profile_mismatch);
    qtest_add_func("/sgi-ioc4/dma-tx-delay-migration",
                   test_dma_tx_delay_migration);
    qtest_add_func("/sgi-ioc4/dma-paused-state-migration",
                   test_dma_paused_state_migration);
    qtest_add_func("/sgi-ioc4/dma-active-state-migration",
                   test_dma_active_state_migration);
    qtest_add_func("/sgi-ioc4/rtc-migration", test_rtc_migration);
    return g_test_run();
}
