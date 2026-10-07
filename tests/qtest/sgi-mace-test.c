/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register tests for the standalone MACE 2.0 peripheral block. */

#include "qemu/osdep.h"
#include <sys/socket.h>
#include <sys/un.h>
#include "libqtest.h"
#include "memtx.h"
#include "hw/mips/sgi/mace-timer.h"
#include "sgi-migrate.h"
#include "sgi-wait.h"

#define MACE_BASE       0x1f000000ULL
#define MACE_RING_BASE  (MACE_BASE + 0x310000)
#define MACE_PCI_ERROR_ADDR (MACE_BASE + 0x080000)
#define MACE_PCI_ERROR_FLAGS (MACE_BASE + 0x080004)
#define MACE_PCI_CONTROL (MACE_BASE + 0x080008)
#define MACE_PCI_REVISION (MACE_BASE + 0x08000c)
#define MACE_PCI_CFGADDR (MACE_BASE + 0x080cf8)
#define MACE_PCI_CFGDATA (MACE_BASE + 0x080cfc)
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

static QTestState *mace_start_uart_socket(const char *path,
                                          const char *extra)
{
    QTestState *s = qtest_initf("-M none -cpu R10000 "
        "-chardev socket,id=port-a,path=%s,server=on,wait=off "
        "-device sgi-mace,id=mace,chardev-a=port-a %s", path, extra);

    qtest_sysbus_mmio_map(s, "/machine/peripheral/mace", 0, MACE_BASE);
    qtest_irq_intercept_out_named(s, "/machine/peripheral/mace", "crime");
    return s;
}

static void test_uart_receive_after_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mace-uart-migrate-XXXXXX",
                                                &error);
    g_autofree char *source_path = NULL;
    g_autofree char *target_path = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    int source_fd, target_fd;
    uint8_t first = 'M', held = 'x', second = 'N';

    g_assert_no_error(error);
    source_path = g_build_filename(directory, "source.sock", NULL);
    target_path = g_build_filename(directory, "target.sock", NULL);
    migration_path = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", migration_path);
    from = mace_start_uart_socket(source_path, "");
    to = mace_start_uart_socket(target_path, "-incoming defer");
    source_fd = mace_connect_socket(source_path);
    target_fd = mace_connect_socket(target_path);

    qtest_writeq(from, MACE_RING_BASE, 0);
    qtest_writeq(from, MACE_IMASK, MACE_UART_A_INT);
    qtest_writeb(from, MACE_UART_A + 0x100, 0x01);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readq(to, MACE_RING_BASE) & 1, ==, 0);

    g_assert_cmpint(write(target_fd, &first, 1), ==, 1);
    sgi_wait_socket_read(target_fd);
    g_assert_cmphex(qtest_readb(to, MACE_UART_A + 0x500) & 1, ==, 1);
    g_assert_true(qtest_readq(to, MACE_ISTAT) & MACE_UART_A_INT);
    g_assert_true(qtest_get_irq(to, 4));
    g_assert_cmphex(qtest_readb(to, MACE_UART_A), ==, first);
    g_assert_false(qtest_get_irq(to, 4));

    qtest_writeq(to, MACE_RING_BASE, 1);
    g_assert_cmpint(write(target_fd, &held, 1), ==, 1);
    sgi_wait_socket_read(target_fd);
    qtest_writeq(to, MACE_RING_BASE, 0);
    qtest_writeb(to, MACE_UART_A + 0x100, 0x01);
    g_assert_cmphex(qtest_readb(to, MACE_UART_A + 0x500) & 1, ==, 0);
    g_assert_false(qtest_readq(to, MACE_ISTAT) & MACE_UART_A_INT);
    g_assert_false(qtest_get_irq(to, 4));

    g_assert_cmpint(write(target_fd, &second, 1), ==, 1);
    sgi_wait_socket_read(target_fd);
    g_assert_cmphex(qtest_readb(to, MACE_UART_A), ==, second);
    close(source_fd);
    close(target_fd);
    qtest_quit(to);
    qtest_quit(from);
    if (g_file_test(source_path, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(source_path), ==, 0);
    }
    if (g_file_test(target_path, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(target_path), ==, 0);
    }
    if (g_file_test(migration_path, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(migration_path), ==, 0);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static QTestState *mace_start_pci_card(void)
{
    QTestState *s = qtest_init(MEMTX_TEST_DEVICE "-M none -cpu R10000 "
        "-device sgi-mace,id=mace "
        "-device lsi53c895a,bus=mace.0,addr=1");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/mace", 0, MACE_BASE);
    qtest_irq_intercept_out_named(s, "/machine/peripheral/mace", "crime");
    return s;
}

static void test_pci_host_reset(void)
{
    QTestState *s = mace_start();

    g_assert_cmphex(qtest_readl(s, MACE_PCI_REVISION), ==, 1);
    g_assert_cmphex(qtest_readl(s, MACE_PCI_CONTROL), ==, 0);
    g_assert_cmphex(qtest_readl(s, MACE_PCI_ERROR_ADDR), ==, 0);
    g_assert_cmphex(qtest_readl(s, MACE_BASE + 0x080020), ==, 0xffffffff);
    g_assert_cmphex(qtest_readw(s, MACE_BASE + 0x080022), ==, 0xffff);
    g_assert_cmphex(qtest_readb(s, MACE_BASE + 0x080021), ==, 0xff);
    qtest_writeb(s, MACE_BASE + 0x080021, 0x12);
    g_assert_cmphex(qtest_readb(s, MACE_BASE + 0x080021), ==, 0xff);
    qtest_writel(s, MACE_PCI_CONTROL, 3);
    g_assert_cmphex(qtest_readl(s, MACE_PCI_CONTROL) & 3, ==, 3);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(s, MACE_PCI_CONTROL), ==, 0);
    qtest_quit(s);
}

static void test_pci_config_lanes(void)
{
    QTestState *s = mace_start_pci_card();

    qtest_writel(s, MACE_PCI_CFGADDR, 1u << 11);
    g_assert_cmphex(qtest_readl(s, MACE_PCI_CFGDATA), ==, 0x00121000);
    g_assert_cmphex(qtest_readw(s, MACE_PCI_CFGDATA + 2), ==, 0x1000);
    g_assert_cmphex(qtest_readw(s, MACE_PCI_CFGDATA), ==, 0x0012);
    g_assert_cmphex(qtest_readb(s, MACE_PCI_CFGDATA + 2), ==, 0x10);
    g_assert_cmphex(qtest_readb(s, MACE_PCI_CFGDATA + 1), ==, 0x12);

    qtest_writel(s, MACE_PCI_CFGADDR, (1u << 11) | 0x0c);
    qtest_writeb(s, MACE_PCI_CFGDATA + 3, 0x20);
    g_assert_cmphex(qtest_readb(s, MACE_PCI_CFGDATA + 3), ==, 0x20);
    qtest_writel(s, MACE_PCI_CFGADDR, (1u << 11) | 0x04);
    qtest_writew(s, MACE_PCI_CFGDATA + 2, 0x0100);
    g_assert_cmphex(qtest_readw(s, MACE_PCI_CFGDATA + 2) & 0x0107, ==,
                    0x0100);
    qtest_quit(s);
}

static void test_pci_bar_decode_refused(void)
{
    QTestState *s = mace_start_pci_card();

    qtest_writel(s, MACE_PCI_CFGADDR, (1u << 11) | 0x04);
    g_assert_cmpuint(memtx_writew(s, MACE_PCI_CFGDATA + 2, 0x0003), ==,
                     MEMTX_ERROR);
    g_assert_cmphex(qtest_readw(s, MACE_PCI_CFGDATA + 2) & 3, ==, 0);
    g_assert_cmpuint(memtx_writeb(s, MACE_PCI_CFGDATA + 3, 0x04), ==,
                     MEMTX_ERROR);
    g_assert_cmphex(qtest_readw(s, MACE_PCI_CFGDATA + 2) & 7, ==, 0);

    qtest_writel(s, MACE_PCI_CFGADDR, (1u << 11) | 0x10);
    g_assert_cmpuint(memtx_writel(s, MACE_PCI_CFGDATA, 0x80000000), ==,
                     MEMTX_ERROR);
    qtest_quit(s);
}

static void test_pci_absent_device(void)
{
    QTestState *s = mace_start();
    uint32_t flags;

    qtest_writel(s, MACE_PCI_CFGADDR, 5u << 11);
    g_assert_cmphex(qtest_readl(s, MACE_PCI_CFGDATA), ==, 0xffffffff);
    flags = qtest_readl(s, MACE_PCI_ERROR_FLAGS);
    g_assert_cmphex(flags & ((1u << 31) | (1u << 20) | (1u << 19)), ==,
                    (1u << 31) | (1u << 20) | (1u << 19));
    qtest_writel(s, MACE_PCI_ERROR_FLAGS, 0);
    flags = qtest_readl(s, MACE_PCI_ERROR_FLAGS);
    g_assert_false(flags & ((1u << 31) | (1u << 19)));
    qtest_quit(s);
}

static void test_pci_interrupt_inputs(void)
{
    QTestState *s = mace_start();
    const char *path = "/machine/peripheral/mace";

    qtest_set_irq_in(s, path, "pci", 0, 1);
    g_assert_false(qtest_get_irq(s, 8));
    qtest_writel(s, MACE_PCI_CONTROL, 1);
    g_assert_true(qtest_get_irq(s, 8));
    g_assert_false(qtest_get_irq(s, 9));
    qtest_set_irq_in(s, path, "pci", 1, 1);
    g_assert_false(qtest_get_irq(s, 9));
    qtest_writel(s, MACE_PCI_CONTROL, 3);
    g_assert_true(qtest_get_irq(s, 9));
    qtest_writel(s, MACE_PCI_CONTROL, 2);
    g_assert_false(qtest_get_irq(s, 8));
    g_assert_true(qtest_get_irq(s, 9));
    qtest_set_irq_in(s, path, "pci", 1, 0);
    g_assert_false(qtest_get_irq(s, 9));
    qtest_quit(s);
}

static void test_pci_state_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mace-pci-migrate-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    const char *path = "/machine/peripheral/mace";
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = mace_start();
    to = mace_start_extra("-incoming defer");
    qtest_writel(from, MACE_PCI_CONTROL, 0x80000001);
    qtest_set_irq_in(from, path, "pci", 0, 1);
    qtest_writel(from, MACE_PCI_CFGADDR, 5u << 11);
    g_assert_cmphex(qtest_readl(from, MACE_PCI_CFGDATA), ==, 0xffffffff);
    g_assert_true(qtest_get_irq(from, 7));
    g_assert_true(qtest_get_irq(from, 8));

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readl(to, MACE_PCI_CONTROL), ==, 0x80000001);
    g_assert_cmphex(qtest_readl(to, MACE_PCI_ERROR_ADDR), ==, 5u << 11);
    g_assert_true(qtest_readl(to, MACE_PCI_ERROR_FLAGS) & (1u << 31));
    g_assert_true(qtest_get_irq(to, 7));
    g_assert_true(qtest_get_irq(to, 8));
    qtest_writel(to, MACE_PCI_ERROR_FLAGS, 0);
    qtest_set_irq_in(to, path, "pci", 0, 0);
    g_assert_false(qtest_get_irq(to, 7));
    g_assert_false(qtest_get_irq(to, 8));

    qtest_quit(to);
    qtest_quit(from);
    if (g_file_test(socket, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(socket), ==, 0);
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
    qtest_add_func("/sgi/mace/uart-receive-after-migration",
                   test_uart_receive_after_migration);
    qtest_add_func("/sgi/mace/pci-host-reset", test_pci_host_reset);
    qtest_add_func("/sgi/mace/pci-config-lanes", test_pci_config_lanes);
    qtest_add_func("/sgi/mace/pci-bar-decode-refused",
                   test_pci_bar_decode_refused);
    qtest_add_func("/sgi/mace/pci-absent-device", test_pci_absent_device);
    qtest_add_func("/sgi/mace/pci-interrupt-inputs",
                   test_pci_interrupt_inputs);
    qtest_add_func("/sgi/mace/pci-state-migration",
                   test_pci_state_migration);
    return g_test_run();
}
