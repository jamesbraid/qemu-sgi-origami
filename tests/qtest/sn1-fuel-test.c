/* SPDX-License-Identifier: BSD-3-Clause */
/* Fuel boot-spike wiring. The board ID word and bank placement are inputs. */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-test.h"
#include "sgi-migrate.h"

#include <poll.h>
#include <sys/socket.h>

#define HUB_LOCAL_REGISTERS (SPACE_HUB + UINT64_C(0x10000000))
#define HUB_REMOTE_REGISTERS (SPACE_HUB + UINT64_C(0x30000000))
#define BOARD_ID_OFFSET UINT64_C(0x4000000)
#define BEDROCK_LOCAL (SPACE_IO + UINT64_C(0x01000000))
#define BEDROCK_PI1 (SPACE_IO + UINT64_C(0x01800000) + 0x200000)
#define PI_CPU_PRESENT_A 0x40
#define PI_CPU_PRESENT_B 0x48
#define PI_CPU_ENABLE_A 0x50
#define PI_CPU_ENABLE_B 0x58
#define XTALK_WIDGET(w) (SPACE_IO + (uint64_t)(w) * UINT64_C(0x1000000))
#define PCI_CONFIG(w, d) (XTALK_WIDGET(w) + 0x20000 + (d) * 0x1000)
#define BOARD_ID_HYPOTHESIS UINT64_C(0x4abc)
#define IOC3_BAR 0x10000000
#define FUEL_IOC3_ONEWIRE_SERIAL UINT64_C(0x5e7000000003)
#define IOC3_WINDOW (XTALK_WIDGET(15) + 0x800000)
#define FUEL_RTC (IOC3_WINDOW + 0x80000)
#define NS_PER_SEC 1000000000
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
#define II_WIDGET_ID (BEDROCK_LOCAL + 0x400000)
#define BEDROCK_REVISION(word) (((word) >> 28) & 0xf)
#define FUEL_XBRIDGE 15
#define XBRIDGE_HOST_PORT 0xa
#define XBRIDGE_INT_STATUS 0x100
#define XBRIDGE_INT_ENABLE 0x108
#define XBRIDGE_INT_UPPER 0x30
#define XBRIDGE_INT_LOWER 0x38
#define XBRIDGE_INT_HOST(pin) (0x130 + (pin) * 8)
#define XBRIDGE_DEVICE(slot) (0x200 + (slot) * 8)
#define XBRIDGE_DEVICE_MEMORY (1U << 12)
#define PI_PENDING_MODIFY 0x90
#define PI_PENDING_0 0x98
#define PI_MASK_0_A 0xa8
#define PI_REMOTE_WINDOW 0x1800000
#define MIPS_CAUSE_HARDWARE_IRQ0 (1U << 10)
#define SCSI_BAR 0x10000000
#define SCSI_WINDOW (XTALK_WIDGET(FUEL_XBRIDGE) + 0x400000)
#define ISP_BUS_CTRL 0x08
#define ISP_SEMAPHORE 0x0c
#define ISP_MAILBOX0 0x70
#define ISP_HOST_CMD 0xc0
#define IOC3_INT_SERIAL_A (1U << 6)
#define IOC3_ENET_INT_STATUS 0x0f4
#define IOC3_ENET_INT_ENABLE 0x0f8
#define IOC3_ENET_INT_RX_TIMER (1U << 0)
#define IOC3_ENET_MAC_CONTROL 0x0f0
#define IOC3_ENET_RX_CONTROL 0x0fc
#define IOC3_ENET_RX_RING_HI 0x100
#define IOC3_ENET_RX_RING_LO 0x104
#define IOC3_ENET_RX_CONSUME 0x10c
#define IOC3_ENET_RX_PRODUCE 0x110
#define IOC3_ENET_RX_TIMER 0x114
#define IOC3_ENET_MCR_PROMISC (1U << 1)
#define IOC3_ENET_MCR_RX_DMA_EN (1U << 15)
#define IOC3_ENET_MCR_RX_EN (1U << 16)
#define IOC3_ENET_MCR_RX_OFFSET(halfwords) ((uint32_t)(halfwords) << 3)
#define IOC3_ENET_RX_PRODUCE_ARM (1U << 31)
#define IOC3_ENET_RX_HEADER_VALID (1U << 31)
#define IOC3_ENET_MII_CONTROL 0x148
#define IOC3_ENET_MII_DATA 0x14c
#define IOC3_ENET_MII_WRITE_DATA 0x150
#define IOC3_ENET_MII_READ (1U << 10)
#define IOC3_RX_RING_PHYS 0x200000
#define IOC3_RX_BUFFER_PHYS 0x201000
#define IOC3_RX_RING_IOVA 0x40000000
#define IOC3_RX_BUFFER_IOVA 0x40001000
#define OHCI_BAR 0x10000000
#define OHCI_WINDOW (XTALK_WIDGET(FUEL_XBRIDGE) + 0x900000)
#define OHCI_CONTROL 0x04
#define OHCI_INTERRUPT_STATUS 0x0c
#define OHCI_INTERRUPT_ENABLE 0x10
#define OHCI_HCCA 0x18
#define OHCI_FM_INTERVAL 0x34
#define OHCI_FM_REMAINING 0x38
#define OHCI_CONTROL_OPERATIONAL 0x80
#define OHCI_CONTROL_SUSPEND 0xc0
#define OHCI_INTERRUPT_SF 0x04
#define OHCI_INTERRUPT_MIE 0x80000000
#define OHCI_HCCA_PHYS 0x100000
#define OHCI_HCCA_IOVA 0x40000000
#define XBRIDGE_ATE0 0x10000
#define XBRIDGE_ATE_VALID 1
#define XBRIDGE_ATE_NO_SWAP (1U << 5)

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

static char *fuel_prid_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    stl_be_p(image, 0x100000ff);        /* branch to 0x400 */
    stl_be_p(image + 0x004, 0);
    stl_be_p(image + 0x400, 0x400c6000); /* mfc0 t4, Status */
    stl_be_p(image + 0x404, 0x358c0080); /* enable 64-bit kernel addresses */
    stl_be_p(image + 0x408, 0x408c6000); /* mtc0 t4, Status */
    stl_be_p(image + 0x40c, 0);
    stl_be_p(image + 0x410, 0x3c089200); /* local crosstalk pointer */
    stl_be_p(image + 0x414, 0x0008403c);
    stl_be_p(image + 0x418, 0x3c090100);
    stl_be_p(image + 0x41c, 0x01094025);
    stl_be_p(image + 0x420, 0x40097800); /* mfc0 t1, PRId */
    stl_be_p(image + 0x424, 0xfd090108); /* publish in PI compare A */
    stl_be_p(image + 0x428, 0x1000ffff);
    stl_be_p(image + 0x42c, 0);

    fd = g_file_open_tmp("sn1-fuel-prid-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void fuel_wait_prid(QTestState *qts, uint32_t expected)
{
    uint64_t value = 0;

    for (unsigned int attempt = 0; attempt < 200; attempt++) {
        value = qtest_readq(qts, BEDROCK_LOCAL + 0x108);
        if (value) {
            break;
        }
        g_usleep(10000);
    }
    g_assert_cmphex(value, ==, expected);
}

static QTestState *fuel_prid_start(const char *prom, const char *extra)
{
    g_autofree char *quoted = g_shell_quote(prom);

    return qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                       "fuel-ioc3-subsystem-id=0xc30a,"
                       "fuel-l1-type-code=0x61,"
                       "fuel-bedrock-revision=0x2 "
                       "-cpu R14000 -smp 1 -m 512M -bios %s "
                       "-accel tcg,thread=multi -S %s", quoted, extra);
}

static void test_fuel_processor_prid(const void *data)
{
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    prom = fuel_prid_prom();
    quoted = g_shell_quote(prom);
    for (unsigned int which = 0; which < 2; which++) {
        QTestState *qts;
        uint32_t expected = which ? 0x0f24 : 0x0f23;

        if (which) {
            qts = qtest_initf("-M origin300 -cpu R14000 -smp 2 -m 512M "
                              "-bios %s -accel tcg,thread=multi -S", quoted);
        } else {
            qts = fuel_prid_start(prom, "");
        }
        qtest_qmp_assert_success(qts, "{'execute':'cont'}");
        fuel_wait_prid(qts, expected);
        qtest_qmp_assert_success(qts, "{'execute':'stop'}");
        qtest_system_reset(qts);
        g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + 0x108), ==, 0);
        qtest_qmp_assert_success(qts, "{'execute':'cont'}");
        fuel_wait_prid(qts, expected);
        qtest_quit(qts);
    }
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

static void test_fuel_processor_prid_migration(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *prom = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    prom = fuel_prid_prom();
    directory = g_dir_make_tmp("sn1-fuel-prid-migration-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = fuel_prid_start(prom, "");
    to = fuel_prid_start(prom, "-incoming defer");
    qtest_qmp_assert_success(from, "{'execute':'cont'}");
    fuel_wait_prid(from, 0x0f23);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, BEDROCK_LOCAL + 0x108), ==, 0x0f23);
    qtest_qmp_assert_success(to, "{'execute':'stop'}");
    qtest_system_reset(to);
    g_assert_cmphex(qtest_readq(to, BEDROCK_LOCAL + 0x108), ==, 0);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    fuel_wait_prid(to, 0x0f23);
    qtest_quit(from);
    qtest_quit(to);
    g_assert_cmpint(g_unlink(prom), ==, 0);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static QTestState *fuel_start(const char *prom)
{
    g_autofree char *quoted = g_shell_quote(prom);

    return qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                       "fuel-ioc3-subsystem-id=0xc30a,"
                       "fuel-l1-type-code=0x61,fuel-bedrock-revision=0x2 "
                       "-cpu R14000 -smp 1 -m 512M -bios %s -S", quoted);
}

static void test_fuel_mac_eeprom_machine(const void *data)
{
    uint8_t image[512] = { 0 };
    g_autofree char *path = NULL;
    GError *error = NULL;
    QTestState *qts;
    unsigned sum = 0;
    int fd;

    if (g_test_subprocess()) {
        qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                    "fuel-ioc3-subsystem-id=0xc30a,"
                    "fuel-l1-type-code=0x61,fuel-bedrock-revision=0x2,"
                    "fuel-mac-eeprom=%s "
                    "-bios %s -S -nic %s",
                    g_getenv("FUEL_MAC_PATH"), (const char *)data,
                    g_getenv("FUEL_MAC_NIC"));
        return;
    }
    fd = g_file_open_tmp("fuel-mac-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    image[3] = 4;
    image[33] = 16;
    image[39] = 4;
    memcpy(image + 40, "MAC ", 4);
    image[44] = 1;
    image[45] = 'X';
    image[46] = 1;
    image[47] = 'Y';
    image[49] = 1;
    image[50] = 'Z';
    image[53] = 1;
    image[54] = 'Q';
    image[70] = 12;
    memcpy(image + 71, "02A1B2C3D4E5", 12);
    for (unsigned i = 32; i < 160; i++) {
        sum += image[i];
    }
    image[32] = -sum;
    sum = 0;
    for (unsigned i = 0; i < 8; i++) {
        sum += image[i];
    }
    image[0] = -sum;
    g_assert_true(g_file_set_contents(path, (char *)image, sizeof(image),
                                      &error));
    g_assert_no_error(error);
    qts = qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                      "fuel-ioc3-subsystem-id=0xc30a,"
                      "fuel-l1-type-code=0x61,fuel-bedrock-revision=0x2,"
                      "fuel-mac-eeprom=%s "
                      "-bios %s -S -nic user", path,
                      (const char *)data);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{"
        "'path':'/machine/node0/baseio/ioc3','property':'mac'}}");

    g_assert_false(qdict_haskey(response, "error"));
    g_assert_cmpstr(qdict_get_str(response, "return"), ==,
                    "02:a1:b2:c3:d4:e5");
    qtest_quit(qts);
    g_setenv("FUEL_MAC_PATH", path, true);
    g_setenv("FUEL_MAC_NIC", "user,mac=02:a1:b2:c3:d4:e5", true);
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_passed();
    g_setenv("FUEL_MAC_NIC", "user,mac=02:a1:b2:c3:d4:e6", true);
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*IOC3 NIC MAC conflicts with board identity*");
    image[71] = 'G';
    image[32] -= 'G' - '0';
    g_assert_true(g_file_set_contents(path, (char *)image, sizeof(image),
                                      &error));
    g_assert_no_error(error);
    g_setenv("FUEL_MAC_NIC", "user", true);
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*Fuel MAC EEPROM record is malformed*");
    memcpy(image + 71, "000000000000", 12);
    image[32] = 0;
    sum = 0;
    for (unsigned i = 32; i < 160; i++) {
        sum += image[i];
    }
    image[32] = -sum;
    g_assert_true(g_file_set_contents(path, (char *)image, sizeof(image),
                                      &error));
    g_assert_no_error(error);
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*Fuel MAC EEPROM record is malformed*");
    g_unsetenv("FUEL_MAC_PATH");
    g_unsetenv("FUEL_MAC_NIC");
    unlink(path);
}

static QTestState *fuel_start_running(const char *prom)
{
    g_autofree char *quoted = g_shell_quote(prom);

    return qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                       "fuel-ioc3-subsystem-id=0xc30a,"
                       "fuel-l1-type-code=0x61,fuel-bedrock-revision=0x2 "
                       "-cpu R14000 -smp 1 -m 512M -bios %s", quoted);
}

static void fuel_map_ioc3(QTestState *qts)
{
    qtest_writel(qts, PCI_CONFIG(15, 4) + 0x10, IOC3_BAR);
    qtest_writel(qts, PCI_CONFIG(15, 4) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, XTALK_WIDGET(15) + 0x200 + 4 * 8 + 4,
                 (1U << 12) | (IOC3_BAR >> 20));
}

static uint32_t fuel_xbridge_read(QTestState *qts, unsigned int offset)
{
    return qtest_readl(qts, XTALK_WIDGET(FUEL_XBRIDGE) + offset + 4);
}

static void fuel_xbridge_write(QTestState *qts, unsigned int offset,
                               uint32_t value)
{
    qtest_writel(qts, XTALK_WIDGET(FUEL_XBRIDGE) + offset + 4, value);
}

static void fuel_route_irq(QTestState *qts, unsigned int pin,
                           unsigned int pending_bit)
{
    fuel_xbridge_write(qts, XBRIDGE_INT_UPPER, XBRIDGE_HOST_PORT << 16);
    fuel_xbridge_write(qts, XBRIDGE_INT_LOWER,
                       PI_REMOTE_WINDOW + PI_PENDING_MODIFY);
    fuel_xbridge_write(qts, XBRIDGE_INT_HOST(pin),
                       (1U << 17) | pending_bit);
    fuel_xbridge_write(qts, XBRIDGE_INT_ENABLE, 1U << pin);
}

static uint32_t fuel_cpu_cause(QTestState *qts)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers 0");
    const char *value_text = strstr(registers, "Cause   0x");
    uint32_t value;

    g_assert_nonnull(value_text);
    value_text += strlen("Cause   0x");
    g_assert_cmpint(sscanf(value_text, "%" SCNx32, &value), ==, 1);
    return value;
}

static void fuel_expect_irq(QTestState *qts, unsigned int pin,
                            unsigned int pending_bit)
{
    uint64_t pending = UINT64_C(1) << pending_bit;

    g_assert_cmphex(fuel_xbridge_read(qts, XBRIDGE_INT_STATUS) & 0xff, ==,
                    1U << pin);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==,
                    pending);
    g_assert_cmphex(fuel_cpu_cause(qts) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_MASK_0_A, pending);
    g_assert_cmphex(fuel_cpu_cause(qts) & MIPS_CAUSE_HARDWARE_IRQ0, ==,
                    MIPS_CAUSE_HARDWARE_IRQ0);
}

static void fuel_ack_irq(QTestState *qts, unsigned int pending_bit)
{
    g_assert_cmphex(fuel_xbridge_read(qts, XBRIDGE_INT_STATUS) & 0xff, ==, 0);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_PENDING_MODIFY, pending_bit);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==, 0);
    g_assert_cmphex(fuel_cpu_cause(qts) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);
}

static void fuel_scsi_writew(QTestState *qts, unsigned int offset,
                             uint16_t value)
{
    qtest_writew(qts, SCSI_WINDOW + offset + 2, value);
}

static uint16_t fuel_scsi_readw(QTestState *qts, unsigned int offset)
{
    return qtest_readw(qts, SCSI_WINDOW + offset + 2);
}

static void test_fuel_scsi_interrupt(const void *data)
{
    const unsigned int pin = 1, pending_bit = 17;
    QTestState *qts = fuel_start(data);

    qtest_writel(qts, PCI_CONFIG(FUEL_XBRIDGE, 1) + 0x14, SCSI_BAR);
    qtest_writel(qts, PCI_CONFIG(FUEL_XBRIDGE, 1) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    fuel_xbridge_write(qts, XBRIDGE_DEVICE(1),
                       XBRIDGE_DEVICE_MEMORY | (SCSI_BAR >> 20));
    fuel_route_irq(qts, pin, pending_bit);
    g_assert_cmphex(fuel_xbridge_read(qts, XBRIDGE_INT_STATUS) & 0xff, ==, 0);
    fuel_scsi_writew(qts, ISP_BUS_CTRL, 0x0006);
    g_assert_cmphex(fuel_scsi_readw(qts, ISP_BUS_CTRL), ==, 0x0006);
    fuel_scsi_writew(qts, ISP_MAILBOX0, 0);
    fuel_scsi_writew(qts, ISP_HOST_CMD, 0x5000);
    g_assert_cmphex(fuel_scsi_readw(qts, ISP_SEMAPHORE) & 1, ==, 1);
    fuel_expect_irq(qts, pin, pending_bit);
    fuel_scsi_writew(qts, ISP_SEMAPHORE, 0);
    fuel_scsi_writew(qts, ISP_HOST_CMD, 0x7000);
    fuel_ack_irq(qts, pending_bit);
    qtest_quit(qts);
}

static void test_fuel_ioc3_superio_interrupt(const void *data)
{
    const unsigned int pin = 0, pending_bit = 18;
    QTestState *qts = fuel_start(data);

    fuel_map_ioc3(qts);
    fuel_route_irq(qts, pin, pending_bit);
    qtest_writel(qts, IOC3_WINDOW + IOC3_INT_ENABLE_SET,
                 IOC3_INT_SERIAL_A);
    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_A + 1, 0x02);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_INT_STATUS) &
                    IOC3_INT_SERIAL_A, ==, IOC3_INT_SERIAL_A);
    fuel_expect_irq(qts, pin, pending_bit);
    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_A + 1, 0);
    fuel_ack_irq(qts, pending_bit);
    qtest_quit(qts);
}

static void test_fuel_ioc3_superio_interrupt_reset(const void *data)
{
    const unsigned int pin = 0, pending_bit = 18;
    QTestState *qts = fuel_start(data);

    fuel_map_ioc3(qts);
    fuel_route_irq(qts, pin, pending_bit);
    qtest_writel(qts, IOC3_WINDOW + IOC3_INT_ENABLE_SET,
                 IOC3_INT_SERIAL_A);
    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_A + 1, 0x02);
    fuel_expect_irq(qts, pin, pending_bit);

    qtest_system_reset(qts);
    g_assert_cmphex(fuel_xbridge_read(qts, XBRIDGE_INT_STATUS) & (1U << pin),
                    ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0) &
                    (UINT64_C(1) << pending_bit), ==, 0);
    g_assert_cmphex(fuel_cpu_cause(qts) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);
    qtest_quit(qts);
}

static void test_fuel_ioc3_superio_interrupt_migration(const void *data)
{
    const unsigned int pin = 0, pending_bit = 18;
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *quoted = g_shell_quote(data);
    QTestState *from, *to;

    directory = g_dir_make_tmp("sn1-fuel-irq-migration-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = fuel_start(data);
    to = qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                     "fuel-ioc3-subsystem-id=0xc30a,"
                     "fuel-l1-type-code=0x61,fuel-bedrock-revision=0x2 "
                     "-cpu R14000 -smp 1 -m 512M -bios %s -S "
                     "-incoming defer", quoted);
    fuel_map_ioc3(from);
    fuel_route_irq(from, pin, pending_bit);
    qtest_writel(from, IOC3_WINDOW + IOC3_INT_ENABLE_SET,
                 IOC3_INT_SERIAL_A);
    qtest_writeb(from, IOC3_WINDOW + IOC3_SERIAL_A + 1, 0x02);
    fuel_expect_irq(from, pin, pending_bit);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readl(to, IOC3_WINDOW + IOC3_INT_STATUS) &
                    IOC3_INT_SERIAL_A, ==, IOC3_INT_SERIAL_A);
    g_assert_cmphex(fuel_xbridge_read(to, XBRIDGE_INT_STATUS) & (1U << pin),
                    ==, 1U << pin);
    g_assert_cmphex(qtest_readq(to, BEDROCK_LOCAL + PI_PENDING_0) &
                    (UINT64_C(1) << pending_bit), ==,
                    UINT64_C(1) << pending_bit);
    g_assert_cmphex(fuel_cpu_cause(to) & MIPS_CAUSE_HARDWARE_IRQ0, ==,
                    MIPS_CAUSE_HARDWARE_IRQ0);
    qtest_writeb(to, IOC3_WINDOW + IOC3_SERIAL_A + 1, 0);
    fuel_ack_irq(to, pending_bit);
    qtest_quit(from);
    qtest_quit(to);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_fuel_ohci_interrupt(const void *data)
{
    const unsigned int pin = 5, pending_bit = 19;
    uint8_t hcca[0x100] = { 0 };
    QTestState *qts = fuel_start_running(data);

    qtest_writeq(qts, BEDROCK_MD_CONFIG, 0x33);
    qtest_writel(qts, PCI_CONFIG(FUEL_XBRIDGE, 5) + 0x10, OHCI_BAR);
    qtest_writel(qts, PCI_CONFIG(FUEL_XBRIDGE, 5) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    fuel_xbridge_write(qts, XBRIDGE_DEVICE(5),
                       XBRIDGE_DEVICE_MEMORY | (OHCI_BAR >> 20));
    fuel_route_irq(qts, pin, pending_bit);
    g_assert_cmphex(qtest_readl(qts, OHCI_WINDOW), ==, 0x10);
    qtest_memwrite(qts, OHCI_HCCA_PHYS, hcca, sizeof(hcca));
    qtest_writeq(qts, XTALK_WIDGET(FUEL_XBRIDGE) + XBRIDGE_ATE0,
                 OHCI_HCCA_PHYS |
                 ((uint64_t)XBRIDGE_HOST_PORT << 8) |
                 XBRIDGE_ATE_NO_SWAP | XBRIDGE_ATE_VALID);
    qtest_writel(qts, OHCI_WINDOW + OHCI_HCCA, OHCI_HCCA_IOVA);
    qtest_writel(qts, OHCI_WINDOW + OHCI_INTERRUPT_ENABLE,
                 OHCI_INTERRUPT_MIE | OHCI_INTERRUPT_SF);
    qtest_writel(qts, OHCI_WINDOW + OHCI_CONTROL,
                 OHCI_CONTROL_OPERATIONAL);
    for (unsigned int i = 0; i < 64; i++) {
        if (fuel_xbridge_read(qts, XBRIDGE_INT_STATUS) & (1U << pin)) {
            break;
        }
        qtest_clock_step_next(qts);
    }
    fuel_expect_irq(qts, pin, pending_bit);
    qtest_writel(qts, OHCI_WINDOW + OHCI_INTERRUPT_STATUS,
                 OHCI_INTERRUPT_SF);
    fuel_ack_irq(qts, pending_bit);
    qtest_writel(qts, OHCI_WINDOW + OHCI_CONTROL, OHCI_CONTROL_SUSPEND);
    qtest_quit(qts);
}

static void test_fuel_ohci_frame_remaining(const void *data)
{
    QTestState *qts = fuel_start(data);

    qtest_writel(qts, PCI_CONFIG(FUEL_XBRIDGE, 5) + 0x10, OHCI_BAR);
    qtest_writel(qts, PCI_CONFIG(FUEL_XBRIDGE, 5) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY);
    fuel_xbridge_write(qts, XBRIDGE_DEVICE(5),
                       XBRIDGE_DEVICE_MEMORY | (OHCI_BAR >> 20));

    g_assert_cmphex(qtest_readl(qts, OHCI_WINDOW), ==, 0x10);
    g_assert_cmphex(qtest_readl(qts, OHCI_WINDOW + OHCI_FM_REMAINING), ==, 0);
    qtest_writel(qts, OHCI_WINDOW + OHCI_FM_INTERVAL, 0xffff3fff);
    g_assert_cmphex(qtest_readl(qts, OHCI_WINDOW + OHCI_FM_INTERVAL), ==,
                    0xffff3fff);
    qtest_writel(qts, OHCI_WINDOW + OHCI_FM_INTERVAL, 0);

    /* Lucent USS-312 Rev. 5 Table 41 marks this register host read-only. */
    qtest_writel(qts, OHCI_WINDOW + OHCI_FM_REMAINING, 0x80003fff);
    g_assert_cmphex(qtest_readl(qts, OHCI_WINDOW + OHCI_FM_REMAINING), ==, 0);
    qtest_quit(qts);
}

static void test_fuel_ioc3_ethernet_interrupt(const void *data)
{
    static const uint8_t frame[14] = {
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x08, 0x00,
    };
    const unsigned int pin = 4, pending_bit = 20;
    uint8_t packet[4 + sizeof(frame)];
    g_autofree char *quoted = g_shell_quote(data);
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                      "fuel-ioc3-subsystem-id=0xc30a,"
                      "fuel-l1-type-code=0x61,fuel-bedrock-revision=0x2 "
                      "-cpu R14000 -smp 1 -m 512M -bios %s "
                      "-nic socket,fd=%d,id=ioc3-net", quoted, sv[1]);
    g_assert_cmpint(close(sv[1]), ==, 0);
    qtest_writeq(qts, BEDROCK_MD_CONFIG, 0x33);
    fuel_map_ioc3(qts);
    fuel_route_irq(qts, pin, pending_bit);
    qtest_writeq(qts, XTALK_WIDGET(FUEL_XBRIDGE) + XBRIDGE_ATE0,
                 IOC3_RX_RING_PHYS |
                 ((uint64_t)XBRIDGE_HOST_PORT << 8) |
                 XBRIDGE_ATE_NO_SWAP | XBRIDGE_ATE_VALID);
    qtest_writeq(qts, XTALK_WIDGET(FUEL_XBRIDGE) + XBRIDGE_ATE0 + 8,
                 IOC3_RX_BUFFER_PHYS |
                 ((uint64_t)XBRIDGE_HOST_PORT << 8) |
                 XBRIDGE_ATE_NO_SWAP | XBRIDGE_ATE_VALID);
    qtest_writel(qts, IOC3_RX_RING_PHYS, 0);
    qtest_writel(qts, IOC3_RX_RING_PHYS + 4, IOC3_RX_BUFFER_IOVA);
    qtest_writel(qts, IOC3_WINDOW + IOC3_ENET_MAC_CONTROL,
                 IOC3_ENET_MCR_PROMISC | IOC3_ENET_MCR_RX_DMA_EN |
                 IOC3_ENET_MCR_RX_EN | IOC3_ENET_MCR_RX_OFFSET(33));
    qtest_writel(qts, IOC3_WINDOW + IOC3_ENET_INT_ENABLE,
                 IOC3_ENET_INT_RX_TIMER);
    qtest_writel(qts, IOC3_WINDOW + IOC3_ENET_RX_RING_HI, 0);
    qtest_writel(qts, IOC3_WINDOW + IOC3_ENET_RX_RING_LO,
                 IOC3_RX_RING_IOVA);
    qtest_writel(qts, IOC3_WINDOW + IOC3_ENET_RX_TIMER, 0);
    qtest_writel(qts, IOC3_WINDOW + IOC3_ENET_RX_CONTROL, 0);
    qtest_writel(qts, IOC3_WINDOW + IOC3_ENET_RX_PRODUCE,
                 8 | IOC3_ENET_RX_PRODUCE_ARM);
    stl_be_p(packet, sizeof(frame));
    memcpy(packet + 4, frame, sizeof(frame));
    g_assert_cmpint(write(sv[0], packet, sizeof(packet)), ==,
                    (int)sizeof(packet));
    for (unsigned int i = 0; i < 200; i++) {
        if (qtest_readl(qts, IOC3_WINDOW + IOC3_ENET_RX_CONSUME) == 8) {
            break;
        }
        g_usleep(1000);
        qtest_clock_step(qts, 50000);
    }
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_ENET_RX_CONSUME),
                    ==, 8);
    g_assert_cmphex(qtest_readl(qts, IOC3_RX_BUFFER_PHYS) &
                    IOC3_ENET_RX_HEADER_VALID, ==,
                    IOC3_ENET_RX_HEADER_VALID);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_ENET_INT_STATUS) &
                    IOC3_ENET_INT_RX_TIMER, ==, IOC3_ENET_INT_RX_TIMER);
    fuel_expect_irq(qts, pin, pending_bit);
    qtest_writel(qts, IOC3_WINDOW + IOC3_ENET_INT_STATUS,
                 IOC3_ENET_INT_RX_TIMER);
    fuel_ack_irq(qts, pending_bit);
    qtest_quit(qts);
    g_assert_cmpint(close(sv[0]), ==, 0);
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

static uint16_t fuel_phy_read(QTestState *qts, unsigned int address,
                              unsigned int reg)
{
    qtest_writel(qts, IOC3_WINDOW + IOC3_ENET_MII_CONTROL,
                 IOC3_ENET_MII_READ | (address << 5) | reg);
    return qtest_readl(qts, IOC3_WINDOW + IOC3_ENET_MII_DATA) & 0xffff;
}

static void fuel_phy_write(QTestState *qts, unsigned int address,
                           unsigned int reg, uint16_t value)
{
    qtest_writel(qts, IOC3_WINDOW + IOC3_ENET_MII_WRITE_DATA, value);
    qtest_writel(qts, IOC3_WINDOW + IOC3_ENET_MII_CONTROL,
                 (address << 5) | reg);
}

static void test_fuel_ioc3_phy(const void *data)
{
    QTestState *qts = fuel_start(data);

    fuel_map_ioc3(qts);
    g_assert_cmphex(fuel_phy_read(qts, 1, 0), !=, 0xffff);
    g_assert_cmphex(fuel_phy_read(qts, 1, 2), ==, 0x2000);
    g_assert_cmphex(fuel_phy_read(qts, 1, 3), ==, 0x5c10);
    g_assert_cmphex(fuel_phy_read(qts, 1, 1), ==, 0x7849);
    g_assert_cmphex(fuel_phy_read(qts, 1, 6), ==, 0x0004);
    g_assert_cmphex(fuel_phy_read(qts, 1, 0x17), ==, 0);
    g_assert_cmphex(fuel_phy_read(qts, 1, 0x18) & 0xc77f, ==, 0);
    g_assert_cmphex(fuel_phy_read(qts, 31, 0), ==, 0xffff);

    fuel_phy_write(qts, 1, 0x17, 0x20);
    fuel_phy_write(qts, 1, 0x18, 0x400);
    g_assert_cmphex(fuel_phy_read(qts, 1, 0x17), ==, 0x20);
    g_assert_cmphex(fuel_phy_read(qts, 1, 0x18) & 0xc77f, ==, 0x400);
    fuel_phy_write(qts, 1, 0, 0x8000);
    g_assert_cmphex(fuel_phy_read(qts, 1, 1), ==, 0x7849);
    g_assert_cmphex(fuel_phy_read(qts, 1, 6), ==, 0x0004);
    g_assert_cmphex(fuel_phy_read(qts, 1, 0x17), ==, 0);
    g_assert_cmphex(fuel_phy_read(qts, 1, 0x18) & 0xc77f, ==, 0);

    fuel_phy_write(qts, 1, 0x17, 0x20);
    fuel_phy_write(qts, 1, 0x18, 0x400);
    qtest_system_reset(qts);
    fuel_map_ioc3(qts);
    g_assert_cmphex(fuel_phy_read(qts, 1, 3), ==, 0x5c10);
    g_assert_cmphex(fuel_phy_read(qts, 1, 1), ==, 0x7849);
    g_assert_cmphex(fuel_phy_read(qts, 1, 6), ==, 0x0004);
    g_assert_cmphex(fuel_phy_read(qts, 1, 0x17), ==, 0);
    g_assert_cmphex(fuel_phy_read(qts, 1, 0x18) & 0xc77f, ==, 0);
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

/* Reuse the standalone IOC3 test's QMP input path on the Fuel board. */
static void fuel_ps2_key_down(QTestState *qts, const char *key)
{
    qtest_qmp_assert_success(qts,
        "{'execute':'input-send-event','arguments':"
        " {'events':[{'type':'key','data':"
        " {'down':true,'key':{'type':'qcode','data':%s}}}]}}", key);
}

static void test_fuel_ps2_keyboard_host_input(const void *data)
{
    const unsigned int pin = 0, pending_bit = 18;
    static const char *keys[] = { "a", "b", "c", "d", "e" };
    QTestState *qts = fuel_start_running(data);

    fuel_map_ioc3(qts);
    qtest_writel(qts, IOC3_WINDOW + IOC3_KBC_CONTROL,
                 IOC3_KBC_CLAMP_BOTH_3);
    /* Select raw scan-code set 2 before injecting host keys. */
    qtest_writel(qts, IOC3_WINDOW + IOC3_KBC_KBD_TRANSMIT, 0xf0);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_KBD_RECEIVE),
                    ==, 0x80fa0000);
    qtest_writel(qts, IOC3_WINDOW + IOC3_KBC_KBD_TRANSMIT, 2);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_KBD_RECEIVE),
                    ==, 0x80fa0000);
    qtest_writel(qts, IOC3_WINDOW + IOC3_INT_STATUS, IOC3_INT_KEYBOARD);
    fuel_route_irq(qts, pin, pending_bit);
    qtest_writel(qts, IOC3_WINDOW + IOC3_INT_ENABLE_SET, IOC3_INT_KEYBOARD);

    for (unsigned int i = 0; i < ARRAY_SIZE(keys); i++) {
        fuel_ps2_key_down(qts, keys[i]);
    }
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_INT_STATUS) &
                    IOC3_INT_KEYBOARD, ==, IOC3_INT_KEYBOARD);
    fuel_expect_irq(qts, pin, pending_bit);

    /* Linux acknowledges first; reading releases the two clamped keys. */
    qtest_writel(qts, IOC3_WINDOW + IOC3_INT_STATUS, IOC3_INT_KEYBOARD);
    fuel_ack_irq(qts, pending_bit);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_KBD_RECEIVE),
                    ==, 0xe01c3221);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_INT_STATUS) &
                    IOC3_INT_KEYBOARD, ==, IOC3_INT_KEYBOARD);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_MASK_0_A, 0);
    fuel_expect_irq(qts, pin, pending_bit);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_KBD_RECEIVE),
                    ==, 0xc0232400);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_KBD_RECEIVE),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_AUX_RECEIVE),
                    ==, 0);
    qtest_writel(qts, IOC3_WINDOW + IOC3_INT_STATUS, IOC3_INT_KEYBOARD);
    fuel_ack_irq(qts, pending_bit);
    qtest_quit(qts);
}

static void test_fuel_ps2_mouse_host_input(const void *data)
{
    const unsigned int pin = 0, pending_bit = 18;
    QTestState *qts = fuel_start_running(data);

    fuel_map_ioc3(qts);
    qtest_writel(qts, IOC3_WINDOW + IOC3_KBC_CONTROL,
                 IOC3_KBC_CLAMP_BOTH_3);
    qtest_writel(qts, IOC3_WINDOW + IOC3_KBC_AUX_TRANSMIT, 0xf4);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_AUX_RECEIVE),
                    ==, 0x88fa0000);
    qtest_writel(qts, IOC3_WINDOW + IOC3_INT_STATUS, IOC3_INT_KEYBOARD);
    fuel_route_irq(qts, pin, pending_bit);
    qtest_writel(qts, IOC3_WINDOW + IOC3_INT_ENABLE_SET, IOC3_INT_KEYBOARD);
    qtest_qmp_assert_success(qts,
        "{'execute':'input-send-event','arguments':{'events':["
        " {'type':'rel','data':{'axis':'x','value':4}},"
        " {'type':'btn','data':{'down':true,'button':'left'}}]}}");
    qtest_clock_step(qts, NS_PER_SEC / 100);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_INT_STATUS) &
                    IOC3_INT_KEYBOARD, ==, IOC3_INT_KEYBOARD);
    fuel_expect_irq(qts, pin, pending_bit);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_AUX_RECEIVE),
                    ==, 0xe8090400);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_KBD_RECEIVE),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_KBC_AUX_RECEIVE),
                    ==, 0);
    /* A drained packet stays pending until the shared source is cleared. */
    g_assert_cmphex(qtest_readl(qts, IOC3_WINDOW + IOC3_INT_STATUS) &
                    IOC3_INT_KEYBOARD, ==, IOC3_INT_KEYBOARD);
    qtest_writel(qts, IOC3_WINDOW + IOC3_INT_STATUS, IOC3_INT_KEYBOARD);
    fuel_ack_irq(qts, pending_bit);
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

static uint64_t fuel_qom_uint(QTestState *qts, const char *path,
                               const char *property)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, property);

    g_assert_false(qdict_haskey(response, "error"));
    return qdict_get_int(response, "return");
}

static void test_fuel_zero_identity_inputs(const void *data)
{
    g_autofree char *quoted = g_shell_quote(data);
    QTestState *qts = qtest_initf(
        "-M fuel,fuel-board-id-word=0x4abc,fuel-ioc3-subsystem-id=0,"
        "fuel-l1-type-code=0x61,fuel-bedrock-revision=0 "
        "-cpu R14000 -smp 1 -m 512M -bios %s -S", quoted);

    g_assert_cmphex(BEDROCK_REVISION(qtest_readq(qts, LB_REVISION)), ==, 0);
    g_assert_cmphex(fuel_qom_uint(qts, "/machine/node0/baseio/ioc3",
                                 "subsystem-id"), ==, 0);
    qtest_system_reset(qts);
    g_assert_cmphex(BEDROCK_REVISION(qtest_readq(qts, LB_REVISION)), ==, 0);
    qtest_quit(qts);
}

static void test_baseio_identity_properties(const void *data)
{
    static const char *options[] = {
        "ioc3-subsystem-id=0x1234,ioc3-mac=02:00:00:00:00:01",
        "fuel-subsystem-id=0x1234,fuel-mac=02:00:00:00:00:01",
    };
    g_autofree char *quoted = g_shell_quote(data);

    for (unsigned int i = 0; i < ARRAY_SIZE(options); i++) {
        QTestState *qts = qtest_initf(
            "-M origin300 -cpu R14000 -smp 2 -m 512M -bios %s -S "
            "-device sgi-baseio,id=direct,preset=fuel,"
            "pci-bus=/machine/node0/xbridge/head-e/head-e.0,%s,"
            "scsi-revision=6,ioc3-revision=1,usb-revision=16",
            quoted, options[i]);
        g_autoptr(QDict) response = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':{"
            "'path':'/machine/peripheral/direct/ioc3','property':'board-mac'}}");

        g_assert_cmphex(fuel_qom_uint(qts, "/machine/peripheral/direct/ioc3",
                                     "subsystem-id"), ==, 0x1234);
        g_assert_false(qdict_haskey(response, "error"));
        g_assert_cmpstr(qdict_get_str(response, "return"), ==,
                        "02:00:00:00:00:01");
        qtest_quit(qts);
    }
}

static void test_baseio_rejects_unsupported_revision(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-m", "512M", "-bios", data, "-S", "-display", "none",
        "-device", "sgi-baseio,preset=fuel,"
        "pci-bus=/machine/node0/xbridge/head-e/head-e.0,"
        "fuel-subsystem-id=0x1234,ioc3-revision=2",
        "-qtest", "stdio", "-accel", "qtest", NULL,
    };
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_true(g_pattern_match_simple("*ioc3-revision 2 is unsupported*",
                                         error_output));
}

static void test_fuel_rejects_other_board_code(const void *data)
{
    const char *prom = data;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*board ID fails its configured mask/required-bits constraint*");
        return;
    }
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf("-M fuel,fuel-board-id-word=0x8abc,"
                                  "fuel-ioc3-subsystem-id=0xc30a,"
                                  "fuel-l1-type-code=0x61,fuel-bedrock-revision=0x2 "
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
                                  "fuel-ioc3-subsystem-id=0xc30a,fuel-bedrock-revision=2 "
                                  "-cpu R14000 -smp 1 -m 512M -bios %s -S",
                                  quoted);
    qtest_quit(qts);
}

static void test_fuel_rejects_out_of_range_l1_type_code(const void *data)
{
    const char *prom = data;
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "fuel,fuel-board-id-word=0x4abc,"
        "fuel-ioc3-subsystem-id=0xc30a,"
        "fuel-l1-type-code=0x100,fuel-bedrock-revision=0x2",
        "-cpu", "R14000", "-smp", "1", "-m", "512M", "-bios", prom, "-S",
        "-display", "none", "-qtest", "stdio", "-accel", "qtest", NULL
    };
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_true(g_pattern_match_simple("*fuel-l1-type-code*uint8*",
                                        error_output));
}

static void test_fuel_rejects_zero_l1_type_code(const void *data)
{
    const char *prom = data;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*fuel-l1-type-code must be nonzero*");
        return;
    }
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                                  "fuel-ioc3-subsystem-id=0xc30a,"
                                  "fuel-l1-type-code=0,fuel-bedrock-revision=0x2 "
                                  "-cpu R14000 -smp 1 -m 512M -bios %s -S",
                                  quoted);
    qtest_quit(qts);
}

static void test_fuel_requires_bedrock_revision(const void *data)
{
    const char *prom = data;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*requires an explicit fuel-bedrock-revision*");
        return;
    }
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                                  "fuel-ioc3-subsystem-id=0xc30a,"
                                  "fuel-l1-type-code=0x61 "
                                  "-cpu R14000 -smp 1 -m 512M -bios %s -S",
                                  quoted);
    qtest_quit(qts);
}

static void test_fuel_rejects_large_bedrock_revision(const void *data)
{
    const char *prom = data;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*fuel-bedrock-revision must fit four bits*");
        return;
    }
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                                  "fuel-ioc3-subsystem-id=0xc30a,"
                                  "fuel-l1-type-code=0x61,"
                                  "fuel-bedrock-revision=0x10 "
                                  "-cpu R14000 -smp 1 -m 512M -bios %s -S",
                                  quoted);
    qtest_quit(qts);
}

static void test_fuel_bedrock_revision_rejects_other_machine(const void *data)
{
    const char *prom = data;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*input 'fuel-bedrock-revision' is not bound by topology 'origin300'*");
        return;
    }
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf("-M origin300,fuel-bedrock-revision=0x2 "
                                  "-cpu R14000 -smp 2 -m 512M -bios %s -S",
                                  quoted);
    qtest_quit(qts);
}

static void test_fuel_bedrock_revision(const void *data)
{
    g_autofree char *quoted = g_shell_quote(data);
    QTestState *qts = qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                                  "fuel-ioc3-subsystem-id=0xc30a,"
                                  "fuel-l1-type-code=0x61,"
                                  "fuel-bedrock-revision=0x9 "
                                  "-cpu R14000 -smp 1 -m 512M -bios %s -S",
                                  quoted);

    g_assert_cmphex(BEDROCK_REVISION(qtest_readq(qts, LB_REVISION)), ==, 9);
    g_assert_cmphex(BEDROCK_REVISION(qtest_readq(qts, II_WIDGET_ID)), ==, 9);
    qtest_system_reset(qts);
    g_assert_cmphex(BEDROCK_REVISION(qtest_readq(qts, LB_REVISION)), ==, 9);
    g_assert_cmphex(BEDROCK_REVISION(qtest_readq(qts, II_WIDGET_ID)), ==, 9);
    qtest_quit(qts);

    qts = qtest_initf("-M origin300 -cpu R14000 -smp 2 -m 512M "
                      "-bios %s -S", quoted);
    g_assert_cmphex(BEDROCK_REVISION(qtest_readq(qts, LB_REVISION)), ==, 2);
    g_assert_cmphex(BEDROCK_REVISION(qtest_readq(qts, II_WIDGET_ID)), ==, 2);
    qtest_quit(qts);
}

static void test_fuel_l1_type_code_rejects_other_machine(const void *data)
{
    const char *prom = data;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*input 'fuel-l1-type-code' is not bound by topology 'origin300'*");
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
                     "fuel-l1-type-code=0x61,fuel-bedrock-revision=0x2 "
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
        "fuel-l1-type-code=0x61,fuel-bedrock-revision=0x2 "
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
    uint32_t saved_window, pci_bar;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, a), ==, 0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, b), ==, 0);
    qts = qtest_initf(
        "-M fuel,fuel-board-id-word=0x4abc,"
        "fuel-ioc3-subsystem-id=0xc30a,"
        "fuel-l1-type-code=0x61,fuel-bedrock-revision=0x2 "
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

    pci_bar = qtest_readl(qts, PCI_CONFIG(15, 4) + PCI_BAR0);
    saved_window = fuel_xbridge_read(qts, XBRIDGE_DEVICE(4));
    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_A + IOC3_UART_SCR, 0x5a);
    /* Move only the documented PCI memory base bits, then restore them. */
    fuel_xbridge_write(qts, XBRIDGE_DEVICE(4),
                       (saved_window & ~0x3ffu) | 0x3ffu);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_A +
                                IOC3_UART_SCR), !=, 0x5a);
    g_assert_cmpint(write(a[0], "z", 1), ==, 1);
    fuel_xbridge_write(qts, XBRIDGE_DEVICE(4), saved_window);
    g_assert_cmphex(qtest_readl(qts, PCI_CONFIG(15, 4) + PCI_BAR0), ==,
                    pci_bar);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_A +
                                IOC3_UART_SCR), ==, 0x5a);
    fuel_wait_serial_input(qts, IOC3_WINDOW + IOC3_SERIAL_A);
    g_assert_cmphex(qtest_readb(qts, IOC3_WINDOW + IOC3_SERIAL_A), ==, 'z');
    qtest_writeb(qts, IOC3_WINDOW + IOC3_SERIAL_A, 'Z');
    fuel_expect_serial_byte(a[0], 'Z');

    qtest_quit(qts);
    g_assert_cmpint(close(a[0]), ==, 0);
    g_assert_cmpint(close(b[0]), ==, 0);
}

static QTestState *fuel_start_system_timekeeper_args(const char *prom,
                                                     const char *base,
                                                     const char *extra)
{
    g_autofree char *quoted = g_shell_quote(prom);

    return qtest_initf(
        "-M fuel,"
        "fuel-board-id-word=0x4abc,fuel-ioc3-subsystem-id=0xc30a,"
        "fuel-l1-type-code=0x61,fuel-bedrock-revision=2 "
        "-cpu R14000 -smp 1 -m 512M -bios %s "
        "-rtc base=%s,clock=vm -d unimp,guest_errors %s", quoted, base, extra);
}

static QTestState *fuel_start_system_timekeeper(const char *prom)
{
    return fuel_start_system_timekeeper_args(prom, "2026-01-02T03:04:05", "-S");
}

static void test_fuel_system_timekeeper(const void *data)
{
    QTestState *qts = fuel_start_system_timekeeper(data);

    fuel_map_ioc3(qts);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x100), ==, 0);
    qtest_writeb(qts, FUEL_RTC + 0x100, 0x5a);
    qtest_writeb(qts, FUEL_RTC + 0x900, 0xa5);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x100), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x900), ==, 0xa5);
    qtest_writeb(qts, FUEL_RTC + 0x7f8, 0x69);
    qtest_writeb(qts, FUEL_RTC + 0x7ff, 0x42);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x7fff), ==, 0x58);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x7f8), ==, 0x69);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x7ff), ==, 0x42);
    qtest_system_reset(qts);
    fuel_map_ioc3(qts);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x100), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x900), ==, 0xa5);
    qtest_quit(qts);
}

static void fuel_assert_clock(QTestState *qts, const uint8_t cells[7])
{
    for (unsigned int i = 0; i < 7; i++) {
        g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x7ff9 + i), ==, cells[i]);
    }
}

static void fuel_write_clock(QTestState *qts, const uint8_t cells[7])
{
    qtest_writeb(qts, FUEL_RTC + 0x7ff8, 0x80);
    for (unsigned int i = 0; i < 7; i++) {
        qtest_writeb(qts, FUEL_RTC + 0x7ff9 + i, cells[i]);
    }
    qtest_writeb(qts, FUEL_RTC + 0x7ff8, 0);
}

static void test_fuel_system_timekeeper_clock(const void *data)
{
    static const uint8_t initial[7] = { 0x05, 0x04, 0x03, 0, 0x02, 0x01, 0x58 };
    static const uint8_t feb2026[7] = {
        0x59, 0x59, 0x23, 0x05, 0x28, 0x02, 0x58
    };
    static const uint8_t mar2026[7] = { 0, 0, 0, 0x05, 0x01, 0x03, 0x58 };
    static const uint8_t feb2024[7] = {
        0x59, 0x59, 0x23, 0x05, 0x28, 0x02, 0x56
    };
    static const uint8_t leap2024[7] = { 0, 0, 0, 0x05, 0x29, 0x02, 0x56 };
    static const uint8_t dec2067[7] = {
        0x59, 0x59, 0x23, 0x05, 0x31, 0x12, 0x99
    };
    static const uint8_t jan1968[7] = { 0, 0, 0, 0x05, 0x01, 0x01, 0 };
    static const uint8_t jan1968_next[7] = {
        0x01, 0, 0, 0x05, 0x01, 0x01, 0
    };
    QTestState *qts = fuel_start_system_timekeeper(data);

    fuel_map_ioc3(qts);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x7ff8), ==, 0);
    fuel_assert_clock(qts, initial);
    qtest_writeb(qts, FUEL_RTC, 0x5a);
    qtest_writeb(qts, FUEL_RTC + 0x7ff7, 0xa5);
    for (unsigned int i = 0; i < 8; i++) {
        qtest_writeb(qts, FUEL_RTC + 0x7f8 + i, 0x30 + i);
    }
    qtest_writeb(qts, FUEL_RTC + 0x8000, 0x69);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x8000), ==, 0);

    qtest_writeb(qts, FUEL_RTC + 0x7ff8, 0x80);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x7ff8), ==, 0x80);
    for (unsigned int i = 0; i < 7; i++) {
        qtest_writeb(qts, FUEL_RTC + 0x7ff9 + i, feb2024[i]);
    }
    /* Weekday is stored immediately; the six counters transfer on W clear. */
    for (unsigned int i = 0; i < 7; i++) {
        if (i != 3) {
            g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x7ff9 + i),
                            ==, initial[i]);
        }
    }
    qtest_writeb(qts, FUEL_RTC + 0x7ff8, 0);
    fuel_assert_clock(qts, feb2024);
    qtest_clock_step(qts, NS_PER_SEC);
    fuel_assert_clock(qts, leap2024);

    fuel_write_clock(qts, feb2026);
    fuel_assert_clock(qts, feb2026);
    qtest_clock_step(qts, NS_PER_SEC);
    fuel_assert_clock(qts, mar2026);

    fuel_write_clock(qts, dec2067);
    fuel_assert_clock(qts, dec2067);
    qtest_clock_step(qts, NS_PER_SEC);
    fuel_assert_clock(qts, jan1968);

    fuel_write_clock(qts, dec2067);
    qtest_writeb(qts, FUEL_RTC + 0x7ff8, 0x40);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x7ff8), ==, 0x40);
    /* Every counter changes at this rollover, exposing a partial freeze. */
    qtest_clock_step(qts, 2LL * NS_PER_SEC);
    fuel_assert_clock(qts, dec2067);
    qtest_writeb(qts, FUEL_RTC + 0x7ff8, 0);
    fuel_assert_clock(qts, jan1968_next);

    qtest_writeb(qts, FUEL_RTC + 0x7ff8, 0x80);
    qtest_writeb(qts, FUEL_RTC + 0x7ff9, 0x82);
    qtest_writeb(qts, FUEL_RTC + 0x7ff8, 0);
    qtest_clock_step(qts, 3LL * NS_PER_SEC);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x7ff9), ==, 0x82);
    qtest_writeb(qts, FUEL_RTC + 0x7ff8, 0x80);
    qtest_writeb(qts, FUEL_RTC + 0x7ff9, 0x02);
    qtest_writeb(qts, FUEL_RTC + 0x7ff8, 0);
    qtest_clock_step(qts, NS_PER_SEC);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x7ff9), ==, 0x03);

    g_assert_cmphex(qtest_readb(qts, FUEL_RTC), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x7ff7), ==, 0xa5);
    for (unsigned int i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readb(qts, FUEL_RTC + 0x7f8 + i), ==, 0x30 + i);
    }
    qtest_quit(qts);
}

typedef struct FuelTimekeeperFiles {
    char *image;
    char *clock;
} FuelTimekeeperFiles;

static char *fuel_timekeeper_file(const char *template, size_t size)
{
    char *path = NULL;
    int fd = g_file_open_tmp(template, &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void fuel_timekeeper_files_new(FuelTimekeeperFiles *files)
{
    files->image = fuel_timekeeper_file("fuel-timekeeper-nvram-XXXXXX", 0x8000);
    files->clock = fuel_timekeeper_file("fuel-timekeeper-clock-XXXXXX", 16);
}

static void fuel_timekeeper_files_free(FuelTimekeeperFiles *files)
{
    g_assert_cmpint(g_unlink(files->image), ==, 0);
    g_assert_cmpint(g_unlink(files->clock), ==, 0);
    g_free(files->image);
    g_free(files->clock);
}

static QTestState *fuel_start_timekeeper_backed(const char *prom,
                                               const FuelTimekeeperFiles *files,
                                               const char *base,
                                               const char *uri)
{
    g_autofree char *image = g_shell_quote(files->image);
    g_autofree char *clock = g_shell_quote(files->clock);
    g_autofree char *incoming = uri ? g_strdup_printf("-incoming %s", uri)
                                   : g_strdup("");
    g_autofree char *extra = g_strdup_printf(
        "-drive if=none,id=sgi-nvram0,file=%s,format=raw "
        "-drive if=none,id=sgi-nvram0-clock,file=%s,format=raw %s",
        image, clock, incoming);

    return fuel_start_system_timekeeper_args(prom, base, extra);
}

static void fuel_timekeeper_set_storage(QTestState *qts, uint8_t cells[0x7ff8])
{
    for (unsigned int i = 0; i < 0x7ff8; i++) {
        cells[i] = i * 37 + 11;
    }
    cells[0] = 0x5a;
    cells[0x100] = 0x3c;
    cells[0x900] = 0xc3;
    cells[0x7ff7] = 0xa5;
    qtest_memwrite(qts, FUEL_RTC, cells, 0x7ff8);
}

static void fuel_timekeeper_assert_storage(QTestState *qts,
                                           const uint8_t cells[0x7ff8])
{
    uint8_t restored[0x7ff8];

    qtest_memread(qts, FUEL_RTC, restored, sizeof(restored));
    g_assert_cmpmem(restored, sizeof(restored), cells, sizeof(restored));
}

static void fuel_timekeeper_migration(const char *prom, bool stopped)
{
    uint8_t set[7] = { 0x59, 0x59, 0x23, 0x05, 0x31, 0x12, 0x99 };
    uint8_t next[7] = { 0, 0, 0, 0x05, 0x01, 0x01, 0 };
    uint8_t reopened[7] = { 0x10, 0, 0, 0x05, 0x01, 0x01, 0 };
    uint8_t ordinary[0x7ff8];
    g_autofree char *directory = g_dir_make_tmp("fuel-timekeeper-XXXXXX", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", directory);
    g_autofree char *before_image = NULL, *before_clock = NULL;
    g_autofree char *after_image = NULL, *after_clock = NULL;
    gsize before_image_len, before_clock_len, after_image_len, after_clock_len;
    g_autoptr(QDict) status = NULL;
    FuelTimekeeperFiles source, destination;
    QTestState *from, *to;

    g_assert_nonnull(directory);
    if (stopped) {
        set[0] |= 0x80;
        memcpy(next, set, sizeof(next));
        memcpy(reopened, set, sizeof(reopened));
    }
    fuel_timekeeper_files_new(&source);
    fuel_timekeeper_files_new(&destination);
    from = fuel_start_timekeeper_backed(prom, &source,
                                        "2026-01-02T03:04:05", NULL);
    fuel_map_ioc3(from);
    fuel_timekeeper_set_storage(from, ordinary);
    fuel_write_clock(from, set);
    fuel_assert_clock(from, set);
    to = fuel_start_timekeeper_backed(prom, &destination,
                                      "2026-01-02T03:04:05", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    status = qtest_qmp_assert_success_ref(to, "{'execute':'query-status'}");
    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "running");
    g_clear_pointer(&status, qdict_unref);
    status = qtest_qmp_assert_success_ref(from, "{'execute':'query-status'}");
    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "postmigrate");
    /* Restored PCI decode must expose the clock before any remapping. */
    fuel_timekeeper_assert_storage(to, ordinary);
    fuel_assert_clock(to, set);

    g_assert_true(g_file_get_contents(source.image, &before_image,
                                      &before_image_len, NULL));
    g_assert_true(g_file_get_contents(source.clock, &before_clock,
                                      &before_clock_len, NULL));
    qtest_qmp_assert_success(from, "{'execute':'quit'}");
    qtest_wait_qemu(from);
    qtest_quit(from);
    g_assert_true(g_file_get_contents(source.image, &after_image,
                                      &after_image_len, NULL));
    g_assert_true(g_file_get_contents(source.clock, &after_clock,
                                      &after_clock_len, NULL));
    g_assert_cmpmem(after_image, after_image_len,
                    before_image, before_image_len);
    g_assert_cmpmem(after_clock, after_clock_len,
                    before_clock, before_clock_len);

    qtest_system_reset(to);
    fuel_map_ioc3(to);
    fuel_timekeeper_assert_storage(to, ordinary);
    fuel_assert_clock(to, set);
    qtest_clock_step(to, NS_PER_SEC);
    fuel_assert_clock(to, next);
    qtest_qmp_assert_success(to, "{'execute':'quit'}");
    qtest_wait_qemu(to);
    qtest_quit(to);

    to = fuel_start_timekeeper_backed(prom, &destination,
                                      "2026-01-02T03:04:16", NULL);
    fuel_map_ioc3(to);
    fuel_timekeeper_assert_storage(to, ordinary);
    fuel_assert_clock(to, reopened);
    qtest_quit(to);
    fuel_timekeeper_files_free(&source);
    fuel_timekeeper_files_free(&destination);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_fuel_timekeeper_migration_running(const void *data)
{
    fuel_timekeeper_migration(data, false);
}

static void test_fuel_timekeeper_migration_stopped(const void *data)
{
    fuel_timekeeper_migration(data, true);
}

static void test_fuel_timekeeper_elapsed_rollover(const void *data)
{
    static const uint8_t saved[7] = {
        0x59, 0x59, 0x23, 0x05, 0x31, 0x12, 0x99
    };
    static const uint8_t restored[7] = { 0, 0, 0, 0x05, 0x01, 0x01, 0 };
    uint8_t ordinary[0x7ff8];
    FuelTimekeeperFiles files;
    QTestState *qts;

    fuel_timekeeper_files_new(&files);
    qts = fuel_start_timekeeper_backed(data, &files,
                                      "2067-12-31T23:59:59", NULL);
    fuel_map_ioc3(qts);
    fuel_timekeeper_set_storage(qts, ordinary);
    fuel_write_clock(qts, saved);
    fuel_assert_clock(qts, saved);
    qtest_qmp_assert_success(qts, "{'execute':'quit'}");
    qtest_wait_qemu(qts);
    qtest_quit(qts);

    qts = fuel_start_timekeeper_backed(data, &files,
                                      "2068-01-01T00:00:00", NULL);
    fuel_map_ioc3(qts);
    fuel_timekeeper_assert_storage(qts, ordinary);
    fuel_assert_clock(qts, restored);
    qtest_quit(qts);
    fuel_timekeeper_files_free(&files);
}

int main(int argc, char **argv)
{
    g_autofree char *prom = NULL;
    int status;

    g_test_init(&argc, &argv, NULL);
    prom = fuel_test_prom();
    qtest_add_data_func("fuel/board-and-cpu", prom, test_fuel_board_and_cpu);
    qtest_add_data_func("fuel/processor-prid", NULL,
                        test_fuel_processor_prid);
    qtest_add_data_func("fuel/processor-prid-migration", NULL,
                        test_fuel_processor_prid_migration);
    qtest_add_data_func("fuel/mac-eeprom-machine", prom,
                        test_fuel_mac_eeprom_machine);
    qtest_add_data_func("fuel/cpu-grant", prom, test_fuel_cpu_grant);
    qtest_add_data_func("fuel/board-id-migration", prom,
                        test_fuel_board_id_migration);
    qtest_add_data_func("fuel/pci", prom, test_fuel_pci);
    qtest_add_data_func("fuel/ioc3-identity", prom,
                        test_fuel_ioc3_identity);
    qtest_add_data_func("fuel/ioc3-phy", prom, test_fuel_ioc3_phy);
    qtest_add_data_func("fuel/scsi-interrupt", prom,
                        test_fuel_scsi_interrupt);
    qtest_add_data_func("fuel/ioc3-superio-interrupt", prom,
                        test_fuel_ioc3_superio_interrupt);
    qtest_add_data_func("fuel/ioc3-superio-interrupt-reset", prom,
                        test_fuel_ioc3_superio_interrupt_reset);
    qtest_add_data_func("fuel/ioc3-superio-interrupt-migration", prom,
                        test_fuel_ioc3_superio_interrupt_migration);
    qtest_add_data_func("fuel/ohci-interrupt", prom,
                        test_fuel_ohci_interrupt);
    qtest_add_data_func("fuel/ohci-frame-remaining", prom,
                        test_fuel_ohci_frame_remaining);
    qtest_add_data_func("fuel/ioc3-ethernet-interrupt", prom,
                        test_fuel_ioc3_ethernet_interrupt);
    qtest_add_data_func("fuel/ps2", prom, test_fuel_ps2);
    qtest_add_data_func("fuel/ps2-keyboard-host-input", prom,
                        test_fuel_ps2_keyboard_host_input);
    qtest_add_data_func("fuel/ps2-mouse-host-input", prom,
                        test_fuel_ps2_mouse_host_input);
    qtest_add_data_func("fuel/memory", prom, test_fuel_memory);
    qtest_add_data_func("fuel/rejects-other-board-code", prom,
                        test_fuel_rejects_other_board_code);
    qtest_add_data_func("fuel/requires-l1-type-code", prom,
                        test_fuel_requires_l1_type_code);
    qtest_add_data_func("fuel/rejects-out-of-range-l1-type-code", prom,
                        test_fuel_rejects_out_of_range_l1_type_code);
    qtest_add_data_func("fuel/rejects-zero-l1-type-code", prom,
                        test_fuel_rejects_zero_l1_type_code);
    qtest_add_data_func("fuel/requires-bedrock-revision", prom,
                        test_fuel_requires_bedrock_revision);
    qtest_add_data_func("fuel/rejects-large-bedrock-revision", prom,
                        test_fuel_rejects_large_bedrock_revision);
    qtest_add_data_func("fuel/bedrock-revision-rejects-other-machine", prom,
                        test_fuel_bedrock_revision_rejects_other_machine);
    qtest_add_data_func("fuel/bedrock-revision", prom,
                        test_fuel_bedrock_revision);
    qtest_add_data_func("fuel/l1-type-code-rejects-other-machine", prom,
                        test_fuel_l1_type_code_rejects_other_machine);
    qtest_add_data_func("fuel/prom-copy-boundary", prom,
                        test_fuel_prom_copy_boundary);
    qtest_add_data_func("fuel/serial-backends", prom,
                        test_fuel_serial_backends);
    qtest_add_data_func("fuel/serial-bytes", prom, test_fuel_serial_bytes);
    qtest_add_data_func("fuel/system-timekeeper", prom,
                        test_fuel_system_timekeeper);
    qtest_add_data_func("fuel/system-timekeeper-clock", prom,
                        test_fuel_system_timekeeper_clock);
    qtest_add_data_func("fuel/system-timekeeper-migration/running", prom,
                        test_fuel_timekeeper_migration_running);
    qtest_add_data_func("fuel/system-timekeeper-migration/stopped", prom,
                        test_fuel_timekeeper_migration_stopped);
    qtest_add_data_func("fuel/system-timekeeper-elapsed-rollover", prom,
                        test_fuel_timekeeper_elapsed_rollover);
    qtest_add_data_func("fuel/zero-identity-inputs", prom,
                        test_fuel_zero_identity_inputs);
    qtest_add_data_func("fuel/baseio-identity-properties", prom,
                        test_baseio_identity_properties);
    qtest_add_data_func("fuel/baseio-unsupported-revision", prom,
                        test_baseio_rejects_unsupported_revision);
    status = g_test_run();
    g_assert_cmpint(g_unlink(prom), ==, 0);
    return status;
}
