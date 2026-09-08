/*
 * What the I/O bridge puts in the address of a packet it sends, on a machine
 * where the chip is wired straight to one peer: a transfer carries the offset
 * alone, and an interrupt is addressed into the Hub window of the node its
 * own destination register names.
 *
 * The head is built here out of test-only parts rather than taken from a
 * machine, so what these tests hold still is the chip's own arithmetic and
 * not a board's wiring around it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* The bridge's own 16 MB widget face, put clear of every target below. */
#define BRIDGE_BASE     0x40000000ULL
#define BRIDGE          "/machine/peripheral/bridge"

/*
 * Registers sit at a slot's offset plus four: Linux names that address,
 * OpenBSD names the slot and folds the four in.
 */
#define REG(offset)     (BRIDGE_BASE + (offset) + 4)

#define BRIDGE_INT_DEST_UPPER   0x030
#define BRIDGE_INT_DEST_LOWER   0x038
#define BRIDGE_DIRECT_MAP       0x080
#define BRIDGE_INT_STATUS       0x100
#define BRIDGE_INT_ENABLE       0x108
#define BRIDGE_HOST_ERR_FIELD   0x128
#define BRIDGE_INT_HOST(n)      (0x130 + 8 * (n))
#define BRIDGE_DEVICE(n)        (0x200 + 8 * (n))

/* An undefined slot inside the register block: the invalid-address provoke. */
#define UNDEFINED_REG           0x07c
#define INT_INVALID_ADDR        (1u << 24)

/* The windows the chip decodes above its register block. */
#define ATE(n)          (BRIDGE_BASE + 0x010000 + (n) * 8)
#define CFG0(slot)      (BRIDGE_BASE + 0x020000 + (slot) * 0x1000)
#define WINDOW0         (BRIDGE_BASE + 0x200000)

/* The per-slot register's fields these tests set. */
#define DEV_SWAP_TRANSLATED     (1u << 20)
#define DEV_SWAP_DIRECT         (1u << 19)
#define DEV_MEMORY_SPACE        (1u << 12)

/* The transfer source, its registers, and where they are put in PCI space. */
#define CARD_SLOT               0
#define CARD_MMIO               0x01000000u
#define PCI_COMMAND             0x04
#define PCI_COMMAND_MEMORY_MASTER 0x0006
#define PCI_BAR0                0x10
#define DMA_REG_ADDRESS         0x00
#define DMA_REG_LENGTH          0x04
#define DMA_REG_SEED            0x08
#define DMA_REG_GO              0x0c
#define DMA_REG_ADDRESS_HI      0x18

/* The board part the identity page carries; twelve characters, no semicolon. */
#define BOARD_PART              "030-1244-001"

/*
 * The fabric this head writes into is plain system memory, so an address the
 * chip composes is an address these tests can look at. Every transfer below
 * lands at its bare offset, so one target with room for all three of them
 * stands in for the whole fabric.
 */
#define PLAIN_BASE      0x00010000ULL

/*
 * The three transfer routes. Each names a widget of its own and lands at an
 * offset of its own, so a landing says which route put it there and whether
 * the widget it named changed anything.
 */
#define DIRECT_WIDGET   3
#define ATE_WIDGET      5
#define WIDE_WIDGET     0xb
#define DIRECT_OFF      0x18000
#define ATE_OFF         0x19000
#define WIDE_OFF        0x1a000

/* Where a widget number would put a transfer if the address carried one. */
#define WIDGET_SHIFT    24
#define TARGET(widget)  ((uint64_t)(widget) << WIDGET_SHIFT)

/*
 * The Hub window an interrupt packet is addressed into, and the node stride
 * its destination's node number is multiplied by.
 */
#define HUB_WINDOW      0x01800000ULL
#define HUB_INT_MODIFY  0x90
#define NODE_SHIFT      32
#define NODE1_BASE      (HUB_WINDOW + (1ULL << NODE_SHIFT))

/* Where the chip's own error packet lands, clear of the transfers above. */
#define ERR_OFF         0x1c000

/* The pin these tests drive, and the bits its packets carry. */
#define PIN             3
#define PIN_NUMBER      0x45    /* the host bit a device interrupt sets */
#define ERR_NUMBER      0x53    /* and the one the chip's own error sets */
#define INT_SET         0x100   /* data bit 8: raise rather than clear */

#define DMA_LEN         16
#define DIRECT_SEED     0x30
#define ATE_SEED        0x50
#define WIDE_SEED       0x70

/* The head, plus whatever else the caller wants on its own -device line. */
static QTestState *head_start(const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
        "-device sgi-xtalk-test-target,id=plain,size=1M "
        "-device sgi-xtalk-test-target,id=hub,size=1M "
        "-device sgi-xtalk-test-target,id=node1,size=1M "
        "-device sgi-xtalk-test-target,id=w3,size=1M "
        "-device sgi-xtalk-test-target,id=w5,size=1M "
        "-device sgi-xtalk-test-target,id=wb,size=1M "
        "-device sgi-bridge,id=bridge,board-part=" BOARD_PART ","
        "fabric=/machine/unattached/system[0],"
        "fabric-node-shift=" stringify(NODE_SHIFT) "%s "
        "-device sgi-bridge-dma-test,bus=pci,addr=%d",
        extra, CARD_SLOT);

    qtest_sysbus_mmio_map(qts, BRIDGE, 0, BRIDGE_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/plain", 0, PLAIN_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/hub", 0, HUB_WINDOW);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/node1", 0, NODE1_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/w3", 0,
                          TARGET(DIRECT_WIDGET));
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/w5", 0,
                          TARGET(ATE_WIDGET));
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/wb", 0,
                          TARGET(WIDE_WIDGET));
    return qts;
}

/*
 * Give the card its registers, let it master the bus, and aim slot 0's
 * window at them. Both byte-lane bits are set so a transfer carries the
 * bytes it was given: what these tests read is the address, not the order.
 */
static void card_start(QTestState *qts)
{
    qtest_writel(qts, CFG0(CARD_SLOT) + PCI_BAR0, CARD_MMIO);
    qtest_writel(qts, CFG0(CARD_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY_MASTER);
    qtest_writel(qts, REG(BRIDGE_DEVICE(CARD_SLOT)),
                 DEV_SWAP_TRANSLATED | DEV_SWAP_DIRECT | DEV_MEMORY_SPACE |
                 (CARD_MMIO >> 20));
}

/*
 * The direct window takes its widget from bits 23:20 of the direct map
 * register; with the offset field zero and the 512 MB step cleared a PCI
 * address passes through as its own low 31 bits. The one translation entry
 * names a page, a widget of its own, and is marked valid.
 */
static void aim_the_routes(QTestState *qts)
{
    qtest_writel(qts, REG(BRIDGE_DIRECT_MAP), DIRECT_WIDGET << 20);
    qtest_writeq(qts, ATE(0),
                 (uint64_t)ATE_OFF | ((uint64_t)ATE_WIDGET << 8) | 1);
}

static void card_transfer(QTestState *qts, uint64_t pci_addr, uint32_t seed)
{
    qtest_writel(qts, WINDOW0 + DMA_REG_ADDRESS, (uint32_t)pci_addr);
    qtest_writel(qts, WINDOW0 + DMA_REG_ADDRESS_HI, pci_addr >> 32);
    qtest_writel(qts, WINDOW0 + DMA_REG_LENGTH, DMA_LEN);
    qtest_writel(qts, WINDOW0 + DMA_REG_SEED, seed);
    qtest_writel(qts, WINDOW0 + DMA_REG_GO, 1);
}

/* One transfer down each of the three routes, at a seed apiece. */
static void run_the_routes(QTestState *qts)
{
    card_transfer(qts, 0x80000000ULL | DIRECT_OFF, DIRECT_SEED);
    card_transfer(qts, 0x40000000ULL, ATE_SEED);
    card_transfer(qts, ((uint64_t)WIDE_WIDGET << 60) | WIDE_OFF, WIDE_SEED);
}

static void assert_landed(QTestState *qts, uint64_t at, uint32_t seed)
{
    uint8_t seen[DMA_LEN];
    unsigned int i;

    qtest_memread(qts, at, seen, sizeof(seen));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(seed + i));
    }
}

static void assert_untouched(QTestState *qts, uint64_t at)
{
    uint8_t seen[DMA_LEN];
    unsigned int i;

    qtest_memread(qts, at, seen, sizeof(seen));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, 0);
    }
}

/*
 * The three windows a device transfer can take, on a chip with one peer on
 * the other end of its link: each lands at its bare offset and the widget
 * each of them names changes nothing about where it went.
 */
static void test_transfers_land_at_their_offset(void)
{
    QTestState *qts = head_start("");

    card_start(qts);
    aim_the_routes(qts);
    run_the_routes(qts);

    assert_landed(qts, DIRECT_OFF, DIRECT_SEED);
    assert_landed(qts, ATE_OFF, ATE_SEED);
    assert_landed(qts, WIDE_OFF, WIDE_SEED);

    assert_untouched(qts, TARGET(DIRECT_WIDGET) + DIRECT_OFF);
    assert_untouched(qts, TARGET(ATE_WIDGET) + ATE_OFF);
    assert_untouched(qts, TARGET(WIDE_WIDGET) + WIDE_OFF);

    qtest_quit(qts);
}

/* One pin's own host address: a node number and the bit it is to set. */
static void route_the_pin(QTestState *qts, unsigned int node)
{
    qtest_writel(qts, REG(BRIDGE_INT_HOST(PIN)), (node << 8) | PIN_NUMBER);
}

static void drive_the_pin(QTestState *qts, int level)
{
    qtest_set_irq_in(qts, BRIDGE, NULL, PIN, level);
}

/*
 * A pin's packet is addressed into the Hub window of the node its own
 * host-address register names, and carries that register's bit number with
 * data bit 8 set.
 */
static void test_a_pin_packet_addresses_the_named_node(void)
{
    QTestState *qts = head_start("");

    route_the_pin(qts, 0);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 1u << PIN);

    /* Nothing has been sent yet. */
    g_assert_cmphex(qtest_readq(qts, HUB_WINDOW + HUB_INT_MODIFY), ==, 0);

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, HUB_WINDOW + HUB_INT_MODIFY), ==,
                    INT_SET | PIN_NUMBER);

    /* And the status register follows the pin rather than the packet. */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) & (1u << PIN),
                    ==, 1u << PIN);

    qtest_quit(qts);
}

/* The node the register names is multiplied by the node stride, not ignored. */
static void test_a_pin_host_address_selects_a_node(void)
{
    QTestState *qts = head_start("");

    route_the_pin(qts, 1);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 1u << PIN);

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, NODE1_BASE + HUB_INT_MODIFY), ==,
                    INT_SET | PIN_NUMBER);
    g_assert_cmphex(qtest_readq(qts, HUB_WINDOW + HUB_INT_MODIFY), ==, 0);

    qtest_quit(qts);
}

/*
 * The chip's own error interrupt takes its number from the host-error field
 * and its address from the widget-level destination pair. The eight pin
 * registers are the devices' destinations, so one is programmed here to a
 * different node and number and has to stay unused.
 */
static void provoke_the_error(QTestState *qts)
{
    qtest_writel(qts, REG(BRIDGE_INT_DEST_UPPER), 0);
    qtest_writel(qts, REG(BRIDGE_INT_DEST_LOWER), ERR_OFF);
    qtest_writel(qts, REG(BRIDGE_HOST_ERR_FIELD), ERR_NUMBER);
    route_the_pin(qts, 1);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), INT_INVALID_ADDR);

    /* A write to an undefined slot in the register block posts and reports. */
    qtest_writel(qts, BRIDGE_BASE + UNDEFINED_REG, 0);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) &
                    INT_INVALID_ADDR, ==, INT_INVALID_ADDR);
}

static void test_a_chip_error_uses_the_widget_destination(void)
{
    QTestState *qts = head_start("");

    provoke_the_error(qts);
    g_assert_cmphex(qtest_readq(qts, ERR_OFF), ==, INT_SET | ERR_NUMBER);

    /* Not the pin register's node, and not the pin register's number. */
    g_assert_cmphex(qtest_readq(qts, NODE1_BASE + HUB_INT_MODIFY), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/bridge-widget/transfer-offsets",
                   test_transfers_land_at_their_offset);
    qtest_add_func("/sn0/bridge-widget/pin-node",
                   test_a_pin_packet_addresses_the_named_node);
    qtest_add_func("/sn0/bridge-widget/pin-node-stride",
                   test_a_pin_host_address_selects_a_node);
    qtest_add_func("/sn0/bridge-widget/error-destination",
                   test_a_chip_error_uses_the_widget_destination);
    return g_test_run();
}
