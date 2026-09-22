/*
 * savevm/quit/cold -loadvm round trip: a different code path from
 * `migrate`, so a defect in it never shows in the migration test.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-test.h"

/* Addresses only this test uses; the shared ones come from sgi-test.h. */
#define BRIDGE_WIDGET           8
#define BRIDGE_BASE             (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))
#define REG_DATA                4
#define REG(offset)             (BRIDGE_BASE + (offset) + REG_DATA)
#define BRIDGE_DEVICE(n)        (0x200 + 8 * (n))

#define MACHINE_WITH_BRIDGE     "-M origin200"

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

/* A 32-bit scratch register in the card's memory-mapped register file. */
#define CARD_SCRATCH            0x34

/* Sixteen megabytes apart, matching sn0-bridge-test.c's own layout. */
#define CARD_MMIO(slot)         (0x10000000u + (slot) * 0x1000000u)

/* Window 1 is the second 2 MB window, and is where slot 4 lands on it. */
#define PROBE_WINDOW             1
#define PROBE_WINDOW_OFFSET      0x400000
#define WINDOW(offset)           (BRIDGE_BASE + (offset))
#define IOC3_SLOT                2
#define IOC3_PCI_MMIO            0x10000000u
#define IOC3_WINDOW_OFFSET       0x600000
#define IOC3_TIMEKEEPER          (BRIDGE_BASE + IOC3_WINDOW_OFFSET + 0x80000)
#define TIMEKEEPER_STORAGE_SIZE  0x7ff8
#define TIMEKEEPER_CONTROL       (IOC3_TIMEKEEPER + 0x7ff8)
#define TIMEKEEPER_WEEKDAY       (IOC3_TIMEKEEPER + 0x7ffc)
#define TIMEKEEPER_DAY           (IOC3_TIMEKEEPER + 0x7ffd)
#define TIMEKEEPER_MONTH         (IOC3_TIMEKEEPER + 0x7ffe)
#define TIMEKEEPER_YEAR          (IOC3_TIMEKEEPER + 0x7fff)
#define TIMEKEEPER_CSR_READ      0x40
#define TIMEKEEPER_CSR_WRITE     0x80

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

static void attach_ioc3(QTestState *qts)
{
    qtest_writel(qts, CFG0(IOC3_SLOT, 0) + PCI_BAR0, IOC3_PCI_MMIO);
    qtest_writel(qts, CFG0(IOC3_SLOT, 0) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, REG(BRIDGE_DEVICE(IOC3_SLOT)),
                 DEV_MEMORY_SPACE | (IOC3_PCI_MMIO >> 20));
}

/*
 * Without sgi_bridge_post_load() re-deriving the window's aim, a loaded
 * machine comes up over the wrong PCI space.
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
    /* Snapshot storage only, attached to no bus. */
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

    /* No guest code runs, so savevm alone is the whole save step. */
    hmp_ret = qtest_hmp(qts, "savevm snaptest");
    g_free(hmp_ret);

    /* A real quit: nothing can leak but the qcow2 file, which is the point. */
    qtest_quit(qts);

    cmdline_restored = g_strdup_printf(MACHINE_WITH_BRIDGE PROBE_CARD
                                       "%s -loadvm snaptest", drive_arg);
    qts = qtest_init(cmdline_restored);

    /* Holds only if the window's derived state came back, not just the bits. */
    g_assert_cmphex(qtest_readl(qts, WINDOW(PROBE_WINDOW_OFFSET) +
                                CARD_SCRATCH), ==, 0xc0ffee00);

    qtest_quit(qts);
}

static void test_timekeeper_image_survives_savevm_loadvm(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *qcow2_path = NULL;
    g_autofree gchar *drive_arg = NULL;
    g_autofree gchar *cmdline = NULL;
    g_autofree uint8_t *expected = g_malloc(TIMEKEEPER_STORAGE_SIZE);
    g_autofree uint8_t *actual = g_malloc(TIMEKEEPER_STORAGE_SIZE);
    GError *err = NULL;
    QTestState *qts;
    char *hmp_ret;
    size_t i;

    if (!have_qemu_img()) {
        g_test_skip("QTEST_QEMU_IMG not set; cannot create snapshot storage");
        return;
    }
    for (i = 0; i < TIMEKEEPER_STORAGE_SIZE; i++) {
        expected[i] = 0xa5 ^ i ^ (i >> 8);
    }

    tmpfs = g_dir_make_tmp("sn0-timekeeper-snapshot-XXXXXX", &err);
    g_assert(tmpfs);
    qcow2_path = g_strdup_printf("%s/state.qcow2", tmpfs);
    g_assert(mkimg(qcow2_path, "qcow2", 128));
    drive_arg = g_strdup_printf(" -drive file=%s,format=qcow2,if=none",
                                qcow2_path);
    cmdline = g_strdup_printf(MACHINE_WITH_BRIDGE "%s", drive_arg);
    qts = qtest_init(cmdline);
    attach_ioc3(qts);
    qtest_memwrite(qts, IOC3_TIMEKEEPER, expected, TIMEKEEPER_STORAGE_SIZE);
    qtest_writeb(qts, TIMEKEEPER_CONTROL, TIMEKEEPER_CSR_WRITE);
    qtest_writeb(qts, TIMEKEEPER_WEEKDAY, 5);
    qtest_writeb(qts, TIMEKEEPER_DAY, 7);
    qtest_writeb(qts, TIMEKEEPER_MONTH, 3);
    qtest_writeb(qts, TIMEKEEPER_YEAR, 0x40);
    qtest_writeb(qts, TIMEKEEPER_CONTROL, 0x15);
    hmp_ret = qtest_hmp(qts, "savevm timekeeper");
    g_free(hmp_ret);
    qtest_quit(qts);

    g_free(cmdline);
    cmdline = g_strdup_printf(MACHINE_WITH_BRIDGE "%s -loadvm timekeeper",
                              drive_arg);
    qts = qtest_init(cmdline);
    qtest_memread(qts, IOC3_TIMEKEEPER, actual, TIMEKEEPER_STORAGE_SIZE);
    g_assert_cmpmem(actual, TIMEKEEPER_STORAGE_SIZE,
                    expected, TIMEKEEPER_STORAGE_SIZE);
    g_assert_cmphex(qtest_readb(qts, TIMEKEEPER_CONTROL), ==, 0x15);
    qtest_writeb(qts, TIMEKEEPER_CONTROL, 0x15 | TIMEKEEPER_CSR_READ);
    g_assert_cmphex(qtest_readb(qts, TIMEKEEPER_WEEKDAY), ==, 5);
    g_assert_cmphex(qtest_readb(qts, TIMEKEEPER_DAY), ==, 7);
    g_assert_cmphex(qtest_readb(qts, TIMEKEEPER_MONTH), ==, 3);
    g_assert_cmphex(qtest_readb(qts, TIMEKEEPER_YEAR), ==, 0x40);
    qtest_clock_step(qts, 1000000000LL);
    qtest_memread(qts, IOC3_TIMEKEEPER, actual, TIMEKEEPER_STORAGE_SIZE);
    g_assert_cmpmem(actual, TIMEKEEPER_STORAGE_SIZE,
                    expected, TIMEKEEPER_STORAGE_SIZE);
    qtest_system_reset(qts);
    attach_ioc3(qts);
    g_assert_cmphex(qtest_readb(qts, TIMEKEEPER_CONTROL), ==, 0x15);
    g_assert_cmphex(qtest_readb(qts, TIMEKEEPER_WEEKDAY), ==, 5);
    qtest_memread(qts, IOC3_TIMEKEEPER, actual, TIMEKEEPER_STORAGE_SIZE);
    g_assert_cmpmem(actual, TIMEKEEPER_STORAGE_SIZE,
                    expected, TIMEKEEPER_STORAGE_SIZE);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/snapshot/device-window-survives-savevm-loadvm",
                        NULL, test_a_device_window_survives_savevm_loadvm);
    qtest_add_data_func("sn0/snapshot/timekeeper-image-survives-savevm-loadvm",
                        NULL, test_timekeeper_image_survives_savevm_loadvm);

    return g_test_run();
}
