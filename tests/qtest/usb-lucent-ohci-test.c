/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the AT&T/Lucent two-port PCI OHCI wrapper.
 *
 * The device reuses QEMU's existing PCI OHCI core.  These tests cover the
 * wrapper's PCI identity and port count, plus enough DMA, interrupt, reset,
 * and migration state to prove that the subtype retains the parent's PCI and
 * lifecycle behavior.
 *
 * The controller is soldered onto the Origin 200's base I/O board, so it is
 * driven there, over the Bridge's configuration window, device window and
 * translation entries.  This ran on an x86 PC as well until the fork stopped
 * building SGI devices for that target; the two hosts asserted the same
 * things through different accessors, and the PC is not a machine this part
 * ships on.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "hw/pci/pci_regs.h"
#include "sgi-migrate.h"
#include <glib/gstdio.h>

#define LUCENT_OHCI_SLOT           5

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
#define OHCI_HC_FM_INTERVAL        0x34
#define OHCI_HC_FM_REMAINING       0x38
#define OHCI_HC_FM_NUMBER          0x3c
#define OHCI_HC_PERIODIC_START     0x40
#define OHCI_HC_LS_THRESHOLD       0x44
#define OHCI_HC_RH_DESCRIPTOR_A    0x48
#define OHCI_HC_RH_DESCRIPTOR_B    0x4c
#define OHCI_HC_RH_STATUS          0x50
#define OHCI_HC_RH_PORT_STATUS(n)  (0x54 + 4 * (n))

#define OHCI_CONTROL_OPERATIONAL   0x00000080
#define OHCI_CONTROL_SUSPEND       0x000000c0
#define OHCI_CONTROL_RESUME        0x00000040
#define OHCI_COMMAND_STATUS_HCR    0x00000001
#define OHCI_COMMAND_STATUS_CLF    0x00000002
#define OHCI_COMMAND_STATUS_BLF    0x00000004
#define OHCI_INTERRUPT_SF          0x00000004
#define OHCI_INTERRUPT_UE          0x00000010
#define OHCI_INTERRUPT_RHSC        0x00000040
#define OHCI_INTERRUPT_MIE         0x80000000
#define OHCI_PORT_CCS              0x00000001
#define OHCI_PORT_PES              0x00000002
#define OHCI_PORT_PRS              0x00000010
#define OHCI_PORT_PESC             0x00020000
#define OHCI_PORT_PPS              0x00000100
#define OHCI_PORT_LSDA             0x00000200
#define OHCI_RHS_LPS               0x00000001
#define OHCI_RHS_LPSC              0x00010000
#define OHCI_RHA_PSM               0x00000100
#define OHCI_RHA_NPS               0x00000200
/* The USS-312 datasheet's power-on root-hub descriptors, Tables 45 and 46. */
#define OHCI_RHA_POWER_ON          0x10000902
#define OHCI_RHB_POWER_ON          0x00060000
#define OHCI_PORT_CSC              0x00010000
#define OHCI_RH_DESCRIPTOR_A_NDP   0x000000ff
#define OHCI_MODEL_REVISION_1_0    0x00000010
#define OHCI_MODEL_ABSENT_REGISTER 0xffffffff

#define OHCI_HCCA_SIZE             0x100
#define OHCI_HCCA_FRAME_OFFSET     0x80
#define OHCI_DMA_PHYS              0x00100000

/* Origin 200's SGI Bridge supplies the PCI host used by the mips64 test. */
#define SGI_IO_SPACE               0x20000000000ULL
#define SGI_BRIDGE_WIDGET          8
#define SGI_BRIDGE_BASE            (SGI_IO_SPACE + \
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

/*
 * The reused OHCI core's non-PXA path (what pci-ohci, and so lucent-ohci,
 * always takes) fixes the frame at 1ms; usb_frame_time in hcd-ohci.c is
 * the same constant, kept private to that file.
 */
#define OHCI_FRAME_TIME_NS         1000000

typedef struct LucentOHCI {
    QTestState *qts;
    uint64_t mmio;
    uint32_t pci_bar;
} LucentOHCI;

static QTestState *lucent_ohci_start(const char *type, const char *extra)
{
    return qtest_initf("-nodefaults -machine origin200"
                       " -device %s,id=ohci,addr=%02x.0%s",
                       type, LUCENT_OHCI_SLOT, extra ? extra : "");
}

static void lucent_ohci_open(LucentOHCI *ohci, QTestState *qts)
{
    ohci->qts = qts;
    ohci->pci_bar = OHCI_SGI_BAR;
    ohci->mmio = SGI_DEVICE_WINDOW;
}

static uint32_t lucent_ohci_config_readl(LucentOHCI *ohci, uint8_t offset)
{
    return qtest_readl(ohci->qts, SGI_CFG0 + (offset & ~3));
}

static uint16_t lucent_ohci_config_readw(LucentOHCI *ohci, uint8_t offset)
{
    return lucent_ohci_config_readl(ohci, offset) >> (8 * (offset & 3));
}

static uint8_t lucent_ohci_config_readb(LucentOHCI *ohci, uint8_t offset)
{
    return lucent_ohci_config_readl(ohci, offset) >> (8 * (offset & 3));
}

static void lucent_ohci_config_writel(LucentOHCI *ohci, uint8_t offset,
                                      uint32_t value)
{
    qtest_writel(ohci->qts, SGI_CFG0 + offset, value);
}

static void lucent_ohci_config_writew(LucentOHCI *ohci, uint8_t offset,
                                      uint16_t value)
{
    g_assert_cmphex(offset, ==, PCI_COMMAND);
    qtest_writel(ohci->qts, SGI_CFG0 + offset, value);
}

static void lucent_ohci_map_bar(LucentOHCI *ohci)
{
    lucent_ohci_config_writel(ohci, PCI_BASE_ADDRESS_0, ohci->pci_bar);
    lucent_ohci_config_writew(ohci, PCI_COMMAND,
                              PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(ohci->qts, SGI_BRIDGE_DEVICE,
                 SGI_DEVICE_MEMORY | SGI_DEVICE_SWAP_DMA |
                 (ohci->pci_bar >> 20));
}

static void lucent_ohci_configure_dma(LucentOHCI *ohci)
{
    uint8_t hcca[OHCI_HCCA_SIZE] = { 0 };

    qtest_writeq(ohci->qts, SGI_BRIDGE_ATE, SGI_ATE_ENTRY(OHCI_DMA_PHYS));
    qtest_memwrite(ohci->qts, OHCI_DMA_PHYS, hcca, sizeof(hcca));
}

static uint32_t lucent_ohci_hcca_address(const LucentOHCI *ohci)
{
    return SGI_DMA_IOVA;
}

static void lucent_ohci_close(LucentOHCI *ohci)
{
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

static void test_wacom_input_handler_ready(void)
{
    LucentOHCI ohci = { 0 };
    QDict *response;
    QList *mice;
    const QListEntry *entry;
    bool found = false;

    lucent_ohci_open(&ohci, qtest_initf(
        "-nodefaults -machine origin200"
        " -device lucent-ohci,addr=%02x.0"
        " -device usb-wacom-tablet,bus=usb-bus.0,port=1",
        LUCENT_OHCI_SLOT));

    response = qtest_qmp(ohci.qts, "{'execute': 'query-mice'}");
    mice = qdict_get_qlist(response, "return");
    for (entry = qlist_first(mice); entry; entry = qlist_next(entry)) {
        QDict *mouse = qobject_to(QDict, qlist_entry_obj(entry));

        if (!strcmp(qdict_get_str(mouse, "name"), "QEMU PenPartner tablet")) {
            found = true;
        }
    }
    g_assert_true(found);
    qobject_unref(response);

    lucent_ohci_close(&ohci);
}

static void test_pci_command_status(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));

    g_assert_cmphex(lucent_ohci_config_readl(&ohci, PCI_COMMAND), ==,
                    0x02000000);
    lucent_ohci_config_writew(&ohci, PCI_COMMAND,
                              PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    g_assert_cmphex(lucent_ohci_config_readl(&ohci, PCI_COMMAND), ==,
                    0x02000006);

    qtest_qmp_assert_success(ohci.qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(ohci.qts, "RESET");
    g_assert_cmphex(lucent_ohci_config_readl(&ohci, PCI_COMMAND), ==,
                    0x02000000);

    lucent_ohci_close(&ohci);
}

static void test_dma_error_preserves_pci_status(void)
{
    LucentOHCI ohci = { 0 };
    uint16_t status, error_status, reset_status;

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);
    status = lucent_ohci_config_readw(&ohci, PCI_STATUS);
    g_assert_cmphex(status, ==, PCI_STATUS_DEVSEL_MEDIUM);

    /* No ATE maps this HCCA, so its first DMA read must fail. */
    lucent_ohci_writel(&ohci, OHCI_HC_HCCA, SGI_DMA_IOVA);
    lucent_ohci_writel(&ohci, OHCI_HC_INTERRUPT_ENABLE,
                       OHCI_INTERRUPT_MIE | OHCI_INTERRUPT_UE);
    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, OHCI_CONTROL_OPERATIONAL);
    qtest_clock_step(ohci.qts, OHCI_FRAME_TIME_NS);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_INTERRUPT_STATUS) &
                    OHCI_INTERRUPT_UE, ==, OHCI_INTERRUPT_UE);
    error_status = lucent_ohci_config_readw(&ohci, PCI_STATUS);

    qtest_qmp_assert_success(ohci.qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(ohci.qts, "RESET");
    reset_status = lucent_ohci_config_readw(&ohci, PCI_STATUS);
    g_test_message("PCI Status: initial 0x%x, DMA error 0x%x, reset 0x%x",
                   status, error_status, reset_status);
    g_assert_cmphex(error_status, ==,
                    status | PCI_STATUS_DETECTED_PARITY | PCI_STATUS_INTERRUPT);
    g_assert_cmphex(reset_status, ==, status);

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

/*
 * OpenHCI 1.0a gives every HcInterruptEnable field, MasterInterruptEnable
 * included, a reset value of 0 (section 7.1.5, printed page 115), and a
 * HostControllerReset resets the operational registers it does not except
 * (section 7.1.3, page 112). The Origin 300 IO-8 board test depends on it:
 * it writes 0xc000007f, resets the controller and requires zero back.
 */
static void test_reset_clears_interrupt_enable(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);

    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_INTERRUPT_ENABLE), ==, 0);

    lucent_ohci_writel(&ohci, OHCI_HC_INTERRUPT_ENABLE, 0xc000007f);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_INTERRUPT_ENABLE), ==,
                    0xc000007f);
    lucent_ohci_writel(&ohci, OHCI_HC_COMMAND_STATUS,
                       OHCI_COMMAND_STATUS_HCR);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_INTERRUPT_ENABLE), ==, 0);

    /* The other command bits written with it still reach the controller. */
    lucent_ohci_writel(&ohci, OHCI_HC_COMMAND_STATUS,
                       OHCI_COMMAND_STATUS_CLF);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_COMMAND_STATUS) &
                    OHCI_COMMAND_STATUS_CLF, ==, OHCI_COMMAND_STATUS_CLF);

    lucent_ohci_writel(&ohci, OHCI_HC_INTERRUPT_ENABLE, OHCI_INTERRUPT_MIE);
    qtest_qmp_assert_success(ohci.qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(ohci.qts, "RESET");
    lucent_ohci_map_bar(&ohci);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_INTERRUPT_ENABLE), ==, 0);

    lucent_ohci_close(&ohci);
}

/*
 * OpenHCI 1.0a leaves HcFmInterval's FSLargestDataPacket reset value "TBD"
 * (section 7.3.1), for the part to choose; FrameInterval resets to 0x2edf
 * and FrameIntervalToggle to 0. The Origin 300 IO-8 board test requires
 * the whole register to read 0x2edf after a HostControllerReset, so this
 * part's choice is taken to be zero.
 */
static void test_reset_clears_largest_data_packet(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);

    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_FM_INTERVAL), ==, 0x2edf);

    lucent_ohci_writel(&ohci, OHCI_HC_FM_INTERVAL, 0xa7782ee0);
    lucent_ohci_writel(&ohci, OHCI_HC_COMMAND_STATUS,
                       OHCI_COMMAND_STATUS_HCR);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_FM_INTERVAL), ==, 0x2edf);

    lucent_ohci_writel(&ohci, OHCI_HC_FM_INTERVAL, 0x27782edf);
    qtest_qmp_assert_success(ohci.qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(ohci.qts, "RESET");
    lucent_ohci_map_bar(&ohci);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_FM_INTERVAL), ==, 0x2edf);

    lucent_ohci_close(&ohci);
}

/*
 * HcRhDescriptorB's DeviceRemovable and PortPowerControlMask are written by
 * the driver (OpenHCI 1.0a section 7.4.2), one bit per root-hub port from
 * bit 1 of each half, and a HostControllerReset does not reset the root
 * hub (section 7.1.3), so a written value survives it. This two-port part
 * has bits 1 and 2 of each half. The Origin 300 IO-8 board test writes
 * 0x3ffff, resets the controller and requires 0x20006.
 */
static void test_root_hub_descriptor_b_is_writable(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);

    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_B, 0x3ffff);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_B), ==,
                    0x20006);
    lucent_ohci_writel(&ohci, OHCI_HC_COMMAND_STATUS,
                       OHCI_COMMAND_STATUS_HCR);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_B), ==,
                    0x20006);

    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_B, 0xffffffff);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_B), ==,
                    0x60006);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_B, 0);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_B), ==, 0);

    lucent_ohci_close(&ohci);
}

/*
 * Entering USBRESET keeps HcRhDescriptorB as well. OpenHCI 1.0a lists the
 * root-hub registers as the exception to the registers USBRESET preserves
 * (section 5.1.2.1) without saying what becomes of them, so this rests on
 * the Origin 300 IO-8 board test alone: it writes the register, commands
 * USBRESET through HcControl, resets the controller and requires the
 * written port bits back.
 */
static void test_usb_reset_keeps_root_hub_descriptor_b(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);

    /* A controller reset leaves it in USBSUSPEND, so USBRESET is a change. */
    lucent_ohci_writel(&ohci, OHCI_HC_COMMAND_STATUS,
                       OHCI_COMMAND_STATUS_HCR);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_B, 0x3ffff);
    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, 0x0c);
    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, 0);
    lucent_ohci_writel(&ohci, OHCI_HC_COMMAND_STATUS,
                       OHCI_COMMAND_STATUS_HCR);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_B), ==,
                    0x20006);

    /* The device's own reset returns it to its power-on value. */
    qtest_qmp_assert_success(ohci.qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(ohci.qts, "RESET");
    lucent_ohci_map_bar(&ohci);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_B), ==,
                    OHCI_RHB_POWER_ON);

    lucent_ohci_close(&ohci);
}

/*
 * HcRhDescriptorA's NoPowerSwitching, PowerSwitchingMode,
 * OverCurrentProtectionMode, NoOverCurrentProtection and
 * PowerOnToPowerGoodTime are written by the driver (OpenHCI 1.0a section
 * 7.4.1); NumberDownstreamPorts and DeviceType are read-only. The Origin
 * 300 IO-8 board test writes zero and requires the writable fields back
 * as zero, where the shared core kept NoPowerSwitching set.
 */
static void test_root_hub_descriptor_a_is_writable(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);

    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_A), ==,
                    OHCI_RHA_POWER_ON);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_A, 0);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_A), ==,
                    0x002);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_A, 0xffffffff);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_A), ==,
                    0xff001b02);

    lucent_ohci_close(&ohci);
}

static uint32_t lucent_ohci_port_power(LucentOHCI *ohci, int port)
{
    return lucent_ohci_readl(ohci, OHCI_HC_RH_PORT_STATUS(port)) &
           OHCI_PORT_PPS;
}

/*
 * Per-port power switching, OpenHCI 1.0a sections 7.4.1 to 7.4.4: with
 * NoPowerSwitching clear and PowerSwitchingMode set, a port whose
 * PortPowerControlMask bit is set answers only SetPortPower and
 * ClearPortPower, and one whose bit is clear only SetGlobalPower and
 * ClearGlobalPower. PortPowerStatus reads the port's own power, not a
 * constant one. The Origin 300 IO-8 board test selects this mode with
 * both ports masked, switches each port on and off and reads it back.
 */
static void test_port_power_per_port_switching(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);

    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_A, OHCI_RHA_PSM);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_B, 0x60000);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, 0);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 1), ==, 0);

    lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(0), OHCI_PORT_PPS);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, OHCI_PORT_PPS);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 1), ==, 0);

    /* Both ports masked: the global commands reach neither. */
    lucent_ohci_writel(&ohci, OHCI_HC_RH_STATUS, OHCI_RHS_LPSC);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 1), ==, 0);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_STATUS, OHCI_RHS_LPS);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, OHCI_PORT_PPS);

    lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(0), OHCI_PORT_LSDA);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, 0);

    /* Only the first port masked: the second follows the global commands. */
    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_B, 0x20000);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(1), OHCI_PORT_PPS);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 1), ==, 0);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_STATUS, OHCI_RHS_LPSC);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, 0);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 1), ==, OHCI_PORT_PPS);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(1), OHCI_PORT_LSDA);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 1), ==, OHCI_PORT_PPS);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_STATUS, OHCI_RHS_LPS);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 1), ==, 0);

    lucent_ohci_close(&ohci);
}

/* An attached device becomes visible again after its root port is powered. */
static void test_attached_devices_reconnect_after_port_power_cycle(void)
{
    LucentOHCI ohci = { 0 };
    uint32_t status;
    unsigned int port;

    lucent_ohci_open(&ohci, qtest_initf(
        "-nodefaults -machine origin200"
        " -device lucent-ohci,addr=%02x.0"
        " -device usb-kbd,bus=usb-bus.0,port=1"
        " -device usb-tablet,bus=usb-bus.0,port=2",
        LUCENT_OHCI_SLOT));
    lucent_ohci_map_bar(&ohci);

    for (port = 0; port < 2; port++) {
        status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(port));
        g_assert_cmphex(status & OHCI_PORT_CCS, ==, 0);
    }

    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, OHCI_CONTROL_OPERATIONAL);
    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, 0);
    for (port = 0; port < 2; port++) {
        status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(port));
        g_assert_cmphex(status & OHCI_PORT_CCS, ==, 0);
        lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(port), OHCI_PORT_PPS);
        status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(port));
        g_assert_cmphex(status & (OHCI_PORT_CCS | OHCI_PORT_PPS), ==,
                        OHCI_PORT_CCS | OHCI_PORT_PPS);
        lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(port), OHCI_PORT_CSC);
        lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(port), OHCI_PORT_PRS);
        status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(port));
        g_assert_cmphex(status & OHCI_PORT_PES, ==, OHCI_PORT_PES);
        lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(port), OHCI_PORT_LSDA);
        status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(port));
        g_assert_cmphex(status & (OHCI_PORT_CCS | OHCI_PORT_PES |
                                  OHCI_PORT_PPS), ==, 0);
        g_assert_cmphex(status & (OHCI_PORT_CSC | OHCI_PORT_PESC), ==,
                        OHCI_PORT_CSC | OHCI_PORT_PESC);
        lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(port), OHCI_PORT_PPS);
        status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(port));
        g_assert_cmphex(status & (OHCI_PORT_CCS | OHCI_PORT_PPS), ==,
                        OHCI_PORT_CCS | OHCI_PORT_PPS);
        g_assert_cmphex(status & OHCI_PORT_CSC, ==, OHCI_PORT_CSC);
    }

    lucent_ohci_close(&ohci);
}

/*
 * Global power switching, PowerSwitchingMode clear: only SetGlobalPower
 * and ClearGlobalPower switch the ports, all of them together.
 */
static void test_attached_devices_reconnect_after_global_power(void)
{
    LucentOHCI ohci = { 0 };
    uint32_t status;
    unsigned int port;

    lucent_ohci_open(&ohci, qtest_initf(
        "-nodefaults -machine origin200"
        " -device lucent-ohci,addr=%02x.0"
        " -device usb-kbd,bus=usb-bus.0,port=1"
        " -device usb-tablet,bus=usb-bus.0,port=2",
        LUCENT_OHCI_SLOT));
    lucent_ohci_map_bar(&ohci);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_A, 0);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_STATUS, OHCI_RHS_LPS);
    for (port = 0; port < 2; port++) {
        status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(port));
        g_assert_cmphex(status & (OHCI_PORT_CCS | OHCI_PORT_PPS), ==, 0);
        lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(port), OHCI_PORT_CSC);
    }
    lucent_ohci_writel(&ohci, OHCI_HC_INTERRUPT_STATUS, OHCI_INTERRUPT_RHSC);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_STATUS, OHCI_RHS_LPSC);
    for (port = 0; port < 2; port++) {
        status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(port));
        g_assert_cmphex(status & (OHCI_PORT_CCS | OHCI_PORT_PPS |
                                  OHCI_PORT_CSC), ==,
                        OHCI_PORT_CCS | OHCI_PORT_PPS | OHCI_PORT_CSC);
        lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(port), OHCI_PORT_CSC);
    }
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_INTERRUPT_STATUS) &
                    OHCI_INTERRUPT_RHSC, ==, OHCI_INTERRUPT_RHSC);
    lucent_ohci_writel(&ohci, OHCI_HC_INTERRUPT_STATUS, OHCI_INTERRUPT_RHSC);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_STATUS, OHCI_RHS_LPSC);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_INTERRUPT_STATUS) &
                    OHCI_INTERRUPT_RHSC, ==, 0);
    lucent_ohci_close(&ohci);
}

static void test_attach_while_unpowered(void)
{
    LucentOHCI ohci = { 0 };
    uint32_t status;

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);
    qtest_qmp_device_add(ohci.qts, "usb-kbd", "kbd",
                         "{'bus':'ohci.0','port':'1'}");
    status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(0));
    g_assert_cmphex(status & (OHCI_PORT_CCS | OHCI_PORT_CSC), ==, 0);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(0), OHCI_PORT_PPS);
    status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(0));
    g_assert_cmphex(status & (OHCI_PORT_CCS | OHCI_PORT_CSC), ==,
                    OHCI_PORT_CCS | OHCI_PORT_CSC);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(0), OHCI_PORT_CSC);
    lucent_ohci_writel(&ohci, OHCI_HC_INTERRUPT_STATUS, OHCI_INTERRUPT_RHSC);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(0), OHCI_PORT_LSDA);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(0), OHCI_PORT_CSC);
    lucent_ohci_writel(&ohci, OHCI_HC_INTERRUPT_STATUS, OHCI_INTERRUPT_RHSC);
    qtest_qmp_device_del(ohci.qts, "kbd");
    qtest_qmp_device_add(ohci.qts, "usb-tablet", "tablet",
                         "{'bus':'ohci.0','port':'1'}");
    status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(0));
    g_assert_cmphex(status & (OHCI_PORT_CCS | OHCI_PORT_CSC), ==, 0);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(0), OHCI_PORT_PPS);
    status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(0));
    g_assert_cmphex(status & (OHCI_PORT_CCS | OHCI_PORT_CSC), ==,
                    OHCI_PORT_CCS | OHCI_PORT_CSC);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_INTERRUPT_STATUS) &
                    OHCI_INTERRUPT_RHSC, ==, OHCI_INTERRUPT_RHSC);
    lucent_ohci_close(&ohci);
}

static void test_usb_reset_after_power_off(void)
{
    LucentOHCI ohci = { 0 };
    uint32_t status;
    unsigned int port;

    lucent_ohci_open(&ohci, qtest_initf(
        "-nodefaults -machine origin200"
        " -device lucent-ohci,addr=%02x.0"
        " -device usb-kbd,bus=usb-bus.0,port=1"
        " -device usb-tablet,bus=usb-bus.0,port=2",
        LUCENT_OHCI_SLOT));
    lucent_ohci_map_bar(&ohci);
    for (port = 0; port < 2; port++) {
        lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(port), OHCI_PORT_PPS);
        lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(port), OHCI_PORT_LSDA);
    }
    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, OHCI_CONTROL_OPERATIONAL);
    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, 0);
    for (port = 0; port < 2; port++) {
        status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(port));
        g_assert_cmphex(status & (OHCI_PORT_CCS | OHCI_PORT_PPS |
                                  OHCI_PORT_CSC), ==, 0);
    }
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_INTERRUPT_STATUS) &
                    OHCI_INTERRUPT_RHSC, ==, 0);
    for (port = 0; port < 2; port++) {
        lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(port), OHCI_PORT_PPS);
        status = lucent_ohci_readl(&ohci, OHCI_HC_RH_PORT_STATUS(port));
        g_assert_cmphex(status & (OHCI_PORT_CCS | OHCI_PORT_PPS |
                                  OHCI_PORT_CSC), ==,
                        OHCI_PORT_CCS | OHCI_PORT_PPS | OHCI_PORT_CSC);
    }
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_INTERRUPT_STATUS) &
                    OHCI_INTERRUPT_RHSC, ==, OHCI_INTERRUPT_RHSC);
    lucent_ohci_close(&ohci);
}

static void test_port_power_global_switching(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);

    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_A, 0);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_B, 0x60000);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(0), OHCI_PORT_PPS);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, 0);

    lucent_ohci_writel(&ohci, OHCI_HC_RH_STATUS, OHCI_RHS_LPSC);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, OHCI_PORT_PPS);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 1), ==, OHCI_PORT_PPS);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(1), OHCI_PORT_LSDA);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 1), ==, OHCI_PORT_PPS);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_STATUS, OHCI_RHS_LPS);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, 0);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 1), ==, 0);

    lucent_ohci_close(&ohci);
}

/*
 * NoPowerSwitching set: the ports read powered whatever is written. What a
 * port holds when switching is selected again is not specified, so this
 * does not check it.
 */
static void test_port_power_without_switching(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);

    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_A, OHCI_RHA_PSM);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_B, 0x60000);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, 0);

    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_A, OHCI_RHA_NPS);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, OHCI_PORT_PPS);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(0), OHCI_PORT_LSDA);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_STATUS, OHCI_RHS_LPS);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, OHCI_PORT_PPS);

    lucent_ohci_close(&ohci);
}

/*
 * The part's own power-on root-hub descriptors, from the Lucent USS-312
 * datasheet (Advance Data Sheet Rev. 5, March 2000): HcRhDescriptorA
 * 0x10000902 (Table 45: two ports, per-port power switching and
 * over-current reporting, PowerOnToPowerGoodTime 10h) and HcRhDescriptorB
 * 0x00060000 (Table 46: both ports masked), with every port unpowered
 * (Table 48). A HostControllerReset leaves the root hub alone (OpenHCI
 * 1.0a section 7.1.3); entering USBRESET resets it, and HcRhDescriptorA
 * with it. The Origin 300 IO-8 board test requires both ports unpowered
 * after a controller reset and USBRESET.
 */
static void test_root_hub_power_on_values(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);

    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_A), ==,
                    OHCI_RHA_POWER_ON);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_B), ==,
                    OHCI_RHB_POWER_ON);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, 0);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 1), ==, 0);

    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_A, OHCI_RHA_PSM);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_PORT_STATUS(0), OHCI_PORT_PPS);
    lucent_ohci_writel(&ohci, OHCI_HC_COMMAND_STATUS,
                       OHCI_COMMAND_STATUS_HCR);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_A), ==,
                    OHCI_RHA_PSM | 2);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, OHCI_PORT_PPS);

    /* The controller reset left it in USBSUSPEND, so USBRESET is a change. */
    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, 0);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_A), ==,
                    OHCI_RHA_POWER_ON);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 0), ==, 0);

    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_A, OHCI_RHA_PSM);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_DESCRIPTOR_B, 0);
    lucent_ohci_writel(&ohci, OHCI_HC_RH_STATUS, OHCI_RHS_LPSC);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 1), ==, OHCI_PORT_PPS);
    qtest_qmp_assert_success(ohci.qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(ohci.qts, "RESET");
    lucent_ohci_map_bar(&ohci);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_A), ==,
                    OHCI_RHA_POWER_ON);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_RH_DESCRIPTOR_B), ==,
                    OHCI_RHB_POWER_ON);
    g_assert_cmphex(lucent_ohci_port_power(&ohci, 1), ==, 0);

    lucent_ohci_close(&ohci);
}

/*
 * FrameNumber keeps a driver's write while the controller is not counting
 * frames: the Origin 300 IO-8 board test writes it after a controller reset
 * with HcControl in USBRESUME, and in USBRESET, and requires the value
 * back. OpenHCI 1.0a and the part's own datasheet list the field as
 * read-only to the driver without saying what a write does. A controller
 * reset still clears it.
 */
static void test_frame_number_latches_when_not_operational(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);

    lucent_ohci_writel(&ohci, OHCI_HC_COMMAND_STATUS,
                       OHCI_COMMAND_STATUS_HCR);
    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, OHCI_CONTROL_RESUME);
    lucent_ohci_writel(&ohci, OHCI_HC_FM_NUMBER, 0xffff);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_FM_NUMBER), ==, 0xffff);
    lucent_ohci_writel(&ohci, OHCI_HC_FM_NUMBER, 0x00011234);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_FM_NUMBER), ==, 0x1234);

    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, 0);
    lucent_ohci_writel(&ohci, OHCI_HC_FM_NUMBER, 0xa5a5);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_FM_NUMBER), ==, 0xa5a5);

    lucent_ohci_writel(&ohci, OHCI_HC_COMMAND_STATUS,
                       OHCI_COMMAND_STATUS_HCR);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_FM_NUMBER), ==, 0);

    lucent_ohci_close(&ohci);
}

/* While the controller counts frames, a write still leaves the count alone. */
static void test_frame_number_write_ignored_while_operational(void)
{
    LucentOHCI ohci = { 0 };

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);
    lucent_ohci_configure_dma(&ohci);

    lucent_ohci_writel(&ohci, OHCI_HC_HCCA, lucent_ohci_hcca_address(&ohci));
    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, OHCI_CONTROL_OPERATIONAL);
    lucent_ohci_writel(&ohci, OHCI_HC_FM_NUMBER, 0x8000);
    qtest_clock_step_next(ohci.qts);
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_FM_NUMBER), <, 0x8000);

    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, OHCI_CONTROL_SUSPEND);
    lucent_ohci_close(&ohci);
}

static void test_frame_number_tracks_clock_while_timer_starved(void)
{
    LucentOHCI ohci = { 0 };
    uint32_t remaining, interval;
    unsigned int i;

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);
    lucent_ohci_configure_dma(&ohci);

    lucent_ohci_writel(&ohci, OHCI_HC_HCCA, lucent_ohci_hcca_address(&ohci));
    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, OHCI_CONTROL_OPERATIONAL);

    /*
     * Stop the VM. pause_all_vcpus() disables QEMU_CLOCK_VIRTUAL's timer
     * service (qemu_clock_enable(..., false) in system/cpus.c) without
     * freezing the clock's own value: qtest_clock_step() below still
     * advances qemu_clock_get_ns(), but timerlist_run_timers() now
     * bails out before running eof_timer's callback. That reproduces,
     * deterministically, the exact gap the PROM hits under host
     * scheduling pressure: the clock has passed the frame deadline, but
     * nothing has advanced frame_number yet.
     */
    qtest_qmp_assert_success(ohci.qts, "{'execute': 'stop'}");

    /* One warp across three frame deadlines; no timer callback runs. */
    qtest_clock_step(ohci.qts, 3 * OHCI_FRAME_TIME_NS);

    /*
     * Unfixed: frame_number only ever advances inside the eof_timer
     * callback, so this reads back 0. Fixed: HcFmNumber catches the
     * frame counter up to the clock at read time.
     */
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_FM_NUMBER), ==, 3);
    g_assert_cmphex(lucent_ohci_dma_frame(&ohci), ==, 3);

    /*
     * Frame remaining must reflect the caught-up frame, not the stale one:
     * its count, bits 13:0, lies within the frame interval.
     */
    remaining = lucent_ohci_readl(&ohci, OHCI_HC_FM_REMAINING);
    interval = lucent_ohci_readl(&ohci, OHCI_HC_FM_INTERVAL);
    /* Field to field: the words' upper bits are unrelated fields. */
    g_assert_cmphex(remaining & 0x3fff, <=, interval & 0x3fff);

    /*
     * Resume the clock and let the real timer run. It must find nothing
     * left to do for the three frames the read-time catch-up already
     * processed, and advance exactly one more frame, not four (a naive
     * "always advance" catch-up would double count here).
     */
    qtest_qmp_assert_success(ohci.qts, "{'execute': 'cont'}");
    for (i = 0; i < 64 &&
         lucent_ohci_readl(&ohci, OHCI_HC_FM_NUMBER) == 3; i++) {
        qtest_clock_step_next(ohci.qts);
    }
    g_assert_cmphex(lucent_ohci_readl(&ohci, OHCI_HC_FM_NUMBER), ==, 4);

    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, OHCI_CONTROL_SUSPEND);
    lucent_ohci_close(&ohci);
}

/*
 * Register-contract invariant, independent of the read-time catch-up:
 * with the clock running normally (no 'stop'), the real eof_timer
 * services every frame deadline as qtest_clock_step_next() crosses it,
 * so a read must never report fewer frames than have actually elapsed.
 * This holds whether or not the read-time catch-up above exists; it
 * guards against a future change making HcFmNumber lag or double count.
 */
static void test_frame_number_never_lags_elapsed_time(void)
{
    LucentOHCI ohci = { 0 };
    unsigned int i;

    lucent_ohci_open(&ohci, lucent_ohci_start("lucent-ohci", ""));
    lucent_ohci_map_bar(&ohci);
    lucent_ohci_configure_dma(&ohci);

    lucent_ohci_writel(&ohci, OHCI_HC_HCCA, lucent_ohci_hcca_address(&ohci));
    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, OHCI_CONTROL_OPERATIONAL);

    for (i = 0; i < 5; i++) {
        int64_t now = qtest_clock_step_next(ohci.qts);
        uint32_t frame_number = lucent_ohci_readl(&ohci, OHCI_HC_FM_NUMBER);
        uint32_t elapsed_frames = now / OHCI_FRAME_TIME_NS;

        g_assert_cmphex(frame_number, >=, elapsed_frames);
    }

    lucent_ohci_writel(&ohci, OHCI_HC_CONTROL, OHCI_CONTROL_SUSPEND);
    lucent_ohci_close(&ohci);
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
    sgi_migrate_stopped(from.qts, uri);

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
    qtest_add_func("/lucent-ohci/wacom-input-handler-ready",
                   test_wacom_input_handler_ready);
    qtest_add_func("/lucent-ohci/pci-command-status",
                   test_pci_command_status);
    qtest_add_func("/lucent-ohci/dma-error-preserves-pci-status",
                   test_dma_error_preserves_pci_status);
    qtest_add_func("/lucent-ohci/dma-and-inta-reuse",
                   test_dma_and_inta_reuse);
    qtest_add_func("/lucent-ohci/reset-reuse", test_reset_reuse);
    qtest_add_func("/lucent-ohci/reset-clears-interrupt-enable",
                   test_reset_clears_interrupt_enable);
    qtest_add_func("/lucent-ohci/reset-clears-largest-data-packet",
                   test_reset_clears_largest_data_packet);
    qtest_add_func("/lucent-ohci/root-hub-descriptor-b-is-writable",
                   test_root_hub_descriptor_b_is_writable);
    qtest_add_func("/lucent-ohci/usb-reset-keeps-root-hub-descriptor-b",
                   test_usb_reset_keeps_root_hub_descriptor_b);
    qtest_add_func("/lucent-ohci/root-hub-descriptor-a-is-writable",
                   test_root_hub_descriptor_a_is_writable);
    qtest_add_func("/lucent-ohci/root-hub-power-on-values",
                   test_root_hub_power_on_values);
    qtest_add_func("/lucent-ohci/port-power-per-port-switching",
                   test_port_power_per_port_switching);
    qtest_add_func("/lucent-ohci/attached-port-reconnect",
                   test_attached_devices_reconnect_after_port_power_cycle);
    qtest_add_func("/lucent-ohci/attached-global-power",
                   test_attached_devices_reconnect_after_global_power);
    qtest_add_func("/lucent-ohci/attach-while-unpowered",
                   test_attach_while_unpowered);
    qtest_add_func("/lucent-ohci/usb-reset-after-power-off",
                   test_usb_reset_after_power_off);
    qtest_add_func("/lucent-ohci/port-power-global-switching",
                   test_port_power_global_switching);
    qtest_add_func("/lucent-ohci/port-power-without-switching",
                   test_port_power_without_switching);
    qtest_add_func("/lucent-ohci/frame-number-tracks-clock-while-timer-starved",
                   test_frame_number_tracks_clock_while_timer_starved);
    qtest_add_func("/lucent-ohci/frame-number-never-lags-elapsed-time",
                   test_frame_number_never_lags_elapsed_time);
    qtest_add_func("/lucent-ohci/frame-number-latches-when-not-operational",
                   test_frame_number_latches_when_not_operational);
    qtest_add_func("/lucent-ohci/frame-number-write-ignored-while-operational",
                   test_frame_number_write_ignored_while_operational);
    qtest_add_func("/lucent-ohci/migration-reuse", test_migration_reuse);

    return g_test_run();
}
