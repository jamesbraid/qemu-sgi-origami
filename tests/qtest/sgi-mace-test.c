/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register tests for the standalone MACE 2.0 peripheral block. */

#include "qemu/osdep.h"
#include <sys/socket.h>
#include <sys/un.h>
#include "libqtest.h"
#include "hw/mips/sgi/mace-timer.h"
#include "sgi-migrate.h"
#include "sgi-wait.h"

#define MACE_BASE       0x1f000000ULL
#define MACE_RING_BASE  (MACE_BASE + 0x310000)
#define MACE_ISTAT      (MACE_BASE + 0x310010)
#define MACE_IMASK      (MACE_BASE + 0x310018)
#define MACE_UST        (MACE_BASE + 0x340000)
#define MACE_COMPARE0   (MACE_BASE + 0x340008)
#define MACE_COMPARE0_ALIAS (MACE_BASE + 0x350020)
#define MACE_TIMER0     (1ULL << 13)
#define MACE_RTC_INT    (1ULL << 8)
#define MACE_UART_A_INT (1ULL << 20)
#define MACE_UART_B_INT (1ULL << 26)
#define MACE_UART_A     (MACE_BASE + 0x390000)
#define MACE_UART_B     (MACE_BASE + 0x398000)
#define MACE_RTC        (MACE_BASE + 0x3a0000)

static QTestState *mace_start_extra(const char *extra)
{
    QTestState *s = qtest_initf("-M none -cpu R10000 "
                                "-device sgi-mace,id=mace %s", extra);

    qtest_sysbus_mmio_map(s, "/machine/peripheral/mace", 0, MACE_BASE);
    qtest_irq_intercept_out_named(s, "/machine/peripheral/mace", "crime");
    return s;
}

static QTestState *mace_start(void)
{
    return mace_start_extra("");
}

static void test_compare_mask_and_ack(void)
{
    QTestState *s = mace_start();
    uint32_t now = qtest_readq(s, MACE_UST);

    qtest_writeq(s, MACE_IMASK, MACE_TIMER0);
    qtest_writeq(s, MACE_COMPARE0, (uint32_t)(now + 3));
    g_assert_false(qtest_get_irq(s, 5));

    qtest_clock_step(s, 2 * 960);
    g_assert_false(qtest_readq(s, MACE_ISTAT) & MACE_TIMER0);
    qtest_clock_step(s, 960);
    g_assert_true(qtest_readq(s, MACE_ISTAT) & MACE_TIMER0);
    g_assert_true(qtest_get_irq(s, 5));

    qtest_writeq(s, MACE_IMASK, 0);
    g_assert_true(qtest_readq(s, MACE_ISTAT) & MACE_TIMER0);
    g_assert_false(qtest_get_irq(s, 5));
    qtest_writeq(s, MACE_IMASK, MACE_TIMER0);
    g_assert_true(qtest_get_irq(s, 5));

    qtest_writeq(s, MACE_COMPARE0_ALIAS, (uint32_t)(now + 100));
    g_assert_cmphex(qtest_readq(s, MACE_COMPARE0), ==, (uint32_t)(now + 100));
    g_assert_false(qtest_readq(s, MACE_ISTAT) & MACE_TIMER0);
    g_assert_false(qtest_get_irq(s, 5));
    qtest_clock_step(s, 10 * 960);
    g_assert_false(qtest_readq(s, MACE_ISTAT) & MACE_TIMER0);
    qtest_quit(s);
}

static void test_late_compare_predicate(void)
{
    g_assert_false(sgi_mace_compare_reached(3, 2));
    g_assert_true(sgi_mace_compare_reached(3, 10));
    g_assert_false(sgi_mace_compare_reached(0x100000001ULL,
                                            0x100000000ULL));
    g_assert_true(sgi_mace_compare_reached(0x100000001ULL,
                                           0x100000005ULL));
    g_assert_false(sgi_mace_compare_reached(UINT64_MAX, UINT64_MAX));
    g_assert_cmpuint(sgi_mace_compare_due_tick(0xfffffffeULL, 1), ==,
                     0x100000001ULL);
    g_assert_cmpuint(sgi_mace_compare_due_tick(10, 100), ==, 100);
    g_assert_false(sgi_mace_compare_reached(
        sgi_mace_compare_due_tick(10, 100), 10));
    g_assert_cmpuint(sgi_mace_compare_due_tick(10, 10), ==, UINT64_MAX);
}

static void test_compare_overshoot(void)
{
    QTestState *s = mace_start();
    uint32_t now = qtest_readq(s, MACE_UST);

    qtest_writeq(s, MACE_IMASK, MACE_TIMER0);
    qtest_writeq(s, MACE_COMPARE0, (uint32_t)(now + 3));
    qtest_clock_step(s, 10 * 960);
    g_assert_true(qtest_readq(s, MACE_ISTAT) & MACE_TIMER0);
    g_assert_true(qtest_get_irq(s, 5));
    qtest_quit(s);
}

static void test_external_reset_control(void)
{
    QTestState *s = mace_start();

    g_assert_cmphex(qtest_readq(s, MACE_RING_BASE) & 1, ==, 1);
    qtest_writeq(s, MACE_RING_BASE, 0);
    g_assert_cmphex(qtest_readq(s, MACE_RING_BASE) & 1, ==, 0);
    qtest_quit(s);
}

static void test_rtc_direct_stride(void)
{
    QTestState *s = mace_start();

    qtest_writeb(s, MACE_RTC + 0x2000, 0x5a);
    qtest_writeb(s, MACE_RTC + 0x2107, 0x7c);
    g_assert_cmphex(qtest_readb(s, MACE_RTC + 0x2007), ==, 0x5a);
    g_assert_cmphex(qtest_readb(s, MACE_RTC + 0x2100), ==, 0x7c);
    qtest_quit(s);
}

static void test_uart_register_spacing(void)
{
    QTestState *s = mace_start();

    qtest_writeq(s, MACE_RING_BASE, 0);
    qtest_writeb(s, MACE_UART_A + 0x700, 0x5a);
    qtest_writeb(s, MACE_UART_B + 0x707, 0x7c);
    g_assert_cmphex(qtest_readb(s, MACE_UART_A + 0x707), ==, 0x5a);
    g_assert_cmphex(qtest_readb(s, MACE_UART_B + 0x700), ==, 0x7c);
    qtest_quit(s);
}

static void test_uart_interrupt_routes(void)
{
    QTestState *s = mace_start();

    qtest_writeq(s, MACE_RING_BASE, 0);
    qtest_writeq(s, MACE_IMASK, MACE_UART_A_INT | MACE_UART_B_INT);
    qtest_writeb(s, MACE_UART_A + 0x100, 0x02);
    g_assert_true(qtest_readq(s, MACE_ISTAT) & MACE_UART_A_INT);
    g_assert_false(qtest_readq(s, MACE_ISTAT) & MACE_UART_B_INT);
    g_assert_true(qtest_get_irq(s, 4));

    qtest_writeb(s, MACE_UART_B + 0x100, 0x02);
    qtest_writeb(s, MACE_UART_A + 0x100, 0);
    g_assert_false(qtest_readq(s, MACE_ISTAT) & MACE_UART_A_INT);
    g_assert_true(qtest_readq(s, MACE_ISTAT) & MACE_UART_B_INT);
    g_assert_true(qtest_get_irq(s, 4));

    qtest_writeb(s, MACE_UART_B + 0x100, 0);
    g_assert_false(qtest_get_irq(s, 4));
    qtest_quit(s);
}

static void test_rtc_periodic_interrupt(void)
{
    QTestState *s = mace_start();

    qtest_writeq(s, MACE_IMASK, MACE_RTC_INT);
    qtest_writeb(s, MACE_RTC + 0x0a00, 0x26);
    qtest_writeb(s, MACE_RTC + 0x0b00, 0x46);
    qtest_clock_step(s, 1000000);
    g_assert_true(qtest_readq(s, MACE_ISTAT) & MACE_RTC_INT);
    g_assert_true(qtest_get_irq(s, 5));

    g_assert_true(qtest_readb(s, MACE_RTC + 0x0c00) & 0x40);
    g_assert_false(qtest_readq(s, MACE_ISTAT) & MACE_RTC_INT);
    g_assert_false(qtest_get_irq(s, 5));
    qtest_quit(s);
}

static void test_migrate_pending_compare(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mace-migration-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = mace_start();
    to = mace_start_extra("-incoming defer");

    qtest_writeq(from, MACE_RING_BASE, 0);
    qtest_writeb(from, MACE_RTC + 0x2000, 0x5a);
    qtest_writeb(from, MACE_UART_A + 0x700, 0x6b);
    qtest_writeq(from, MACE_IMASK, MACE_TIMER0 | MACE_UART_A_INT);
    qtest_writeb(from, MACE_UART_A + 0x100, 0x02);
    qtest_writeq(from, MACE_COMPARE0, 10);
    qtest_clock_step(from, 5 * 960);
    g_assert_false(qtest_readq(from, MACE_ISTAT) & MACE_TIMER0);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");

    g_assert_cmphex(qtest_readq(to, MACE_RING_BASE) & 1, ==, 0);
    g_assert_cmphex(qtest_readb(to, MACE_RTC + 0x2007), ==, 0x5a);
    g_assert_cmphex(qtest_readb(to, MACE_UART_A + 0x707), ==, 0x6b);
    g_assert_cmphex(qtest_readq(to, MACE_UST), ==, 5);
    g_assert_true(qtest_readq(to, MACE_ISTAT) & MACE_UART_A_INT);
    g_assert_true(qtest_get_irq(to, 4));
    g_assert_false(qtest_get_irq(to, 5));
    qtest_clock_step(to, 5 * 960);
    g_assert_true(qtest_readq(to, MACE_ISTAT) & MACE_TIMER0);
    g_assert_true(qtest_get_irq(to, 5));

    qtest_quit(to);
    qtest_quit(from);
    g_unlink(socket);
    g_rmdir(directory);
}

static void test_reset_after_pending_sources(void)
{
    QTestState *s = mace_start();
    uint32_t now = qtest_readq(s, MACE_UST);

    qtest_writeq(s, MACE_RING_BASE, 0);
    qtest_writeq(s, MACE_IMASK, MACE_TIMER0 | MACE_UART_A_INT);
    qtest_writeb(s, MACE_UART_A + 0x100, 0x02);
    qtest_writeq(s, MACE_COMPARE0, (uint32_t)(now + 1));
    qtest_clock_step(s, 960);
    g_assert_true(qtest_get_irq(s, 4));
    g_assert_true(qtest_get_irq(s, 5));

    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(s, MACE_RING_BASE) & 1, ==, 1);
    g_assert_cmphex(qtest_readq(s, MACE_IMASK), ==, 0);
    g_assert_cmphex(qtest_readq(s, MACE_ISTAT), ==, 0);
    g_assert_false(qtest_get_irq(s, 4));
    g_assert_false(qtest_get_irq(s, 5));
    qtest_quit(s);
}

static int mace_connect_socket(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpuint(strlen(path), <, sizeof(addr.sun_path));
    g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
    g_assert_cmpint(connect(fd, (struct sockaddr *)&addr, sizeof(addr)),
                    ==, 0);
    return fd;
}

static void test_uart_receive_backends(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mace-uart-XXXXXX",
                                                &error);
    g_autofree char *path_a = NULL;
    g_autofree char *path_b = NULL;
    QTestState *s;
    int fd_a, fd_b;
    uint8_t byte_a = 'A', byte_b = 'B';
    uint8_t held_a = 'x', held_b = 'y';

    g_assert_no_error(error);
    path_a = g_build_filename(directory, "a.sock", NULL);
    path_b = g_build_filename(directory, "b.sock", NULL);
    s = qtest_initf("-M none -cpu R10000 "
        "-chardev socket,id=port-a,path=%s,server=on,wait=off "
        "-chardev socket,id=port-b,path=%s,server=on,wait=off "
        "-device sgi-mace,id=mace,chardev-a=port-a,chardev-b=port-b",
        path_a, path_b);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/mace", 0, MACE_BASE);
    qtest_irq_intercept_out_named(s, "/machine/peripheral/mace", "crime");
    fd_a = mace_connect_socket(path_a);
    fd_b = mace_connect_socket(path_b);

    g_assert_cmphex(qtest_readq(s, MACE_RING_BASE) & 1, ==, 1);
    g_assert_cmpint(write(fd_a, &held_a, 1), ==, 1);
    g_assert_cmpint(write(fd_b, &held_b, 1), ==, 1);
    sgi_wait_socket_read(fd_a);
    sgi_wait_socket_read(fd_b);
    qtest_writeq(s, MACE_RING_BASE, 0);
    qtest_writeq(s, MACE_IMASK, MACE_UART_A_INT | MACE_UART_B_INT);
    qtest_writeb(s, MACE_UART_A + 0x100, 0x01);
    qtest_writeb(s, MACE_UART_B + 0x100, 0x01);
    g_assert_cmphex(qtest_readb(s, MACE_UART_A + 0x500) & 1, ==, 0);
    g_assert_cmphex(qtest_readb(s, MACE_UART_B + 0x500) & 1, ==, 0);
    g_assert_cmphex(qtest_readq(s, MACE_ISTAT) &
                    (MACE_UART_A_INT | MACE_UART_B_INT), ==, 0);
    g_assert_false(qtest_get_irq(s, 4));

    g_assert_cmpint(write(fd_a, &byte_a, 1), ==, 1);
    g_assert_cmpint(write(fd_b, &byte_b, 1), ==, 1);
    sgi_wait_socket_read(fd_a);
    sgi_wait_socket_read(fd_b);

    g_assert_cmphex(qtest_readb(s, MACE_UART_A + 0x500) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(s, MACE_UART_B + 0x500) & 1, ==, 1);
    g_assert_cmphex(qtest_readq(s, MACE_ISTAT) &
                    (MACE_UART_A_INT | MACE_UART_B_INT), ==,
                    MACE_UART_A_INT | MACE_UART_B_INT);
    g_assert_true(qtest_get_irq(s, 4));
    g_assert_cmphex(qtest_readb(s, MACE_UART_A), ==, byte_a);
    g_assert_false(qtest_readq(s, MACE_ISTAT) & MACE_UART_A_INT);
    g_assert_true(qtest_readq(s, MACE_ISTAT) & MACE_UART_B_INT);
    g_assert_true(qtest_get_irq(s, 4));
    g_assert_cmphex(qtest_readb(s, MACE_UART_B), ==, byte_b);
    g_assert_false(qtest_readq(s, MACE_ISTAT) & MACE_UART_B_INT);
    g_assert_false(qtest_get_irq(s, 4));

    close(fd_a);
    close(fd_b);
    qtest_quit(s);
    if (g_file_test(path_a, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(path_a), ==, 0);
    }
    if (g_file_test(path_b, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(path_b), ==, 0);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/mace/compare-mask-ack", test_compare_mask_and_ack);
    qtest_add_func("/sgi/mace/compare-late-callback",
                   test_late_compare_predicate);
    qtest_add_func("/sgi/mace/compare-overshoot", test_compare_overshoot);
    qtest_add_func("/sgi/mace/external-reset", test_external_reset_control);
    qtest_add_func("/sgi/mace/rtc-direct-stride", test_rtc_direct_stride);
    qtest_add_func("/sgi/mace/uart-register-spacing",
                   test_uart_register_spacing);
    qtest_add_func("/sgi/mace/uart-interrupt-routes",
                   test_uart_interrupt_routes);
    qtest_add_func("/sgi/mace/rtc-periodic-interrupt",
                   test_rtc_periodic_interrupt);
    qtest_add_func("/sgi/mace/migrate-pending-compare",
                   test_migrate_pending_compare);
    qtest_add_func("/sgi/mace/reset-after-pending-sources",
                   test_reset_after_pending_sources);
    qtest_add_func("/sgi/mace/uart-receive-backends",
                   test_uart_receive_backends);
    return g_test_run();
}
