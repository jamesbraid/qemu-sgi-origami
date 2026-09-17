/*
 * The inbound PIO translated window: byte-lane-corrected access onto PCI
 * memory space at PCI address zero, at a fixed offset inside the widget's
 * own address space. Exercised directly there, against the bridge's sysbus
 * face, rather than through a Hub big window or a processor -- this is the
 * chip's own address decode, not the fabric path a booted kernel takes to
 * reach it. See the header comment on SGI_BRIDGE_PIO_XLATE_BASE for the
 * hardware evidence a booted IRIX kernel programs a Hub big window onto
 * exactly this widget offset.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define BRIDGE          "/machine/peripheral/bridge"
#define BOARD_PART      "030-1244-001"

/* The windows the chip decodes above its register block. */
#define CFG0(slot)      (0x020000 + (slot) * 0x1000)
#define WINDOW0         0x200000

/*
 * Registers sit at a slot's offset plus four: Linux names that address,
 * OpenBSD names the slot and folds the four in.
 */
#define REG(offset)             ((offset) + 4)
#define BRIDGE_DEVICE(n)        (0x200 + 8 * (n))
#define DEV_MEMORY_SPACE        (1u << 12)

/* A card in slot 0, and where firmware would put its BAR in PCI space. */
#define CARD_SLOT               0
#define CARD_MMIO               0x10000000u
#define PCI_COMMAND             0x04
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_BAR1                0x14

/* Storage behind the card, and the least of the card. */
#define CARD_SCRATCH            0x34

/*
 * Restated from the header rather than included from it -- see the comment
 * on SGI_BRIDGE_PIO_XLATE_BASE: byte-lane-corrected access onto PCI memory
 * space at PCI address 0, at this fixed offset inside the widget.
 */
#define PIO_XLATE_BASE          0x40000000ULL
#define PIO_XLATE_SIZE          0x40000000ULL
#define PIO_XLATE(pci_addr)     (PIO_XLATE_BASE + (pci_addr))

#define PATTERN_THROUGH_WINDOW  0xa5c3f00du
#define PATTERN_THROUGH_XLATE   0x5a3c0ff0u

static QTestState *bridge_start(void)
{
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S "
        "-device sgi-bridge,id=bridge,board-part=" BOARD_PART ","
        "fabric=/machine/unattached/system[0] "
        "-device lsi53c895a,bus=pci,addr=" stringify(CARD_SLOT));

    qtest_sysbus_mmio_map(qts, BRIDGE, 0, 0);
    return qts;
}

/*
 * Give the card its BAR, let its memory space answer, and aim its window
 * at it.
 */
static void card_start(QTestState *qts)
{
    qtest_writel(qts, CFG0(CARD_SLOT) + PCI_BAR1, CARD_MMIO);
    qtest_writel(qts, CFG0(CARD_SLOT) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, REG(BRIDGE_DEVICE(CARD_SLOT)),
                 DEV_MEMORY_SPACE | (CARD_MMIO >> 20));
}

/*
 * A value that lands in PCI memory space through the card's own small
 * window -- the ordinary, already-tested path -- reads back through the new
 * inbound window, and a write through the new window lands where the small
 * window can see it. Either direction, on its own, is evidence of a
 * wired-up register; both together are what a genuine path onto PCI memory
 * space requires and a one-way passthrough would fail.
 *
 * The small window corrects PCI's little-endian byte order for the
 * big-endian CPU it faces (sgi_bridge_window_correct, sgi-bridge.c); the
 * translated window applies the very same correction, since it crosses the
 * same big-endian-processor-onto-little-endian-PCI-bus boundary, just
 * without a slot's own programmed base. The two windows therefore agree on
 * the value exactly, not up to a byte swap.
 */
static void test_pio_xlate_reaches_pci_memory(void)
{
    QTestState *qts = bridge_start();
    uint64_t pci_addr = CARD_MMIO + CARD_SCRATCH;

    card_start(qts);

    qtest_writel(qts, WINDOW0 + CARD_SCRATCH, PATTERN_THROUGH_WINDOW);
    g_assert_cmphex(qtest_readl(qts, WINDOW0 + CARD_SCRATCH), ==,
                    PATTERN_THROUGH_WINDOW);
    g_assert_cmphex(qtest_readl(qts, PIO_XLATE(pci_addr)), ==,
                    PATTERN_THROUGH_WINDOW);

    qtest_writel(qts, PIO_XLATE(pci_addr), PATTERN_THROUGH_XLATE);
    g_assert_cmphex(qtest_readl(qts, WINDOW0 + CARD_SCRATCH), ==,
                    PATTERN_THROUGH_XLATE);

    qtest_quit(qts);
}

/*
 * One byte past the window's declared size is no longer backed by this
 * window -- it is a hole in the widget's own address space, and every
 * unclaimed offset there reads back zero.
 */
static void test_pio_xlate_stops_at_its_declared_size(void)
{
    QTestState *qts = bridge_start();
    uint64_t pci_addr = CARD_MMIO + CARD_SCRATCH;

    card_start(qts);
    qtest_writel(qts, WINDOW0 + CARD_SCRATCH, PATTERN_THROUGH_WINDOW);
    g_assert_cmphex(qtest_readl(qts, PIO_XLATE(pci_addr)), ==,
                    PATTERN_THROUGH_WINDOW);

    g_assert_cmphex(qtest_readl(qts, PIO_XLATE_BASE + PIO_XLATE_SIZE), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/bridge-pio-xlate/reaches-pci-memory",
                   test_pio_xlate_reaches_pci_memory);
    qtest_add_func("/sn0/bridge-pio-xlate/stops-at-its-declared-size",
                   test_pio_xlate_stops_at_its_declared_size);
    return g_test_run();
}
