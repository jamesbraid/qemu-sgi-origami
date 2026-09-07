/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the AT&T/Lucent two-port PCI OHCI wrapper.
 *
 * The device reuses QEMU's existing PCI OHCI core.  These tests therefore
 * cover the wrapper's PCI identity and port count, plus enough reset and
 * migration state to prove that the subtype retains the parent's lifecycle.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci-pc.h"
#include "qobject/qdict.h"
#include "hw/pci/pci_regs.h"

#define LUCENT_OHCI_MACHINE        "-nodefaults -machine pc"
#define LUCENT_OHCI_SLOT           4
#define LUCENT_OHCI_DEVFN          QPCI_DEVFN(LUCENT_OHCI_SLOT, 0)

#define LUCENT_VENDOR_ID           0x11c1
#define LUCENT_DEVICE_ID           0x5802
#define LUCENT_PCI_REVISION        0x10
#define LUCENT_SUBSYSTEM_VENDOR_ID 0xffff
#define LUCENT_SUBSYSTEM_ID        0xffff

#define APPLE_VENDOR_ID            0x106b
#define APPLE_OHCI_DEVICE_ID       0x003f
#define QEMU_SUBSYSTEM_VENDOR_ID   0x1af4
#define QEMU_SUBSYSTEM_ID          0x1100

/* Register offsets and model values supplied by the reused OHCI core. */
#define OHCI_HC_REVISION           0x00
#define OHCI_HC_COMMAND_STATUS     0x08
#define OHCI_HC_HCCA               0x18
#define OHCI_HC_RH_DESCRIPTOR_A    0x48
#define OHCI_HC_RH_PORT_STATUS(n)  (0x54 + 4 * (n))
#define OHCI_COMMAND_STATUS_HCR    0x00000001
#define OHCI_RH_DESCRIPTOR_A_NDP   0x000000ff
#define OHCI_MODEL_REVISION_1_0    0x00000010
#define OHCI_MODEL_ABSENT_REGISTER 0xffffffff

typedef struct LucentOHCI {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar bar;
    bool bar_mapped;
} LucentOHCI;

static QTestState *lucent_ohci_start(const char *type, const char *extra)
{
    return qtest_initf(LUCENT_OHCI_MACHINE
                       " -device %s,addr=%02x.0%s",
                       type, LUCENT_OHCI_SLOT, extra ? extra : "");
}

static void lucent_ohci_open(LucentOHCI *ohci, QTestState *qts)
{
    ohci->qts = qts;
    ohci->bus = qpci_new_pc(qts, NULL);
    ohci->dev = qpci_device_find(ohci->bus, LUCENT_OHCI_DEVFN);
    g_assert_nonnull(ohci->dev);
}

static void lucent_ohci_map_bar(LucentOHCI *ohci)
{
    uint16_t command;

    command = qpci_config_readw(ohci->dev, PCI_COMMAND);
    qpci_config_writew(ohci->dev, PCI_COMMAND,
                       command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    ohci->bar = qpci_iomap(ohci->dev, 0, NULL);
    ohci->bar_mapped = true;
}

static void lucent_ohci_close(LucentOHCI *ohci)
{
    if (ohci->bar_mapped) {
        qpci_iounmap(ohci->dev, ohci->bar);
    }
    g_free(ohci->dev);
    qpci_free_pc(ohci->bus);
    qtest_quit(ohci->qts);
}

static uint32_t lucent_ohci_readl(LucentOHCI *ohci, uint32_t offset)
{
    return qpci_io_readl(ohci->dev, ohci->bar, offset);
}

static void lucent_ohci_writel(LucentOHCI *ohci, uint32_t offset,
                               uint32_t value)
{
    qpci_io_writel(ohci->dev, ohci->bar, offset, value);
}

static void assert_single_memory_bar(QPCIDevice *dev)
{
    static const uint8_t bars[] = {
        PCI_BASE_ADDRESS_0, PCI_BASE_ADDRESS_1, PCI_BASE_ADDRESS_2,
        PCI_BASE_ADDRESS_3, PCI_BASE_ADDRESS_4, PCI_BASE_ADDRESS_5,
    };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(bars); i++) {
        uint32_t old = qpci_config_readl(dev, bars[i]);
        uint32_t mask;

        qpci_config_writel(dev, bars[i], 0xffffffff);
        mask = qpci_config_readl(dev, bars[i]);
        qpci_config_writel(dev, bars[i], old);

        if (i == 0) {
            g_assert_cmphex(mask & PCI_BASE_ADDRESS_SPACE, ==,
                            PCI_BASE_ADDRESS_SPACE_MEMORY);
            g_assert_cmphex(mask & PCI_BASE_ADDRESS_MEM_MASK, !=, 0);
        } else {
            g_assert_cmphex(mask, ==, 0);
        }
    }
}

static void test_identity_and_window(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));

    g_assert_cmphex(qpci_config_readw(ohci.dev, PCI_VENDOR_ID), ==,
                    LUCENT_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(ohci.dev, PCI_DEVICE_ID), ==,
                    LUCENT_DEVICE_ID);
    g_assert_cmphex(qpci_config_readb(ohci.dev, PCI_REVISION_ID), ==,
                    LUCENT_PCI_REVISION);
    g_assert_cmphex(qpci_config_readb(ohci.dev, PCI_CLASS_PROG), ==, 0x10);
    g_assert_cmphex(qpci_config_readw(ohci.dev, PCI_CLASS_DEVICE), ==, 0x0c03);
    g_assert_cmphex(qpci_config_readb(ohci.dev, PCI_INTERRUPT_PIN), ==, 0x01);
    g_assert_cmphex(qpci_config_readw(ohci.dev, PCI_SUBSYSTEM_VENDOR_ID), ==,
                    LUCENT_SUBSYSTEM_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(ohci.dev, PCI_SUBSYSTEM_ID), ==,
                    LUCENT_SUBSYSTEM_ID);
    assert_single_memory_bar(ohci.dev);

    lucent_ohci_map_bar(&ohci);
    g_assert_false(ohci.bar.is_io);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_REVISION), ==,
                    OHCI_MODEL_REVISION_1_0);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_A) &
                    OHCI_RH_DESCRIPTOR_A_NDP, ==, 2);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(0)), !=,
                    OHCI_MODEL_ABSENT_REGISTER);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(1)), !=,
                    OHCI_MODEL_ABSENT_REGISTER);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(2)), ==,
                    OHCI_MODEL_ABSENT_REGISTER);

    lucent_ohci_close(&ohci);
}

static void test_parent_identity_unchanged(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("pci-ohci", ""));

    g_assert_cmphex(qpci_config_readw(ohci.dev, PCI_VENDOR_ID), ==,
                    APPLE_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(ohci.dev, PCI_DEVICE_ID), ==,
                    APPLE_OHCI_DEVICE_ID);
    g_assert_cmphex(qpci_config_readw(ohci.dev, PCI_SUBSYSTEM_VENDOR_ID), ==,
                    QEMU_SUBSYSTEM_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(ohci.dev, PCI_SUBSYSTEM_ID), ==,
                    QEMU_SUBSYSTEM_ID);
    lucent_ohci_map_bar(&ohci);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_A) &
                    OHCI_RH_DESCRIPTOR_A_NDP, ==, 3);

    lucent_ohci_close(&ohci);
}

static void test_reset_reuse(void)
{
    LucentOHCI ohci = { 0 };
    const uint32_t hcca = 0x12345000;

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);

    lucent_ohci_writel(&ohci, OHCI_HC_HCCA, hcca);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_HCCA), ==, hcca);
    lucent_ohci_writel(&ohci, OHCI_HC_COMMAND_STATUS,
                       OHCI_COMMAND_STATUS_HCR);
    /* This zero is the existing core's model behavior, not a Lucent value. */
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_HCCA), ==, 0);

    lucent_ohci_writel(&ohci, OHCI_HC_HCCA, hcca);
    qtest_qmp_assert_success(ohci.qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(ohci.qts, "RESET");
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_HCCA), ==, 0);

    lucent_ohci_close(&ohci);
}

static void migrate_stopped_vm(QTestState *from, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{'execute': 'migrate', 'arguments': {'uri': %s}}", uri);

    g_test_timer_start();
    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(
            from, "{'execute': 'query-migrate'}");
        g_autofree char *status = g_strdup(qdict_get_str(ret, "status"));

        qobject_unref(ret);
        g_assert_cmpstr(status, !=, "failed");
        if (g_str_equal(status, "completed")) {
            break;
        }
        g_assert(g_test_timer_elapsed() < 30);
        g_usleep(1000);
    }
}

static void test_migration_reuse(void)
{
    g_autofree gchar *tmpdir = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *incoming = NULL;
    g_autoptr(GError) err = NULL;
    LucentOHCI from = { 0 }, to = { 0 };
    const uint32_t hcca = 0x45678000;

    tmpdir = g_dir_make_tmp("lucent-ohci-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_assert_nonnull(tmpdir);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpdir);

    lucent_ohci_open(&from, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&from);
    lucent_ohci_writel(&from, OHCI_HC_HCCA, hcca);
    g_assert_cmphex(lucent_ohci_readl(&from, OHCI_HC_HCCA), ==, hcca);

    incoming = g_strdup_printf(" -incoming %s", uri);
    lucent_ohci_open(&to, lucent_ohci_start("lucent-ohci", incoming));
    migrate_stopped_vm(from.qts, uri);
    lucent_ohci_map_bar(&to);
    g_assert_cmphex(lucent_ohci_readl(&to, OHCI_HC_HCCA), ==, hcca);

    lucent_ohci_close(&to);
    lucent_ohci_close(&from);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/lucent-ohci/identity-and-window",
                   test_identity_and_window);
    qtest_add_func("/lucent-ohci/parent-identity-unchanged",
                   test_parent_identity_unchanged);
    qtest_add_func("/lucent-ohci/reset-reuse", test_reset_reuse);
    qtest_add_func("/lucent-ohci/migration-reuse", test_migration_reuse);

    return g_test_run();
}
