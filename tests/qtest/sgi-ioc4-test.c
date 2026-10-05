/*
 * IOC4 revision 0x53 serial register tests through a standalone XBridge.
 * This fixture tests the PCI function without assuming an IO9 board route.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-migrate.h"
#include <poll.h>
#include <sys/socket.h>

#define CHIP "/machine/peripheral/xbridge"
#define WIDGET (UINT64_C(0x100000000000) + (UINT64_C(0xe) << 44))
#define CFG (WIDGET + 0x20000)
#define BRIDGE_DEVICE0 (WIDGET + 0x204)
#define PCI_COMMAND_MASTER (1u << 2)
#define BAR (WIDGET + 0x200000)
#define PCI_ADDRESS 0x01000000u
#define UART(n) (BAR + 0x380 + 8 * (n))
#define RTC(n) (BAR + 0x87ff8 + (n))
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
#define SRCIR_TIMER_ARM (1u << 31)
#define TX_VALID 0x40
#define TX_EXPLICIT 0x20
#define TX_MCR 0x80
#define TX_DELAY 0xc0
#define RX_DATA_VALID 0x80
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
    const uint8_t control[4] = { TX_VALID, TX_VALID, 0, 0 };
    const uint8_t expected[2] = { 0x42, 0x41 };
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
    reg_write(qts, 0x08, RX_RING_FULL(0));
    g_assert_cmphex(reg_read(qts, 0x08) & RX_RING_FULL(0), ==, 0);

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

    reg_write(qts, SDMA_CONTROL(0), SSCR_RESET);
    g_assert_cmphex(reg_read(qts, 0x00), ==, expected_low);
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
    g_assert_cmphex(reg_read(qts, SDMA_SHADOW(1)), ==, 0x15000000);
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
    g_assert_nonnull(strstr(contents, "invalid TX producer"));
}

static void test_dma_full_reset_clears_shadow(void)
{
    g_autofree char *log = ioc4_log_file_new();
    QTestState *qts = ioc4_start_dma(false, true, 0, -1, log);
    g_autofree char *contents = NULL;

    reg_write(qts, SDMA_SHADOW(0), 0x1f000000);
    g_assert_cmphex(reg_read(qts, SDMA_SHADOW(0)), ==, 0x1f000000);
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    ioc4_map(qts);
    g_assert_cmphex(reg_read(qts, SDMA_SHADOW(0)), ==, 0);
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
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(0)), ==, 0);
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
    reg_write(qts, SDMA_RX_CONSUME(0), SRCIR_TIMER_ARM);
    g_assert_cmpint(write(sv[0], input, sizeof(input)), ==, sizeof(input));
    g_usleep(10000);
    reg_write(qts, SDMA_CONTROL(0), SSCR_DMA_EN | SSCR_DRAIN);
    ioc4_wait_reg(qts, SDMA_RX_PRODUCE(0), 0xff8, 8);
    g_assert_cmphex(reg_read(qts, SDMA_TX_CONSUME(1)), ==, 8);
    g_assert_cmphex(reg_read(qts, SDMA_SHADOW(1)), ==, 0x15000000);
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
    qtest_writeb(qts, UART(0) + 2, 1);
    qtest_writeb(qts, UART(0) + 3, 0x80);
    qtest_writeb(qts, UART(0), 0x34);
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    ioc4_map(qts);
    g_assert_cmphex(reg_read(qts, 0x10), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x14), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x24) & 1, ==, 0);
    g_assert_cmphex(qtest_readb(qts, UART(0) + 3), ==, 0);
    qtest_irq_intercept_out_named(qts, CHIP, "head-e-pci-intx");
    qtest_writeb(qts, UART(0) + 1, 2);
    g_assert_cmphex(reg_read(qts, 0x08) & PASS(0), ==, PASS(0));
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
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

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = ioc4_start_dma(false, true, 0, -1, from_log);
    to = ioc4_start_dma(true, true, 0, -1, to_log);
    ioc4_set_pair_base_default(from, 0, 0x1000);
    reg_write(from, SDMA_CONTROL(0), SSCR_DMA_EN);
    ioc4_write_tx_entry(from, 0, 0x1000, 0, data, control);
    reg_write(from, SDMA_TX_PRODUCE(0), 8);
    qtest_clock_step(from, 1000000);
    g_assert_cmphex(reg_read(from, 0x08) & TX_COMPLETION(0), ==, 0);
    g_assert_cmphex(reg_read(from, SDMA_TX_CONSUME(0)), ==, 0);

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
    g_assert_cmphex(reg_read(to, SDMA_RX_CONSUME(0)), ==, 0);
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

    reg_write(to, 0x08, RX_RING_FULL(0) | RX_TIMER_EXPIRED(0));
    g_assert_false(qtest_get_irq(to, 0));
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

    reg_write(to, 0x08, RX_TIMER_EXPIRED(0));
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
    qtest_add_func("/sgi-ioc4/dma-tx-wrap-1k", test_dma_tx_wrap_1k);
    qtest_add_func("/sgi-ioc4/dma-tx-wrap-4k", test_dma_tx_wrap_4k);
    qtest_add_func("/sgi-ioc4/dma-rx-full-staged-recovery",
                   test_dma_rx_full_and_staged_recovery);
    qtest_add_func("/sgi-ioc4/dma-rx-timer-expiry", test_dma_rx_timer_expiry);
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
    qtest_add_func("/sgi-ioc4/uart-clock", test_uart_and_clock);
    qtest_add_func("/sgi-ioc4/interrupts", test_interrupts);
    qtest_add_func("/sgi-ioc4/pio-fifo", test_pio_fifo);
    qtest_add_func("/sgi-ioc4/xbridge-unaligned-split",
                   test_xbridge_unaligned_split);
    qtest_add_func("/sgi-ioc4/rtc-absent", test_rtc_absent);
    qtest_add_func("/sgi-ioc4/rtc-alias", test_rtc_alias);
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
