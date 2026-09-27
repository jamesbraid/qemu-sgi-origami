/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics-free Octane profile: synthetic RAM and raw flash policy. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#include <sys/socket.h>

#define HEART 0x0ff00000ULL
#define XBOW 0x10000000ULL
#define HEART_WIDGET 0x18000000ULL
#define BRIDGE 0x1f000000ULL
#define OCTANE_LONG_BRIDGE (15ULL << 36)
#define FLASH (BRIDGE + 0xc00000)
#define RAM 0x20000000ULL
#define UNIT 0x02000000ULL
#define BANK0 0x80030000U
#define WIDE 0x80070000U
#define CFG(slot) (BRIDGE + 0x020000 + (slot) * 0x1000)
#define BRIDGE_NIC (BRIDGE + 0x0b4)
#define IOC3 (BRIDGE + 0x600000)
#define UART_A (IOC3 + 0x20178)
#define IOC3_SERIAL_IRQ (1u << 6)
#define BRIDGE_REG(offset) (BRIDGE + (offset) + 4)
#define SUPERIO_PIN (1u << 3)

static char *flash_path;

static QTestState *octane_start_args(const char *extra)
{
    g_autofree char *quoted = g_shell_quote(flash_path);

    return qtest_initf("-M octane -m 128M -cpu R10000 -bios %s -S %s",
                       quoted, extra);
}

static QTestState *octane_start(void)
{
    return octane_start_args("");
}

static QTestState *octane_start_serial(int *sock)
{
    g_autofree char *quoted = g_shell_quote(flash_path);
    g_autofree char *args = g_strdup_printf(
        "-M octane -m 128M -cpu R10000 -bios %s -S", quoted);

    return qtest_init_with_serial(args, sock);
}

static uint32_t octane_cpu_cause(QTestState *qts)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers");
    const char *field = strstr(registers, "Cause");
    uint32_t cause;

    g_assert_nonnull(field);
    g_assert_cmpint(sscanf(field, "Cause %" SCNx32, &cause), ==, 1);
    return cause;
}

static uint64_t octane_cpu_pc(QTestState *qts)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers");
    const char *field = strstr(registers, "pc=0x");
    uint64_t pc;

    g_assert_nonnull(field);
    g_assert_cmpint(sscanf(field, "pc=0x%" SCNx64, &pc), ==, 1);
    return pc;
}

static void test_widgets_and_memory(void)
{
    QTestState *qts = octane_start();
    uint32_t value;
    uint64_t missing;

    g_assert_cmphex(qtest_readl(qts, HEART + 0x20), ==, BANK0);
    g_assert_cmpuint(qtest_readl_tx(qts, XBOW + 4, &value), ==, MEMTX_OK);
    /* Crossbow does not route a packet back to its source widget. */
    g_assert_cmpuint(qtest_readl_tx(qts, HEART_WIDGET + 4, &value), !=,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_readl_tx(qts, BRIDGE + 4, &value), ==, MEMTX_OK);
    g_assert_cmpuint(qtest_readl_tx(qts, 0x92000004, &value), !=, MEMTX_OK);

    qtest_writeq(qts, RAM, 0x0123456789abcdefULL);
    qtest_writeq(qts, RAM + 4 * UNIT - 8, 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(qts, RAM), ==, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, RAM + 4 * UNIT - 8), ==,
                    0xfedcba9876543210ULL);
    g_assert_cmpuint(qtest_readq_tx(qts, RAM + 4 * UNIT, &missing), !=,
                     MEMTX_OK);

    qtest_writel(qts, HEART + 0x20, WIDE);
    g_assert_cmphex(qtest_readl(qts, HEART + 0x20), ==, WIDE);
    g_assert_cmphex(qtest_readq(qts, RAM + 4 * UNIT), ==,
                    0x0123456789abcdefULL);
    qtest_writel(qts, HEART + 0x20, BANK0);
    g_assert_cmpuint(qtest_readq_tx(qts, RAM + 4 * UNIT, &missing), !=,
                     MEMTX_OK);
    qtest_writel(qts, HEART + 0x20, 0);
    g_assert_cmphex(qtest_readl(qts, HEART + 0x20), ==, 0);
    g_assert_cmpuint(qtest_readq_tx(qts, RAM, &missing), !=, MEMTX_OK);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, HEART + 0x20), ==, BANK0);
    g_assert_cmphex(qtest_readq(qts, RAM), ==, 0x0123456789abcdefULL);
    qtest_quit(qts);
}

static void test_flash_and_pci(void)
{
    QTestState *qts = octane_start();
    uint32_t value;
    uint64_t last;
    g_autoptr(QDict) listing = qtest_qmp(qts,
        "{'execute':'qom-list','arguments':{'path':'/machine/bridge'}}");
    QList *children = qdict_get_qlist(listing, "return");

    for (const QListEntry *entry = qlist_first(children); entry;
         entry = qlist_next(entry)) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));
        g_assert_cmpstr(qdict_get_str(item, "name"), !=, "onewire-part");
    }

    g_assert_cmpuint(qtest_readl_tx(qts, FLASH, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0x12345678);
    g_assert_cmphex(qtest_readb(qts, FLASH), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, FLASH + 1), ==, 0x34);
    g_assert_cmphex(qtest_readb(qts, FLASH + 2), ==, 0x56);
    g_assert_cmphex(qtest_readb(qts, FLASH + 3), ==, 0x78);
    g_assert_cmphex(qtest_readb(qts, FLASH + 4), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, FLASH + 0x1fffff), ==, 0xff);
    /* Bridge drops writes while flash write enable is clear. */
    g_assert_cmpuint(qtest_writel_tx(qts, FLASH, 0), ==, MEMTX_OK);
    g_assert_cmpuint(qtest_readl_tx(qts, FLASH, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0x12345678);
    g_assert_cmphex(qtest_readb(qts, FLASH), ==, 0x12);
    g_assert_cmpuint(qtest_readq_tx(qts, FLASH + 0x200000, &last), !=,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readl(qts, CFG(0)), ==, 0x10201077);
    g_assert_cmphex(qtest_readl(qts, CFG(1)), ==, 0x10201077);
    g_assert_cmphex(qtest_readl(qts, CFG(2)), ==, 0x000310a9);
    g_assert_cmpuint(qtest_readl_tx(qts, CFG(3), &value), ==,
                     MEMTX_ERROR);
    g_assert_cmphex(value, ==, 0xffffffff);
    g_assert_cmpuint(qtest_readl_tx(qts, OCTANE_LONG_BRIDGE + 4, &value), ==,
                     MEMTX_OK);
    /* An empty one-wire bus has no device to pull a reset slot low. */
    qtest_writel(qts, BRIDGE_NIC, (520u << 10) | (65u << 2));
    g_assert_cmphex(qtest_readl(qts, BRIDGE_NIC) & 3, ==, 3);
    qtest_quit(qts);
}

static void test_firmware_rejections(void)
{
    const char *missing[] = {
        qtest_qemu_binary(NULL), "-M", "octane", "-m", "128M",
        "-bios", "/nonexistent/octane-prom", "-display", "none", NULL
    };
    g_autofree char *oversized = NULL;
    g_autofree char *error_output = NULL;
    int status;
    int fd = g_file_open_tmp("octane-oversized-XXXXXX", &oversized, NULL);
    const char *too_large[] = {
        qtest_qemu_binary(NULL), "-M", "octane", "-m", "128M",
        "-bios", oversized, "-display", "none", NULL
    };

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 2 * 1024 * 1024 + 1), ==, 0);
    close(fd);
    g_assert_true(g_spawn_sync(NULL, (char **)missing, NULL, 0, NULL, NULL,
                               NULL, &error_output, &status, NULL));
    g_assert_false(g_spawn_check_exit_status(status, NULL));
    g_assert_nonnull(strstr(error_output, "cannot read firmware image"));
    g_clear_pointer(&error_output, g_free);
    g_assert_true(g_spawn_sync(NULL, (char **)too_large, NULL, 0, NULL, NULL,
                               NULL, &error_output, &status, NULL));
    g_assert_false(g_spawn_check_exit_status(status, NULL));
    g_assert_nonnull(strstr(error_output, "exceeds 2 MiB"));
    g_assert_cmpint(unlink(oversized), ==, 0);
}

static void test_heart_interrupt_levels(void)
{
    QTestState *qts = octane_start();
    const uint64_t level0 = 1;
    const uint64_t level2 = 1ULL << 32;

    qtest_irq_intercept_out_named(qts, "/machine/heart", "processor");
    qtest_writeq(qts, HEART + 0x10020, level0 | level2);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030), ==,
                    level0 | level2);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    qtest_writeq(qts, HEART + 0x10000, level0 | level2);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_writeq(qts, HEART + 0x10028, level0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_writeq(qts, HEART + 0x10000, 0);
    g_assert_false(qtest_get_irq(qts, 1));
    qtest_quit(qts);
}

static void test_serial_a_loopback(void)
{
    QTestState *qts = octane_start();
    const uint64_t uart = BRIDGE + 0x600000 + 0x20178;

    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writeb(qts, uart + 4, 0x10);
    qtest_writeb(qts, uart, 0x5a);
    g_assert_cmphex(qtest_readb(qts, uart), ==, 0x5a);
    qtest_quit(qts);
}

static void test_serial_a_chardev(void)
{
    int sock;
    QTestState *qts = octane_start_serial(&sock);
    const uint8_t sent = 'I';
    uint8_t received;
    struct timeval deadline = { .tv_sec = 5 };

    g_assert_cmpint(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                               &deadline, sizeof(deadline)), ==, 0);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writeb(qts, UART_A + 3, 0x03);
    qtest_writeb(qts, UART_A + 1, 0x01);
    g_assert_cmpint(write(sock, &sent, 1), ==, 1);

    g_test_timer_start();
    while (!(qtest_readb(qts, UART_A + 5) & 1)) {
        g_assert_cmpfloat(g_test_timer_elapsed(), <, 5.0);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readl(qts, IOC3 + 0x1c) & IOC3_SERIAL_IRQ, ==,
                    IOC3_SERIAL_IRQ);
    qtest_writel(qts, IOC3 + 0x20, IOC3_SERIAL_IRQ);
    g_assert_cmphex(qtest_readl(qts, IOC3 + 0x20) & IOC3_SERIAL_IRQ, ==,
                    IOC3_SERIAL_IRQ);

    g_assert_cmphex(qtest_readb(qts, UART_A), ==, sent);
    g_assert_cmphex(qtest_readl(qts, IOC3 + 0x1c) & IOC3_SERIAL_IRQ, ==, 0);

    qtest_writeb(qts, UART_A, 'O');
    g_assert_cmpint(read(sock, &received, 1), ==, 1);
    g_assert_cmphex(received, ==, 'O');
    close(sock);
    qtest_quit(qts);
}

static void test_serial_a_interrupt_route(void)
{
    int sock;
    QTestState *qts = octane_start_serial(&sock);
    const uint8_t sent = 'R';
    struct timeval deadline = { .tv_sec = 5 };

    g_assert_cmpint(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                               &deadline, sizeof(deadline)), ==, 0);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writel(qts, BRIDGE_REG(0x030), 8u << 16);
    qtest_writel(qts, BRIDGE_REG(0x038), 0x80);
    qtest_writel(qts, BRIDGE_REG(0x130 + 8 * 3), 3);
    qtest_writel(qts, BRIDGE_REG(0x118), SUPERIO_PIN);
    qtest_writeb(qts, UART_A + 3, 0x03);
    qtest_writeb(qts, UART_A + 1, 0x01);
    g_assert_cmpint(write(sock, &sent, 1), ==, 1);

    g_test_timer_start();
    while (!(qtest_readb(qts, UART_A + 5) & 1)) {
        g_assert_cmpfloat(g_test_timer_elapsed(), <, 5.0);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readl(qts, IOC3 + 0x1c) & IOC3_SERIAL_IRQ, ==,
                    IOC3_SERIAL_IRQ);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & SUPERIO_PIN, ==, 0);
    qtest_writel(qts, IOC3 + 0x20, IOC3_SERIAL_IRQ);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & SUPERIO_PIN, ==,
                    SUPERIO_PIN);
    qtest_writel(qts, IOC3 + 0x24, IOC3_SERIAL_IRQ);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & SUPERIO_PIN, ==, 0);
    qtest_writel(qts, IOC3 + 0x20, IOC3_SERIAL_IRQ);
    qtest_writel(qts, BRIDGE_REG(0x108), SUPERIO_PIN);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030) & (1u << 3), ==,
                    1u << 3);
    g_assert_cmphex(octane_cpu_cause(qts) & 0x400, ==, 0);
    qtest_writeq(qts, HEART + 0x10000, 1u << 3);
    g_assert_cmphex(octane_cpu_cause(qts) & 0x400, ==, 0x400);

    g_assert_cmphex(qtest_readb(qts, UART_A), ==, sent);
    g_assert_cmphex(qtest_readl(qts, IOC3 + 0x1c) & IOC3_SERIAL_IRQ, ==, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & SUPERIO_PIN, ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030) & (1u << 3), ==, 0);
    g_assert_cmphex(octane_cpu_cause(qts) & 0x400, ==, 0);
    close(sock);
    qtest_quit(qts);
}

static void test_reset_state(void)
{
    QTestState *qts = octane_start();

    qtest_writel(qts, HEART + 0x20, WIDE);
    qtest_writeq(qts, HEART + 0x10000, 1u << 3);
    qtest_writeq(qts, HEART + 0x10020, 1u << 3);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writeb(qts, UART_A + 7, 0x5a);
    g_assert_cmphex(qtest_readb(qts, UART_A + 7), ==, 0x5a);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, HEART + 0x20), ==, BANK0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10000), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x108)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, CFG(2) + 0x10), ==, 0);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    g_assert_cmphex(qtest_readb(qts, UART_A + 7), ==, 0);
    g_assert_cmphex(qtest_readl(qts, FLASH), ==, 0x12345678);
    g_assert_cmphex(octane_cpu_pc(qts), ==, 0xffffffffbfc00000ULL);
    g_assert_cmphex(octane_cpu_cause(qts) & 0x400, ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int fd;
    const uint8_t bytes[] = { 0x12, 0x34, 0x56, 0x78 };

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("octane-flash-XXXXXX", &flash_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, bytes, sizeof(bytes)), ==, sizeof(bytes));
    close(fd);
    qtest_add_func("/octane/widgets-and-memory", test_widgets_and_memory);
    qtest_add_func("/octane/flash-and-pci", test_flash_and_pci);
    qtest_add_func("/octane/firmware-rejections", test_firmware_rejections);
    qtest_add_func("/octane/serial-a-loopback", test_serial_a_loopback);
    qtest_add_func("/octane/serial-a-chardev", test_serial_a_chardev);
    qtest_add_func("/octane/serial-a-interrupt-route",
                   test_serial_a_interrupt_route);
    qtest_add_func("/octane/heart-interrupt-levels",
                   test_heart_interrupt_levels);
    qtest_add_func("/octane/reset-state", test_reset_state);
    int result = g_test_run();
    unlink(flash_path);
    g_free(flash_path);
    return result;
}
