/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fuel boot-spike wiring. The board ID word and bank placement are inputs. */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-test.h"
#include "sgi-migrate.h"

#include <poll.h>
#include <sys/socket.h>

#define HUB_LOCAL_REGISTERS (SPACE_HUB + UINT64_C(0x10000000))
#define HUB_REMOTE_REGISTERS (SPACE_HUB + UINT64_C(0x30000000))
#define BOARD_ID_OFFSET UINT64_C(0x4000000)
#define BEDROCK_LOCAL (SPACE_XTALK + UINT64_C(0x01000000))
#define BEDROCK_PI1 (SPACE_XTALK + UINT64_C(0x01800000) + 0x200000)
#define PI_CPU_PRESENT_A 0x40
#define PI_CPU_PRESENT_B 0x48
#define PI_CPU_ENABLE_A 0x50
#define PI_CPU_ENABLE_B 0x58
#define XTALK_WIDGET(w) (SPACE_XTALK + (uint64_t)(w) * UINT64_C(0x1000000))
#define PCI_CONFIG(w, d) (XTALK_WIDGET(w) + 0x20000 + (d) * 0x1000)
#define BOARD_ID_HYPOTHESIS UINT64_C(0x4abc)
#define IOC3_BAR 0x10000000
#define FUEL_IOC3_ONEWIRE_SERIAL UINT64_C(0x5e7000000003)
#define IOC3_WINDOW (XTALK_WIDGET(15) + 0x800000)
#define IOC3_SERIAL_A 0x20178
#define IOC3_SERIAL_B 0x20170
#define IOC3_UART_LSR 5
#define IOC3_UART_SCR 7
#define IOC3_UART_LSR_DR 1
#define IOC3_INT_STATUS 0x1c
#define IOC3_INT_ENABLE_SET 0x20
#define IOC3_INT_KEYBOARD (1U << 22)
#define IOC3_KBC_CONTROL 0x9c
#define IOC3_KBC_KBD_RECEIVE 0xa0
#define IOC3_KBC_AUX_RECEIVE 0xa4
#define IOC3_KBC_KBD_TRANSMIT 0xa8
#define IOC3_KBC_AUX_TRANSMIT 0xac
#define IOC3_KBC_CLAMP_BOTH_3 0x00c00000
#define BEDROCK_MD_CONFIG (BEDROCK_LOCAL + 0x780008)
#define MEMORY_APERTURE UINT64_C(0)
#define MEMORY_BANK_STRIDE UINT64_C(0x40000000)
#define MEMORY_BANK_256M UINT64_C(0x10000000)
#define HUB_LOCAL_BOOT (SPACE_HUB + UINT64_C(0x18000000))
#define LB_REVISION (BEDROCK_LOCAL + 0x600000)

static bool fuel_cpu_readq(QTestState *qts, uint64_t address, uint64_t *value)
{
    g_autofree char *command = g_strdup_printf("xp/1gx 0x%" PRIx64, address);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute':'human-monitor-command',"
        " 'arguments':{'command-line':%s,'cpu-index':0}}", command);
    const char *result = qdict_get_str(response, "return");
    const char *colon;

    if (strstr(result, "Cannot access memory")) {
        return false;
    }
    colon = strchr(result, ':');
    g_assert_nonnull(colon);
    g_assert_cmpint(sscanf(colon + 1, "%" SCNx64, value), ==, 1);
    return true;
}

static char *fuel_test_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd = g_file_open_tmp("sn1-fuel-prom-XXXXXX.bin", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *fuel_start(const char *prom)
{
    g_autofree char *quoted = g_shell_quote(prom);

    return qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                       "fuel-ioc3-subsystem-id=0xc30a,"
                       "fuel-l1-type-code=0x61 "
                       "-cpu R14000 -smp 1 -m 512M -bios %s -S", quoted);
}

static void fuel_map_ioc3(QTestState *qts)
{
    qtest_writel(qts, PCI_CONFIG(15, 4) + 0x10, IOC3_BAR);
    qtest_writel(qts, PCI_CONFIG(15, 4) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, XTALK_WIDGET(15) + 0x200 + 4 * 8 + 4,
                 (1U << 12) | (IOC3_BAR >> 20));
}

static void test_fuel_ioc3_identity(const void *data)
{
    QTestState *qts = fuel_start(data);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{"
        "'path':'/machine/node0/baseio/ioc3',"
        "'property':'onewire-serial'}}");

    g_assert_false(qdict_haskey(response, "error"));
    g_assert_cmphex(qdict_get_int(response, "return"), ==,
                    FUEL_IOC3_ONEWIRE_SERIAL);
    qtest_quit(qts);
}

static void test_fuel_board_and_cpu(const void *data)
{
    const char *prom = data;
    QTestState *qts = fuel_start(prom);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + BOARD_ID_OFFSET),
                    ==, BOARD_ID_HYPOTHESIS);
    g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS + BOARD_ID_OFFSET),
                    ==, BOARD_ID_HYPOTHESIS);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_PRESENT_A), ==, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_PRESENT_B), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_PI1 + PI_CPU_PRESENT_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_PI1 + PI_CPU_PRESENT_B), ==, 0);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + BOARD_ID_OFFSET),
                    ==, BOARD_ID_HYPOTHESIS);
    qtest_quit(qts);
}

static void test_fuel_cpu_grant(const void *data)
{
    const char *prom = data;
    QTestState *qts = fuel_start(prom);

    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_A), ==, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_PI1 + PI_CPU_ENABLE_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_PI1 + PI_CPU_ENABLE_B), ==, 0);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_A, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_A), ==, 0);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_PI1 + PI_CPU_ENABLE_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_PI1 + PI_CPU_ENABLE_B), ==, 0);
    qtest_quit(qts);
}

static void test_fuel_pci(const void *data)
{
    const char *prom = data;
    QTestState *qts = fuel_start(prom);

    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(15, 1)), ==, 0x12161077);
    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(15, 4)), ==, 0x000310a9);
    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(15, 4) + 0x2c), ==,
                    0xc30a10a9);
    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(14, 1)), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(14, 4)), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(15, 5)), ==, 0x580211c1);
    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(15, 5) + 8) >> 8, ==,
                    0x0c0310);
    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(15, 5) + 0x100), ==,
                    0xffffffff);
    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(14, 5)), ==, 0xffffffff);
    qtest_writel(qts, PCI_CONFIG(15, 5) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(15, 5) + PCI_COMMAND) &
                    (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER), ==,
                    PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    fuel_map_ioc3(qts);
    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_A + 7, 0x5a);
    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_B + 7, 0xa5);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_A + 7), ==,
                    0x5a);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_B + 7), ==,
                    0xa5);
    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_A + 3, 0x1b);
    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_B + 3, 0x1f);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_A + 3), ==,
                    0x1b);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_B + 3), ==,
                    0x1f);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(15, 5)), ==, 0x580211c1);
    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(15, 5) + PCI_COMMAND) &
                    (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(15, 4) + PCI_COMMAND) &
                    PCI_COMMAND_MEMORY, ==, 0);
    fuel_map_ioc3(qts);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_A + 3), ==, 0);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_B + 3), ==, 0);
    qtest_quit(qts);
}

static void test_fuel_ps2(const void *data)
{
    const char *prom = data;
    QTestState *qts = fuel_start(prom);

    fuel_map_ioc3(qts);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_KBD_RECEIVE),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_AUX_RECEIVE),
                    ==, 0);

    qtest_writel(qts, IOC3_WINDOW + IOC3_KBC_CONTROL,
                 IOC3_KBC_CLAMP_BOTH_3);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_CONTROL), ==,
                    IOC3_KBC_CLAMP_BOTH_3);
    qtest_writel(qts, IOC3_WINDOW + IOC3_INT_ENABLE_SET,
                 IOC3_INT_KEYBOARD);
    qtest_writel(qts, IOC3_WINDOW + IOC3_KBC_KBD_TRANSMIT, 0xff);
    qtest_writel(qts, IOC3_WINDOW + IOC3_KBC_AUX_TRANSMIT, 0xff);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_INT_STATUS) &
                    IOC3_INT_KEYBOARD, ==, IOC3_INT_KEYBOARD);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_KBD_RECEIVE),
                    ==, 0xc0faaa00);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_AUX_RECEIVE),
                    ==, 0xe8faaa00);
    qtest_writel(qts, IOC3_WINDOW + IOC3_INT_STATUS, IOC3_INT_KEYBOARD);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_INT_STATUS) &
                    IOC3_INT_KEYBOARD, ==, 0);

    qtest_writel(qts, IOC3_WINDOW + IOC3_KBC_KBD_TRANSMIT, 0xff);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_INT_STATUS) &
                    IOC3_INT_KEYBOARD, ==, IOC3_INT_KEYBOARD);
    qtest_system_reset(qts);
    fuel_map_ioc3(qts);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_CONTROL), ==, 0);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_KBD_RECEIVE),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_AUX_RECEIVE),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_INT_STATUS) &
                    IOC3_INT_KEYBOARD, ==, 0);
    qtest_quit(qts);
}

static void test_fuel_memory(const void *data)
{
    const char *prom = data;
    QTestState *qts = fuel_start(prom);
    const uint64_t first = UINT64_C(0x0123456789abcdef);
    const uint64_t second = UINT64_C(0xfedcba9876543210);

    /* Each declared 256 MiB bank occupies one 1 GiB software bank. */
    qtest_writeq(qts, BEDROCK_MD_CONFIG, 0x33);
    qtest_writeq(qts, MEMORY_APERTURE + MEMORY_BANK_256M - 8, first);
    qtest_writeq(qts, MEMORY_APERTURE + MEMORY_BANK_STRIDE +
                      MEMORY_BANK_256M - 8, second);
    g_assert_cmphex(qtest_readq(qts, MEMORY_APERTURE +
                                MEMORY_BANK_256M - 8), ==, first);
    g_assert_cmphex(qtest_readq(qts, MEMORY_APERTURE + MEMORY_BANK_STRIDE +
                                MEMORY_BANK_256M - 8), ==, second);
    qtest_writeq(qts, MEMORY_APERTURE + 2 * MEMORY_BANK_256M, first);
    qtest_writeq(qts, MEMORY_APERTURE + MEMORY_BANK_STRIDE +
                      MEMORY_BANK_256M, second);
    g_assert_cmphex(qtest_readq(qts, MEMORY_APERTURE +
                                2 * MEMORY_BANK_256M),
                    !=, first);
    g_assert_cmphex(qtest_readq(qts, MEMORY_APERTURE + MEMORY_BANK_STRIDE +
                                MEMORY_BANK_256M), !=, second);
    qtest_writeq(qts, MEMORY_APERTURE + 2 * MEMORY_BANK_STRIDE, first);
    g_assert_cmphex(qtest_readq(qts, MEMORY_APERTURE +
                                2 * MEMORY_BANK_STRIDE), !=, first);
    qtest_quit(qts);
}

static void test_fuel_rejects_other_board_code(const void *data)
{
    const char *prom = data;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*bits 15:12 (0x4)*");
        return;
    }
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf("-M fuel,fuel-board-id-word=0x8abc,"
                                  "fuel-ioc3-subsystem-id=0xc30a,"
                                  "fuel-l1-type-code=0x61 "
                                  "-cpu R14000 -smp 1 -m 512M -bios %s -S",
                                  quoted);
    qtest_quit(qts);
}

static void test_fuel_requires_l1_type_code(const void *data)
{
    const char *prom = data;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*requires an explicit fuel-l1-type-code*");
        return;
    }
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                                  "fuel-ioc3-subsystem-id=0xc30a "
                                  "-cpu R14000 -smp 1 -m 512M -bios %s -S",
                                  quoted);
    qtest_quit(qts);
}

static void test_fuel_rejects_other_l1_type_code(const void *data)
{
    const char *prom = data;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*fuel-l1-type-code must be 0x34*");
        return;
    }
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                                  "fuel-ioc3-subsystem-id=0xc30a,"
                                  "fuel-l1-type-code=0x35 "
                                  "-cpu R14000 -smp 1 -m 512M -bios %s -S",
                                  quoted);
    qtest_quit(qts);
}

static void test_fuel_l1_type_code_rejects_other_machine(const void *data)
{
    const char *prom = data;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*applies only to -M fuel*");
        return;
    }
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf("-M origin300,fuel-l1-type-code=0x61 "
                                  "-cpu R14000 -smp 2 -m 512M -bios %s -S",
                                  quoted);
    qtest_quit(qts);
}

static void test_fuel_board_id_migration(const void *data)
{
    const char *prom = data;
    g_autofree char *quoted = g_shell_quote(prom);
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-fuel-migration-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = fuel_start(prom);
    to = qtest_initf("-M fuel,fuel-board-id-word=0x4def,"
                     "fuel-ioc3-subsystem-id=0xc30a,"
                     "fuel-l1-type-code=0x61 "
                     "-cpu R14000 -smp 1 -m 512M -bios %s -S -incoming defer",
                     quoted);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL_REGISTERS + BOARD_ID_OFFSET),
                    ==, 0x4def);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL_REGISTERS + BOARD_ID_OFFSET),
                    ==, BOARD_ID_HYPOTHESIS);
    g_assert_cmphex(qtest_readq(to, HUB_REMOTE_REGISTERS + BOARD_ID_OFFSET),
                    ==, BOARD_ID_HYPOTHESIS);
    qtest_quit(from);
    qtest_quit(to);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_fuel_prom_copy_boundary(const void *data)
{
    const char *prom = data;
    QTestState *qts = fuel_start(prom);
    uint64_t value;

    g_assert_cmphex((qtest_readq(qts, LB_REVISION) >> 32) & 0xff, ==, 0);
    g_assert_true(fuel_cpu_readq(qts, HUB_LOCAL_BOOT + PROM_SIZE - 8,
                                 &value));
    g_assert_cmphex(value, ==, 0);
    g_assert_false(fuel_cpu_readq(qts, HUB_LOCAL_BOOT + PROM_SIZE, &value));
    g_assert_cmphex((qtest_readq(qts, LB_REVISION) >> 32) & 0xff, ==, 0);
    qtest_quit(qts);
}

static void test_fuel_serial_backends(const void *data)
{
    const char *prom = data;
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf(
        "-M fuel,fuel-board-id-word=0x4abc,"
        "fuel-ioc3-subsystem-id=0xc30a,"
        "fuel-l1-type-code=0x61 "
        "-cpu R14000 -smp 1 -m 512M -bios %s -S "
        "-serial null -serial null -serial null "
        "-serial null -serial null", quoted);

    qtest_quit(qts);
}

static void fuel_expect_serial_byte(int fd, char expected)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    char actual;

    g_assert_cmpint(poll(&pfd, 1, 3000), ==, 1);
    g_assert_cmpint(read(fd, &actual, 1), ==, 1);
    g_assert_cmpint(actual, ==, expected);
}

static void fuel_expect_no_serial_byte(int fd)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    g_assert_cmpint(poll(&pfd, 1, 200), ==, 0);
}

static void fuel_wait_serial_input(QTestState *qts, uint64_t port)
{
    gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

    while (!(qtest_readb(qts, port + IOC3_UART_LSR) & IOC3_UART_LSR_DR)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void test_fuel_serial_bytes(const void *data)
{
    const char *prom = data;
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts;
    int a[2], b[2];

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, a), ==, 0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, b), ==, 0);
    qts = qtest_initf(
        "-M fuel,fuel-board-id-word=0x4abc,"
        "fuel-ioc3-subsystem-id=0xc30a,"
        "fuel-l1-type-code=0x61 "
        "-cpu R14000 -smp 1 -m 512M -bios %s -S "
        "-serial null "
        "-chardev socket,id=ioc3-a,fd=%d -serial chardev:ioc3-a "
        "-chardev socket,id=ioc3-b,fd=%d -serial chardev:ioc3-b",
        quoted, a[1], b[1]);
    g_assert_cmpint(close(a[1]), ==, 0);
    g_assert_cmpint(close(b[1]), ==, 0);
    fuel_map_ioc3(qts);

    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_A, 'A');
    fuel_expect_serial_byte(a[0], 'A');
    fuel_expect_no_serial_byte(b[0]);
    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_B, 'B');
    fuel_expect_serial_byte(b[0], 'B');
    fuel_expect_no_serial_byte(a[0]);

    g_assert_cmpint(write(a[0], "x", 1), ==, 1);
    fuel_wait_serial_input(qts, IOC3_WINDOW + IOC3_SERIAL_A);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_B +
                                IOC3_UART_LSR) & IOC3_UART_LSR_DR, ==, 0);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_A), ==, 'x');
    g_assert_cmpint(write(b[0], "y", 1), ==, 1);
    fuel_wait_serial_input(qts, IOC3_WINDOW + IOC3_SERIAL_B);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_A +
                                IOC3_UART_LSR) & IOC3_UART_LSR_DR, ==, 0);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_B), ==, 'y');

    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_A + IOC3_UART_SCR, 0x5a);
    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_B + IOC3_UART_SCR, 0xa5);
    qtest_system_reset(qts);
    fuel_map_ioc3(qts);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_A +
                                IOC3_UART_SCR), ==, 0);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_B +
                                IOC3_UART_SCR), ==, 0);

    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_A, 'C');
    fuel_expect_serial_byte(a[0], 'C');
    fuel_expect_no_serial_byte(b[0]);
    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_B, 'D');
    fuel_expect_serial_byte(b[0], 'D');
    fuel_expect_no_serial_byte(a[0]);

    g_assert_cmpint(write(a[0], "m", 1), ==, 1);
    fuel_wait_serial_input(qts, IOC3_WINDOW + IOC3_SERIAL_A);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_A), ==, 'm');
    g_assert_cmpint(write(b[0], "n", 1), ==, 1);
    fuel_wait_serial_input(qts, IOC3_WINDOW + IOC3_SERIAL_B);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_B), ==, 'n');

    qtest_quit(qts);
    g_assert_cmpint(close(a[0]), ==, 0);
    g_assert_cmpint(close(b[0]), ==, 0);
}

int main(int argc, char **argv)
{
    g_autofree char *prom = NULL;
    int status;

    g_test_init(&argc, &argv, NULL);
    prom = fuel_test_prom();
    qtest_add_data_func("fuel/board-and-cpu", prom, test_fuel_board_and_cpu);
    qtest_add_data_func("fuel/cpu-grant", prom, test_fuel_cpu_grant);
    qtest_add_data_func("fuel/board-id-migration", prom,
                        test_fuel_board_id_migration);
    qtest_add_data_func("fuel/pci", prom, test_fuel_pci);
    qtest_add_data_func("fuel/ioc3-identity", prom,
                        test_fuel_ioc3_identity);
    qtest_add_data_func("fuel/ps2", prom, test_fuel_ps2);
    qtest_add_data_func("fuel/memory", prom, test_fuel_memory);
    qtest_add_data_func("fuel/rejects-other-board-code", prom,
                        test_fuel_rejects_other_board_code);
    qtest_add_data_func("fuel/requires-l1-type-code", prom,
                        test_fuel_requires_l1_type_code);
    qtest_add_data_func("fuel/rejects-other-l1-type-code", prom,
                        test_fuel_rejects_other_l1_type_code);
    qtest_add_data_func("fuel/l1-type-code-rejects-other-machine", prom,
                        test_fuel_l1_type_code_rejects_other_machine);
    qtest_add_data_func("fuel/prom-copy-boundary", prom,
                        test_fuel_prom_copy_boundary);
    qtest_add_data_func("fuel/serial-backends", prom,
                        test_fuel_serial_backends);
    qtest_add_data_func("fuel/serial-bytes", prom, test_fuel_serial_bytes);
    status = g_test_run();
    g_assert_cmpint(g_unlink(prom), ==, 0);
    return status;
}
