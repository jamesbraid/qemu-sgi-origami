/*
 * Standalone TI TSB12LV22 revision-01 register contract, TI SLLS290.
 * No PHY, bus, ROM, DMA engine or product profile is present.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "libqos/pci-sgi-bridge.h"
#include "hw/pci/pci_regs.h"
#include "sgi-migrate.h"

#define MACHINE "-nodefaults -M none " QPCI_SGI_BRIDGE_HEAD
#define CARD " -device tsb12lv22,id=ohci,addr=4.0"

typedef struct TestOHCI {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar bar[2];
} TestOHCI;

static TestOHCI open_card(const char *extra)
{
    TestOHCI t = { 0 };

    g_assert_true(qtest_has_device("tsb12lv22"));
    t.qts = qtest_initf(MACHINE CARD " %s", extra);
    t.bus = qpci_init_sgi_bridge(t.qts, NULL);
    t.dev = qpci_device_find(t.bus, QPCI_DEVFN(4, 0));
    g_assert_nonnull(t.dev);
    t.bar[0] = qpci_iomap(t.dev, 0, NULL);
    t.bar[1] = qpci_iomap(t.dev, 1, NULL);
    qpci_config_writew(t.dev, PCI_COMMAND,
                       PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    return t;
}

static void close_card(TestOHCI *t)
{
    g_free(t->dev);
    qpci_free_sgi_bridge(t->bus);
    qtest_quit(t->qts);
}

/* Raw byte buffers pin the card's little-endian interface on MIPS64. */
static uint32_t read_reg(TestOHCI *t, unsigned offset)
{
    uint8_t b[4];

    qpci_memread(t->dev, t->bar[0], offset, b, sizeof(b));
    return ldl_le_p(b);
}

static void write_reg(TestOHCI *t, unsigned offset, uint32_t value)
{
    uint8_t b[4];

    stl_le_p(b, value);
    qpci_memwrite(t->dev, t->bar[0], offset, b, sizeof(b));
}

static bool irq(TestOHCI *t)
{
    /* Observe the actual INTA input at the existing Bridge fixture. */
    return qtest_readl(t->qts, 0x40000104) & (1 << 4);
}

static void test_pci(void)
{
    TestOHCI t = open_card("");
    unsigned i;

    g_assert_cmphex(qpci_config_readl(t.dev, 0), ==, 0x8009104c);
    g_assert_cmphex(qpci_config_readl(t.dev, 8), ==, 0x0c001001);
    g_assert_cmphex(qpci_config_readw(t.dev, 6), ==, 0x0210);
    g_assert_cmphex(qpci_config_readb(t.dev, 0x3d), ==, 1);
    g_assert_cmphex(qpci_config_readw(t.dev, 0x3e), ==, 0x0402);
    g_assert_cmphex(qpci_config_readb(t.dev, 0x34), ==, 0x44);
    g_assert_cmphex(qpci_config_readl(t.dev, 0x44), ==, 0x40010001);
    for (i = 0; i < 2; i++) {
        uint32_t saved = qpci_config_readl(t.dev, 0x10 + 4 * i);

        qpci_config_writel(t.dev, 0x10 + 4 * i, UINT32_MAX);
        g_assert_cmphex(qpci_config_readl(t.dev, 0x10 + 4 * i), ==,
                        0xfffff800);
        qpci_config_writel(t.dev, 0x10 + 4 * i, saved);
    }
    qpci_config_writew(t.dev, 4, 0xffff);
    g_assert_cmphex(qpci_config_readw(t.dev, 4), ==, 0x0156);
    qpci_config_writel(t.dev, 0xf8, 0x12345678);
    g_assert_cmphex(qpci_config_readl(t.dev, 0x2c), ==, 0x12345678);
    qpci_config_writel(t.dev, 0x2c, 0xffffffff);
    g_assert_cmphex(qpci_config_readl(t.dev, 0xf8), ==, 0x12345678);
    qpci_config_writel(t.dev, 0x80, 0xffffffff);
    g_assert_cmphex(qpci_config_readl(t.dev, 0x80), ==, 0);
    qpci_config_writew(t.dev, 0x48, 0x0103);
    g_assert_cmphex(qpci_config_readw(t.dev, 0x48), ==, 0x0100);
    close_card(&t);
}

static void test_pci_decode(void)
{
    TestOHCI t = open_card("");
    QPCIBar old = t.bar[0];
    uint32_t moved = old.addr + 0x10000;

    write_reg(&t, 0x08, 0x123);
    qpci_config_writel(t.dev, PCI_BASE_ADDRESS_0, moved);
    t.bar[0].addr = moved;
    g_assert_cmphex(read_reg(&t, 0x08), ==, 0x123);
    qpci_io_writel(t.dev, old, 0x08, 0x456);
    g_assert_cmphex(read_reg(&t, 0x08), ==, 0x123);

    qpci_config_writew(t.dev, PCI_COMMAND, PCI_COMMAND_MASTER);
    write_reg(&t, 0x08, 0x456);
    qpci_config_writew(t.dev, PCI_COMMAND, PCI_COMMAND_MEMORY);
    g_assert_cmphex(read_reg(&t, 0x08), ==, 0x123);
    write_reg(&t, 0x08, 0x789);
    g_assert_cmphex(read_reg(&t, 0x08), ==, 0x789);
    close_card(&t);
}

/*
 * Malta's GT64120 maps PCI memory with an alias. Unlike the Bridge's
 * 32-bit transport, this existing host lets raw widths reach the card.
 * The index register is little-endian; its big-endian PCI data view swaps.
 */
static void malta_config_write(QTestState *qts, unsigned reg, uint32_t value)
{
    uint8_t bytes[4];

    stl_le_p(bytes, 0x80009000 | reg);
    qtest_memwrite(qts, 0x14000cf8, bytes, sizeof(bytes));
    stl_be_p(bytes, value);
    qtest_memwrite(qts, 0x14000cfc, bytes, sizeof(bytes));
}

static void test_access_width(void)
{
    QTestState *qts = qtest_init("-nodefaults -M malta -bios none"
                               " -device tsb12lv22,addr=12.0");
    const uint64_t bar = 0x12000000;
    uint8_t bytes[8] = { 0, 0, 0, 0, 0xff, 0x0f, 0, 0 };
    uint8_t seen[4];

    malta_config_write(qts, PCI_BASE_ADDRESS_0, bar);
    malta_config_write(qts, PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_memread(qts, bar, seen, sizeof(seen));
    g_assert_cmphex(ldl_le_p(seen), ==, 0x00010000);
    stl_le_p(bytes, 0x123);
    qtest_memwrite(qts, bar + 8, bytes, 4);
    /* An eight-byte access at +4 must not become two accepted words. */
    qtest_memwrite(qts, bar + 4, bytes, sizeof(bytes));
    qtest_memread(qts, bar + 8, seen, sizeof(seen));
    g_assert_cmphex(ldl_le_p(seen), ==, 0x123);
    stl_le_p(bytes, 0x11223344);
    qtest_memwrite(qts, bar + 0xc, bytes, 4);
    memset(bytes, 0xff, sizeof(bytes));
    qtest_memwrite(qts, bar + 0xa, bytes, sizeof(bytes));
    qtest_memread(qts, bar + 0xc, seen, sizeof(seen));
    g_assert_cmphex(ldl_le_p(seen), ==, 0x11223344);
    qtest_memwrite(qts, bar + 8, bytes, 1);
    qtest_memwrite(qts, bar + 8, bytes, 2);
    qtest_memwrite(qts, bar + 9, bytes, 4);
    qtest_memread(qts, bar + 8, seen, sizeof(seen));
    g_assert_cmphex(ldl_le_p(seen), ==, 0x123);
    qtest_quit(qts);
}

static void test_defaults(void)
{
    TestOHCI t = open_card("");
    static const struct { unsigned at; uint32_t value; } values[] = {
        { 0x00, 0x00010000 }, { 0x04, 0 }, { 0x08, 0 },
        { 0x0c, 0 }, { 0x10, 0 }, { 0x14, 0x80000000 },
        { 0x18, 0 }, { 0x1c, 0x31333934 }, { 0x20, 0xa002 },
        { 0x24, 0 }, { 0x28, 0 }, { 0x38, 0 }, { 0x3c, 0 },
        { 0x50, 0 }, { 0x64, 0 }, { 0x68, 0 }, { 0x70, 0 },
        { 0x78, 0 }, { 0x80, 0 }, { 0x88, 0 }, { 0x90, 0 },
        { 0x98, 0 }, { 0xa0, 0 }, { 0xa8, 0 }, { 0xdc, 0 },
        { 0xe0, 0 }, { 0xe8, 0xffc0 }, { 0xec, 0 }, { 0xf0, 0 },
        { 0x100, 0 }, { 0x108, 0 }, { 0x110, 0 }, { 0x118, 0 },
        { 0x180, 0 }, { 0x18c, 0 }, { 0x200, 0 }, { 0x400, 0 },
        { 0x410, 0 },
    };
    unsigned i;

    for (i = 0; i < G_N_ELEMENTS(values); i++) {
        g_assert_cmphex(read_reg(&t, values[i].at), ==, values[i].value);
    }
    g_assert_false(irq(&t));
    close_card(&t);
}

static void test_masks(void)
{
    TestOHCI t = open_card("");
    static const struct { unsigned at; uint32_t expected; } masks[] = {
        { 0x0c, 0xffffffff }, { 0x10, 0xffffffff },
        { 0x18, 0xffffffff }, { 0x20, 0xf8fff0c2 },
        { 0xf0, 0xffffffff },
        { 0x70, 0xffffffff }, { 0x78, 0xffffffff },
        { 0x100, 0xffffffff }, { 0x108, 0xffffffff },
        { 0x110, 0xffffffff }, { 0x118, 0xffffffff },
        { 0x1ac, 0xffffffff }, { 0x1cc, 0xffffffff },
        { 0x1ec, 0xffffffff }, { 0x200, 0xffff0000 },
        { 0x270, 0xffff0000 }, { 0x20c, 0xffffffff },
        { 0x27c, 0xffffffff }, { 0x400, 0xf0000000 },
        { 0x460, 0xf0000000 }, { 0x40c, 0xffffffff },
        { 0x46c, 0xffffffff }, { 0x470, 0xf1ffff7f },
    };
    unsigned i;

    for (i = 0; i < G_N_ELEMENTS(masks); i++) {
        g_test_message("register 0x%x", masks[i].at);
        write_reg(&t, masks[i].at, 0xffffffff);
        g_assert_cmphex(read_reg(&t, masks[i].at), ==, masks[i].expected);
    }
    write_reg(&t, 0xec, 0x3fff);
    g_assert_cmphex(read_reg(&t, 0xec), ==, 0x00000fff);
    write_reg(&t, 0x104, 0xffffffff);

    write_reg(&t, 0x08, 0xffffffff);
    g_assert_cmphex(read_reg(&t, 0x08), ==, 0x0fff);
    write_reg(&t, 0x34, 0xffffffff);
    g_assert_cmphex(read_reg(&t, 0x34), ==, 0xfffffc00);
    write_reg(&t, 0x64, 0xffffffff);
    g_assert_cmphex(read_reg(&t, 0x64), ==, 0xfffff800);
    write_reg(&t, 0x100, 0x12345678);
    write_reg(&t, 0x100, 0);
    g_assert_cmphex(read_reg(&t, 0x104), ==, 0x12345678);
    write_reg(&t, 0x104, 0x00340000);
    g_assert_cmphex(read_reg(&t, 0x100), ==, 0x12005678);
    write_reg(&t, 0x410, 0xffffffff);
    g_assert_cmphex(read_reg(&t, 0x410), ==, 0xf1ffff7f);
    write_reg(&t, 0x18c, 0x11223344);
    write_reg(&t, 0x19c, 0xdeadbeef);
    g_assert_cmphex(read_reg(&t, 0x18c), ==, 0x11223344);
    g_assert_cmphex(read_reg(&t, 0x19c), ==, 0);
    write_reg(&t, 0x480, 0xffffffff);
    g_assert_cmphex(read_reg(&t, 0x480), ==, 0);
    write_reg(&t, 0x50, 0xfffe0000);
    g_assert_cmphex(read_reg(&t, 0x54), ==, 0x404e0000);
    write_reg(&t, 0x54, 0xffffffff);
    g_assert_cmphex(read_reg(&t, 0x50), ==, 0);
    write_reg(&t, 0xe0, 0xffffffff);
    g_assert_cmphex(read_reg(&t, 0xe4), ==, 0x00400600);
    write_reg(&t, 0xe4, 0xffffffff);
    g_assert_cmphex(read_reg(&t, 0xe0), ==, 0);
    write_reg(&t, 0xe8, 0xffffffff);
    g_assert_cmphex(read_reg(&t, 0xe8), ==, 0xffc0);
    write_reg(&t, 0x1c, 0);
    write_reg(&t, 0x120, 0xffffffff);
    g_assert_cmphex(read_reg(&t, 0x1c), ==, 0x31333934);
    g_assert_cmphex(read_reg(&t, 0x120), ==, 0);
    close_card(&t);
}

static void test_interrupts(void)
{
    TestOHCI t = open_card("");

    write_reg(&t, 0x80, 0xc1);
    g_assert_cmphex(read_reg(&t, 0x80), ==, 1);
    g_assert_cmphex(read_reg(&t, 0x84), ==, 0);
    write_reg(&t, 0x88, 1);
    g_assert_false(irq(&t));
    write_reg(&t, 0x88, 0x80000000);
    g_assert_true(irq(&t));
    write_reg(&t, 0x8c, 1);
    g_assert_false(irq(&t));
    g_assert_cmphex(read_reg(&t, 0x80), ==, 1);
    write_reg(&t, 0x90, 0xffffffff);
    g_assert_cmphex(read_reg(&t, 0x90), ==, 0xff);
    write_reg(&t, 0x98, 0x80);
    write_reg(&t, 0x88, 0x40);
    g_assert_true(irq(&t));
    g_assert_cmphex(read_reg(&t, 0x94), ==, 0x80);
    write_reg(&t, 0x84, 0x40);
    g_assert_true(irq(&t));
    write_reg(&t, 0x94, 0x80);
    g_assert_false(irq(&t));
    g_assert_cmphex(read_reg(&t, 0x80), ==, 1);
    write_reg(&t, 0xa0, 0xffffffff);
    g_assert_cmphex(read_reg(&t, 0xa0), ==, 0xf);
    write_reg(&t, 0xa8, 8);
    write_reg(&t, 0x88, 0x80);
    g_assert_true(irq(&t));
    write_reg(&t, 0xac, 8);
    g_assert_false(irq(&t));
    close_card(&t);
}

static void test_resets(void)
{
    TestOHCI t = open_card("");

    write_reg(&t, 0x24, 0);
    write_reg(&t, 0x24, 0x11223344);
    write_reg(&t, 0x28, 0x55667788);
    write_reg(&t, 0x20, 0x5002);
    write_reg(&t, 0x08, 0x123);
    write_reg(&t, 0x80, 1);
    write_reg(&t, 0x88, 0x80000001);
    write_reg(&t, 0x50, 0x10000);
    g_assert_cmphex(read_reg(&t, 0x50), ==, 0);
    g_assert_cmphex(read_reg(&t, 0x24), ==, 0);
    g_assert_cmphex(read_reg(&t, 0x28), ==, 0x55667788);
    write_reg(&t, 0x24, 0xaabbccdd);
    g_assert_cmphex(read_reg(&t, 0x24), ==, 0);
    g_assert_cmphex(read_reg(&t, 0x20), ==, 0x5002);
    g_assert_cmphex(read_reg(&t, 0x08), ==, 0);
    g_assert_false(irq(&t));
    g_assert_cmphex(qpci_config_readw(t.dev, 4), !=, 0);
    qtest_system_reset(t.qts);
    /* PCI reset disables decode; map the same BARs again. */
    t.bar[0] = qpci_iomap(t.dev, 0, NULL);
    t.bar[1] = qpci_iomap(t.dev, 1, NULL);
    qpci_config_writew(t.dev, PCI_COMMAND,
                       PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    g_assert_cmphex(read_reg(&t, 0x20), ==, 0xa002);
    g_assert_cmphex(read_reg(&t, 0x28), ==, 0);
    write_reg(&t, 0x24, 0xaabbccdd);
    g_assert_cmphex(read_reg(&t, 0x24), ==, 0xaabbccdd);
    close_card(&t);
}

static void test_unsupported(void)
{
    TestOHCI t = open_card("");
    uint8_t before[64], after[64];

    memset(before, 0x5a, sizeof(before));
    qtest_memwrite(t.qts, 0x02000000, before, sizeof(before));
    write_reg(&t, 0x18c, 0x02000001);
    write_reg(&t, 0x180, 0x9000);
    write_reg(&t, 0xec, 0xcfff);
    write_reg(&t, 0x04, 0x82000000);
    write_reg(&t, 0x14, 0);
    write_reg(&t, 0xe0, 0x300000);
    qtest_clock_step(t.qts, 1000000000);
    g_assert_cmphex(read_reg(&t, 0x180), ==, 0);
    g_assert_cmphex(read_reg(&t, 0xec), ==, 0);
    g_assert_cmphex(read_reg(&t, 0x04), ==, 0);
    g_assert_cmphex(read_reg(&t, 0x14), ==, 0x80000000);
    g_assert_cmphex(read_reg(&t, 0xe0), ==, 0);
    g_assert_cmphex(read_reg(&t, 0x80), ==, 0);
    g_assert_cmphex(read_reg(&t, 0xe8), ==, 0xffc0);
    qtest_memread(t.qts, 0x02000000, after, sizeof(after));
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    qpci_io_writel(t.dev, t.bar[1], 0, 0xffffffff);
    g_assert_cmphex(qpci_io_readl(t.dev, t.bar[1], 0), ==, 0);
    qpci_io_writeb(t.dev, t.bar[0], 0x08, 0xff);
    g_assert_cmphex(read_reg(&t, 0x08), ==, 0);
    close_card(&t);
}

static void test_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("tsb12lv22-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *incoming = g_strdup_printf("-incoming %s", uri);
    TestOHCI from = open_card("");
    TestOHCI to = open_card(incoming);

    write_reg(&from, 0x24, 0);
    write_reg(&from, 0x28, 0x12345678);
    write_reg(&from, 0x108, 0x11223344);
    write_reg(&from, 0x80, 1);
    write_reg(&from, 0x88, 0x80000001);
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    g_assert_cmphex(read_reg(&to, 0x28), ==, 0x12345678);
    g_assert_cmphex(read_reg(&to, 0x10c), ==, 0x11223344);
    g_assert_true(irq(&to));
    write_reg(&to, 0x24, 0x11223344);
    g_assert_cmphex(read_reg(&to, 0x24), ==, 0);
    write_reg(&to, 0x84, 1);
    g_assert_false(irq(&to));
    close_card(&to);
    close_card(&from);
    unlink(socket);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("tsb12lv22/pci", test_pci);
    qtest_add_func("tsb12lv22/pci-decode", test_pci_decode);
    qtest_add_func("tsb12lv22/access-width", test_access_width);
    qtest_add_func("tsb12lv22/defaults", test_defaults);
    qtest_add_func("tsb12lv22/masks", test_masks);
    qtest_add_func("tsb12lv22/interrupts", test_interrupts);
    qtest_add_func("tsb12lv22/resets", test_resets);
    qtest_add_func("tsb12lv22/unsupported", test_unsupported);
    qtest_add_func("tsb12lv22/migration", test_migration);
    return g_test_run();
}
