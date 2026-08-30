/*
 * The savevm/quit/fresh-process/-loadvm round trip -- a different code
 * path from `migrate` (savevm.c's qcow2 snapshot storage, a cold second
 * process), so a defect in it never shows in the migration test. The
 * Bridge device-window case is reused deliberately: the windows' aim is
 * derived from the per-slot register on load, so carrying the register
 * bits is a weaker claim than the access through the window still
 * reaching the card, which is what this asserts.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* This file's own copies, the way every sn0-*-test.c stands alone. */
#define SPACE_XTALK             0x20000000000ULL
#define BRIDGE_WIDGET           8
#define BRIDGE_BASE             (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))
#define REG_DATA                4
#define REG(offset)             (BRIDGE_BASE + (offset) + REG_DATA)
#define BRIDGE_DEVICE(n)        (0x200 + 8 * (n))
#define DEV_MEMORY_SPACE        (1u << 12)

#define MACHINE_WITH_BRIDGE     "-M origin200,io-bridge=on"

/* PCI type 0 configuration space, and the window each device sits behind. */
#define BRIDGE_CFG0             0x020000
#define CFG0_DEVICE_STRIDE      0x1000
#define CFG0_FUNCTION_STRIDE    0x100
#define CFG0(device, function)  (BRIDGE_BASE + BRIDGE_CFG0 + \
                                 (device) * CFG0_DEVICE_STRIDE + \
                                 (function) * CFG0_FUNCTION_STRIDE)

/* Any card works for a window test; this one needs no backing or net. */
#define PROBE_SLOT              4
#define PROBE_CARD              " -device lsi53c895a,addr=" stringify(PROBE_SLOT)

#define PCI_COMMAND             0x04
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_BAR1                0x14

/* A 32-bit scratch register in the card's memory-mapped register file. */
#define CARD_SCRATCH            0x34

/* Sixteen megabytes apart, matching sn0-bridge-test.c's own layout. */
#define CARD_MMIO(slot)         (0x10000000u + (slot) * 0x1000000u)

/* Window 1 is the second 2 MB window, and is where slot 4 lands on it. */
#define PROBE_WINDOW             1
#define PROBE_WINDOW_OFFSET      0x400000
#define WINDOW(offset)           (BRIDGE_BASE + (offset))

static void bridge_put_card_in_slot(QTestState *qts, int slot)
{
    qtest_writel(qts, CFG0(slot, 0) + PCI_BAR1, CARD_MMIO(slot));
    qtest_writel(qts, CFG0(slot, 0) + PCI_COMMAND, PCI_COMMAND_MEMORY);
}

static void bridge_aim_window_at_card(QTestState *qts, int window, int slot)
{
    qtest_writel(qts, REG(BRIDGE_DEVICE(window)),
                DEV_MEMORY_SPACE | (CARD_MMIO(slot) >> 20));
}

/*
 * Aim a window at a card, savevm, quit, cold -loadvm, and require the
 * same access to still reach the card: without sgi_bridge_post_load()
 * re-deriving the window's aim, it comes up over the wrong PCI space.
 */
static void test_a_device_window_survives_savevm_loadvm(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *qcow2_path = NULL;
    g_autofree gchar *drive_arg = NULL;
    g_autofree gchar *cmdline_source = NULL;
    g_autofree gchar *cmdline_restored = NULL;
    GError *err = NULL;
    QTestState *qts;
    char *hmp_ret;

    if (!have_qemu_img()) {
        g_test_skip("QTEST_QEMU_IMG not set; cannot create snapshot storage");
        return;
    }

    tmpfs = g_dir_make_tmp("sn0-snapshot-test-XXXXXX", &err);
    g_assert(tmpfs);
    qcow2_path = g_strdup_printf("%s/state.qcow2", tmpfs);
    /*
     * Snapshot storage only -- never attached to any bus, so the guest
     * (and the qtest accelerator, which runs no guest code at all) never
     * sees it as a device. `savevm` just needs somewhere with internal
     * snapshot support to write into.
     */
    g_assert(mkimg(qcow2_path, "qcow2", 128));
    drive_arg = g_strdup_printf(" -drive file=%s,format=qcow2,if=none",
                                qcow2_path);

    cmdline_source = g_strdup_printf(MACHINE_WITH_BRIDGE PROBE_CARD "%s",
                                     drive_arg);
    qts = qtest_init(cmdline_source);

    bridge_put_card_in_slot(qts, PROBE_SLOT);
    bridge_aim_window_at_card(qts, PROBE_WINDOW, PROBE_SLOT);

    qtest_writel(qts, WINDOW(PROBE_WINDOW_OFFSET) + CARD_SCRATCH, 0xc0ffee00);
    g_assert_cmphex(qtest_readl(qts, WINDOW(PROBE_WINDOW_OFFSET) +
                                CARD_SCRATCH), ==, 0xc0ffee00);

    /*
     * qtest's own accelerator never runs guest code, so there is no
     * "running" state to stop for consistency the way a live TCG guest
     * would need -- `savevm` alone is the whole save step here.
     */
    hmp_ret = qtest_hmp(qts, "savevm snaptest");
    g_free(hmp_ret);

    /*
     * The actual quit, not just dropping the handle: everything about the
     * first process -- its memory, its device objects, its open sockets --
     * is gone before the second one starts. Nothing can leak between them
     * except through the qcow2 file, which is the point.
     */
    qtest_quit(qts);

    cmdline_restored = g_strdup_printf(MACHINE_WITH_BRIDGE PROBE_CARD
                                       "%s -loadvm snaptest", drive_arg);
    qts = qtest_init(cmdline_restored);

    /*
     * The actual claim: a fresh process, having only ever loaded the
     * snapshot, reaches the same card through the same window -- which
     * only holds if the window's derived state, not just the register a
     * guest wrote, came back correct.
     */
    g_assert_cmphex(qtest_readl(qts, WINDOW(PROBE_WINDOW_OFFSET) +
                                CARD_SCRATCH), ==, 0xc0ffee00);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/snapshot/device-window-survives-savevm-loadvm",
                        NULL, test_a_device_window_survives_savevm_loadvm);

    return g_test_run();
}
