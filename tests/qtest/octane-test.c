/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics-free Octane profile: synthetic RAM and raw flash policy. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

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

static char *flash_path;

static QTestState *octane_start(void)
{
    g_autofree char *quoted = g_shell_quote(flash_path);

    return qtest_initf("-M octane -m 128M -cpu R10000 -bios %s -S",
                       quoted);
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
    qtest_writew(qts, FLASH, 0);
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
    qtest_quit(qts);
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
    qtest_add_func("/octane/serial-a-loopback", test_serial_a_loopback);
    qtest_add_func("/octane/heart-interrupt-levels",
                   test_heart_interrupt_levels);
    int result = g_test_run();
    unlink(flash_path);
    g_free(flash_path);
    return result;
}
