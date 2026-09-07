/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the AT&T/Lucent two-port PCI OHCI wrapper.
 *
 * The device reuses QEMU's existing PCI OHCI core.  These tests cover the
 * wrapper's PCI identity and port count, plus enough DMA, interrupt, reset,
 * and migration state to prove that the subtype retains the parent's PCI and
 * lifecycle behavior.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci-pc.h"
#include "qobject/qdict.h"
#include "hw/pci/pci_regs.h"
#include <glib/gstdio.h>

#define LUCENT_OHCI_SLOT           5
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
#define OHCI_HC_CONTROL            0x04
#define OHCI_HC_COMMAND_STATUS     0x08
#define OHCI_HC_INTERRUPT_STATUS   0x0c
#define OHCI_HC_INTERRUPT_ENABLE   0x10
#define OHCI_HC_HCCA               0x18
#define OHCI_HC_CONTROL_HEAD_ED    0x20
#define OHCI_HC_BULK_HEAD_ED       0x28
#define OHCI_HC_FM_NUMBER          0x3c
#define OHCI_HC_PERIODIC_START     0x40
#define OHCI_HC_LS_THRESHOLD       0x44
#define OHCI_HC_RH_DESCRIPTOR_A    0x48
#define OHCI_HC_RH_PORT_STATUS(n)  (0x54 + 4 * (n))

#define OHCI_CONTROL_OPERATIONAL   0x00000080
#define OHCI_CONTROL_SUSPEND       0x000000c0
#define OHCI_COMMAND_STATUS_HCR    0x00000001
#define OHCI_COMMAND_STATUS_CLF    0x00000002
#define OHCI_COMMAND_STATUS_BLF    0x00000004
#define OHCI_INTERRUPT_SF          0x00000004
#define OHCI_INTERRUPT_RHSC        0x00000040
#define OHCI_INTERRUPT_MIE         0x80000000
#define OHCI_PORT_PES              0x00000002
#define OHCI_PORT_CSC              0x00010000
#define OHCI_RH_DESCRIPTOR_A_NDP   0x000000ff
#define OHCI_MODEL_REVISION_1_0    0x00000010
#define OHCI_MODEL_ABSENT_REGISTER 0xffffffff

#define OHCI_HCCA_SIZE             0x100
#define OHCI_HCCA_FRAME_OFFSET     0x80
#define OHCI_DMA_PHYS              0x00100000
#define OHCI_X86_BAR               0xe0000000

/* Origin 200's SGI Bridge supplies the PCI host used by the mips64 test. */
#define SGI_XTALK_SPACE            0x20000000000ULL
#define SGI_BRIDGE_WIDGET          8
#define SGI_BRIDGE_BASE            (SGI_XTALK_SPACE + \
                                    ((uint64_t)SGI_BRIDGE_WIDGET << 24))
#define SGI_BRIDGE_REG_DATA        4
#define SGI_BRIDGE_CFG0            0x020000
#define SGI_BRIDGE_CFG_STRIDE      0x1000
#define SGI_CFG0                   (SGI_BRIDGE_BASE + SGI_BRIDGE_CFG0 + \
                                    LUCENT_OHCI_SLOT * SGI_BRIDGE_CFG_STRIDE)
#define SGI_BRIDGE_DEVICE          (SGI_BRIDGE_BASE + 0x200 + \
                                    8 * LUCENT_OHCI_SLOT + \
                                    SGI_BRIDGE_REG_DATA)
#define SGI_DEVICE_WINDOW          (SGI_BRIDGE_BASE + 0x900000)
#define SGI_DEVICE_MEMORY          (1U << 12)
#define SGI_DEVICE_SWAP_DMA        (1U << 20)
#define OHCI_SGI_BAR               0x10000000
#define SGI_BRIDGE_ATE             (SGI_BRIDGE_BASE + 0x010000)
#define SGI_DMA_IOVA               0x40000000
#define SGI_ATE_VALID              1
#define SGI_ATE_ENTRY(addr)        ((uint64_t)(addr) | \
                                    ((uint64_t)SGI_BRIDGE_WIDGET << 8) | \
                                    SGI_ATE_VALID)

#define MIGRATION_CONTROL_HEAD     0x40000100
#define MIGRATION_BULK_HEAD        0x40000110
#define MIGRATION_PERIODIC_START   0x2a2a
#define MIGRATION_LS_THRESHOLD     0x55aa

typedef struct LucentOHCI {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    uint64_t mmio;
    uint32_t pci_bar;
    bool is_mips64;
} LucentOHCI;

static bool lucent_ohci_is_mips64(void)
{
    return g_str_equal(qtest_get_arch(), "mips64");
}

static QTestState *lucent_ohci_start(const char *type, const char *extra)
{
    const char *machine = lucent_ohci_is_mips64() ?
                          "-nodefaults -machine origin200" :
                          "-nodefaults -machine pc";

    return qtest_initf("%s -device %s,id=ohci,addr=%02x.0%s",
                       machine, type, LUCENT_OHCI_SLOT,
                       extra ? extra : "");
}

static void lucent_ohci_open(LucentOHCI *ohci, QTestState *qts)
{
    ohci->qts = qts;
    ohci->is_mips64 = lucent_ohci_is_mips64();
    if (ohci->is_mips64) {
        ohci->pci_bar = OHCI_SGI_BAR;
        ohci->mmio = SGI_DEVICE_WINDOW;
        return;
    }

    ohci->pci_bar = OHCI_X86_BAR;
    ohci->mmio = OHCI_X86_BAR;
    ohci->bus = qpci_new_pc(qts, NULL);
    ohci->dev = qpci_device_find(ohci->bus, LUCENT_OHCI_DEVFN);
    g_assert_nonnull(ohci->dev);
}

static uint32_t lucent_ohci_config_readl(LucentOHCI *ohci, uint8_t offset)
{
    if (ohci->is_mips64) {
        return qtest_readl(ohci->qts, SGI_CFG0 + (offset & ~3));
    }
    return qpci_config_readl(ohci->dev, offset);
}

static uint16_t lucent_ohci_config_readw(LucentOHCI *ohci, uint8_t offset)
{
    if (ohci->is_mips64) {
        return lucent_ohci_config_readl(ohci, offset) >>
               (8 * (offset & 3));
    }
    return qpci_config_readw(ohci->dev, offset);
}

static uint8_t lucent_ohci_config_readb(LucentOHCI *ohci, uint8_t offset)
{
    if (ohci->is_mips64) {
        return lucent_ohci_config_readl(ohci, offset) >>
               (8 * (offset & 3));
    }
    return qpci_config_readb(ohci->dev, offset);
}

static void lucent_ohci_config_writel(LucentOHCI *ohci, uint8_t offset,
                                      uint32_t value)
{
    if (ohci->is_mips64) {
        qtest_writel(ohci->qts, SGI_CFG0 + offset, value);
    } else {
        qpci_config_writel(ohci->dev, offset, value);
    }
}

static void lucent_ohci_config_writew(LucentOHCI *ohci, uint8_t offset,
                                      uint16_t value)
{
    if (ohci->is_mips64) {
        g_assert_cmphex(offset, ==, PCI_COMMAND);
        qtest_writel(ohci->qts, SGI_CFG0 + offset, value);
    } else {
        qpci_config_writew(ohci->dev, offset, value);
    }
}

static void lucent_ohci_map_bar(LucentOHCI *ohci)
{
    lucent_ohci_config_writel(ohci, PCI_BASE_ADDRESS_0, ohci->pci_bar);
    lucent_ohci_config_writew(ohci, PCI_COMMAND,
                              PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    if (ohci->is_mips64) {
        qtest_writel(ohci->qts, SGI_BRIDGE_DEVICE,
                     SGI_DEVICE_MEMORY | SGI_DEVICE_SWAP_DMA |
                     (ohci->pci_bar >> 20));
    }
}

static void lucent_ohci_configure_dma(LucentOHCI *ohci)
{
    uint8_t hcca[OHCI_HCCA_SIZE] = { 0 };

    if (ohci->is_mips64) {
        qtest_writeq(ohci->qts, SGI_BRIDGE_ATE,
                     SGI_ATE_ENTRY(OHCI_DMA_PHYS));
    }
    qtest_memwrite(ohci->qts, OHCI_DMA_PHYS, hcca, sizeof(hcca));
}

static uint32_t lucent_ohci_hcca_address(const LucentOHCI *ohci)
{
    return ohci->is_mips64 ? SGI_DMA_IOVA : OHCI_DMA_PHYS;
}

static void lucent_ohci_close(LucentOHCI *ohci)
{
    if (!ohci->is_mips64) {
        g_free(ohci->dev);
        qpci_free_pc(ohci->bus);
    }
    qtest_quit(ohci->qts);
}

static uint32_t lucent_ohci_readl(LucentOHCI *ohci, uint32_t offset)
{
    return qtest_readl(ohci->qts, ohci->mmio + offset);
}

static void lucent_ohci_writel(LucentOHCI *ohci, uint32_t offset,
                               uint32_t value)
{
    qtest_writel(ohci->qts, ohci->mmio + offset, value);
}

static bool lucent_ohci_intx_asserted(LucentOHCI *ohci)
{
    return (lucent_ohci_config_readw(ohci, PCI_STATUS) &
            PCI_STATUS_INTERRUPT) != 0;
}

static uint16_t lucent_ohci_dma_frame(LucentOHCI *ohci)
{
    uint8_t frame[2];

    qtest_memread(ohci->qts, OHCI_DMA_PHYS + OHCI_HCCA_FRAME_OFFSET,
                  frame, sizeof(frame));
    return frame[0] | (frame[1] << 8);
}

static void lucent_ohci_step_until_frame(LucentOHCI *ohci)
{
    unsigned int i;

    for (i = 0; i < 64 && lucent_ohci_dma_frame(ohci) == 0; i++) {
        qtest_clock_step_next(ohci->qts);
    }
    g_assert_cmphex(lucent_ohci_dma_frame(ohci), !=, 0);
}

static void assert_single_memory_bar(LucentOHCI *ohci)
{
    static const uint8_t bars[] = {
        PCI_BASE_ADDRESS_0, PCI_BASE_ADDRESS_1, PCI_BASE_ADDRESS_2,
        PCI_BASE_ADDRESS_3, PCI_BASE_ADDRESS_4, PCI_BASE_ADDRESS_5,
    };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(bars); i++) {
        uint32_t old = lucent_ohci_config_readl(ohci, bars[i]);
        uint32_t mask;

        lucent_ohci_config_writel(ohci, bars[i], 0xffffffff);
        mask = lucent_ohci_config_readl(ohci, bars[i]);
        lucent_ohci_config_writel(ohci, bars[i], old);

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

    g_assert_cmphex(lucent_ohci_config_readw(&ohci, PCI_VENDOR_ID), ==,
                    LUCENT_VENDOR_ID);
    g_assert_cmphex(lucent_ohci_config_readw(&ohci, PCI_DEVICE_ID), ==,
                    LUCENT_DEVICE_ID);
    g_assert_cmphex(lucent_ohci_config_readb(&ohci, PCI_REVISION_ID), ==,
                    LUCENT_PCI_REVISION);
    g_assert_cmphex(lucent_ohci_config_readb(&ohci, PCI_CLASS_PROG), ==, 0x10);
    g_assert_cmphex(lucent_ohci_config_readw(&ohci, PCI_CLASS_DEVICE), ==,
                    0x0c03);
    g_assert_cmphex(lucent_ohci_config_readb(&ohci, PCI_INTERRUPT_PIN), ==,
                    0x01);
    g_assert_cmphex(lucent_ohci_config_readw(&ohci,
                                             PCI_SUBSYSTEM_VENDOR_ID), ==,
                    LUCENT_SUBSYSTEM_VENDOR_ID);
    g_assert_cmphex(lucent_ohci_config_readw(&ohci, PCI_SUBSYSTEM_ID), ==,
                    LUCENT_SUBSYSTEM_ID);
    assert_single_memory_bar(&ohci);

    lucent_ohci_map_bar(&ohci);
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

    g_assert_cmphex(lucent_ohci_config_readw(&ohci, PCI_VENDOR_ID), ==,
                    APPLE_VENDOR_ID);
    g_assert_cmphex(lucent_ohci_config_readw(&ohci, PCI_DEVICE_ID), ==,
                    APPLE_OHCI_DEVICE_ID);
    g_assert_cmphex(lucent_ohci_config_readw(&ohci,
                                             PCI_SUBSYSTEM_VENDOR_ID), ==,
                    QEMU_SUBSYSTEM_VENDOR_ID);
    g_assert_cmphex(lucent_ohci_config_readw(&ohci, PCI_SUBSYSTEM_ID), ==,
                    QEMU_SUBSYSTEM_ID);
    lucent_ohci_map_bar(&ohci);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_A) &
                    OHCI_RH_DESCRIPTOR_A_NDP, ==, 3);

    lucent_ohci_close(&ohci);
}

static void test_dma_and_inta_reuse(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);
    lucent_ohci_configure_dma(&ohci);

    lucent_ohci_writel(&ohci, OHCI_HC_HCCA,
                       lucent_ohci_hcca_address(&ohci));
    lucent_ohci_writel(&ohci, OHCI_HC_INTERRUPT_ENABLE,
                       OHCI_INTERRUPT_MIE | OHCI_INTERRUPT_SF);
    g_assert_false(lucent_ohci_intx_asserted(&ohci));

    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, OHCI_CONTROL_OPERATIONAL);
    lucent_ohci_step_until_frame(&ohci);
    g_assert_true(lucent_ohci_intx_asserted(&ohci));

    lucent_ohci_writel(&ohci, OHCI_HC_INTERRUPT_STATUS, OHCI_INTERRUPT_SF);
    g_assert_false(lucent_ohci_intx_asserted(&ohci));
    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, OHCI_CONTROL_SUSPEND);

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
    lucent_ohci_map_bar(&ohci);
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

static void assert_migration_registers(LucentOHCI *ohci)
{
    uint32_t schedule = OHCI_COMMAND_STATUS_CLF | OHCI_COMMAND_STATUS_BLF;

    g_assert_cmphex(lucent_ohci_config_readl(ohci, PCI_BASE_ADDRESS_0) &
                    PCI_BASE_ADDRESS_MEM_MASK, ==, ohci->pci_bar);
    g_assert_cmphex(lucent_ohci_config_readw(ohci, PCI_COMMAND) &
                    (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER), ==,
                    PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    g_assert_cmphex(lucent_ohci_readl(ohci, OHCI_HC_HCCA), ==,
                    lucent_ohci_hcca_address(ohci));
    g_assert_cmphex(lucent_ohci_readl(ohci, OHCI_HC_CONTROL_HEAD_ED), ==,
                    MIGRATION_CONTROL_HEAD);
    g_assert_cmphex(lucent_ohci_readl(ohci, OHCI_HC_BULK_HEAD_ED), ==,
                    MIGRATION_BULK_HEAD);
    g_assert_cmphex(lucent_ohci_readl(ohci, OHCI_HC_COMMAND_STATUS) &
                    schedule, ==, schedule);
    g_assert_cmphex(lucent_ohci_readl(ohci, OHCI_HC_PERIODIC_START), ==,
                    MIGRATION_PERIODIC_START);
    g_assert_cmphex(lucent_ohci_readl(ohci, OHCI_HC_LS_THRESHOLD), ==,
                    MIGRATION_LS_THRESHOLD);
    g_assert_cmphex(lucent_ohci_readl(ohci,
                                      OHCI_HC_RH_PORT_STATUS(0)) &
                    OHCI_PORT_CSC, ==, OHCI_PORT_CSC);
    g_assert_cmphex(lucent_ohci_readl(ohci,
                                      OHCI_HC_RH_PORT_STATUS(1)) &
                    OHCI_PORT_CSC, ==, OHCI_PORT_CSC);
}

static void test_migration_reuse(void)
{
    g_autofree gchar *tmpdir = NULL;
    g_autofree gchar *socket_path = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *incoming = NULL;
    g_autoptr(GError) err = NULL;
    LucentOHCI from = { 0 }, to = { 0 };

    tmpdir = g_dir_make_tmp("usb-lucent-ohci-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_assert_nonnull(tmpdir);
    socket_path = g_build_filename(tmpdir, "migrate.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket_path);

    lucent_ohci_open(&from, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&from);
    lucent_ohci_configure_dma(&from);
    lucent_ohci_writel(&from, OHCI_HC_HCCA,
                       lucent_ohci_hcca_address(&from));
    lucent_ohci_writel(&from, OHCI_HC_CONTROL_HEAD_ED,
                       MIGRATION_CONTROL_HEAD);
    lucent_ohci_writel(&from, OHCI_HC_BULK_HEAD_ED, MIGRATION_BULK_HEAD);
    lucent_ohci_writel(&from, OHCI_HC_COMMAND_STATUS,
                       OHCI_COMMAND_STATUS_CLF | OHCI_COMMAND_STATUS_BLF);
    lucent_ohci_writel(&from, OHCI_HC_PERIODIC_START,
                       MIGRATION_PERIODIC_START);
    lucent_ohci_writel(&from, OHCI_HC_LS_THRESHOLD,
                       MIGRATION_LS_THRESHOLD);
    lucent_ohci_writel(&from, OHCI_HC_RH_PORT_STATUS(0), OHCI_PORT_PES);
    lucent_ohci_writel(&from, OHCI_HC_RH_PORT_STATUS(1), OHCI_PORT_PES);
    lucent_ohci_writel(&from, OHCI_HC_INTERRUPT_STATUS,
                       OHCI_INTERRUPT_RHSC);
    /* Keep active transfers and pending INTA outside this migration claim. */
    lucent_ohci_writel(&from, OHCI_HC_CONTROL, OHCI_CONTROL_OPERATIONAL);
    g_assert_cmphex(lucent_ohci_readl(&from, OHCI_HC_FM_NUMBER), ==, 0);
    g_assert_false(lucent_ohci_intx_asserted(&from));
    assert_migration_registers(&from);

    incoming = g_strdup_printf(" -incoming %s", uri);
    lucent_ohci_open(&to, lucent_ohci_start("lucent-ohci", incoming));
    migrate_stopped_vm(from.qts, uri);

    /*
     * No setup is repeated on the destination: these accesses depend on the
     * migrated PCI BAR, command register, and host mapping.
     */
    assert_migration_registers(&to);
    g_assert_cmphex(lucent_ohci_readl(&to, OHCI_HC_FM_NUMBER), ==, 0);
    g_assert_false(lucent_ohci_intx_asserted(&to));

    /*
     * The EOF timer subsection exists only while the timer is pending.
     * Its restored callback performs the observable HCCA DMA writeback.
     */
    lucent_ohci_step_until_frame(&to);
    lucent_ohci_writel(&to, OHCI_HC_CONTROL, OHCI_CONTROL_SUSPEND);

    lucent_ohci_close(&to);
    lucent_ohci_close(&from);

    if (g_remove(socket_path) < 0) {
        g_assert_cmpint(errno, ==, ENOENT);
    }
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/lucent-ohci/identity-and-window",
                   test_identity_and_window);
    qtest_add_func("/lucent-ohci/parent-identity-unchanged",
                   test_parent_identity_unchanged);
    qtest_add_func("/lucent-ohci/dma-and-inta-reuse",
                   test_dma_and_inta_reuse);
    qtest_add_func("/lucent-ohci/reset-reuse", test_reset_reuse);
    qtest_add_func("/lucent-ohci/migration-reuse", test_migration_reuse);

    return g_test_run();
}
