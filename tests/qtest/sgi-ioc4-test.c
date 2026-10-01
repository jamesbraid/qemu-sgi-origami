/*
 * IOC4 revision 0x53 serial register tests through a standalone XBridge.
 * This fixture tests the PCI function without assuming an IO9 board route.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-migrate.h"

#define CHIP "/machine/peripheral/xbridge"
#define WIDGET (UINT64_C(0x100000000000) + (UINT64_C(0xe) << 44))
#define CFG (WIDGET + 0x20000)
#define BAR (WIDGET + 0x200000)
#define PCI_ADDRESS 0x01000000u
#define UART(n) (BAR + 0x380 + 8 * (n))
#define RTC(n) (BAR + 0x87ff8 + (n))
#define PASS(n) (0x40u << (8 * (n)))

static void ioc4_map(QTestState *qts)
{
    qtest_writel(qts, CFG + 0x10, PCI_ADDRESS);
    qtest_writel(qts, CFG + 0x04, 0x00000002);
    qtest_writel(qts, WIDGET + 0x200 + 4, 0x83010);
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
    qtest_add_func("/sgi-ioc4/rtc-migration", test_rtc_migration);
    return g_test_run();
}
