/*
 * What the I/O bridge puts in the address of a packet it sends. Crosstalk
 * carries the destination widget in a packet header beside a 48-bit address,
 * and a switch routes on it. A QEMU address space is one flat space with no
 * header, so a machine with more than one widget to reach spends four bits of
 * the address on that number instead, and a machine with a single peer on the
 * other end of the link spends none.
 *
 * The head is built here out of test-only parts rather than taken from a
 * machine, so the two arrangements differ by one property and nothing else.
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

#define BRIDGE_IDENTIFICATION   0x000
#define BRIDGE_INT_DEST_UPPER   0x030
#define BRIDGE_INT_DEST_LOWER   0x038
#define BRIDGE_DIRECT_MAP       0x080
#define BRIDGE_INT_STATUS       0x100
#define BRIDGE_INT_ENABLE       0x108
#define BRIDGE_INT_MODE         0x118
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

/* Enough of the identity register to say a head came up and answered. */
#define ID_PART_SHIFT           12
#define ID_PART                 0xc002

/*
 * The fabric this head writes into is plain system memory, so an address the
 * chip composes is an address these tests can look at. With no widget field
 * in it every transfer lands at its bare offset, so one target with room for
 * all three of them stands in for the whole fabric.
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

/*
 * Two shifts. 24 is where a widget's own memory-mapped face sits inside its
 * crosstalk space, which puts the widget field low enough that an ordinary
 * transfer offset can reach it -- the overflow cases below need that. 34 is
 * what the machine's own address map uses, so the routing case runs at both.
 */
#define WIDGET_SHIFT    24
#define MACHINE_SHIFT   34

/*
 * An address bit inside the widget field at 24, and the widget it composes
 * to: widget 3 with that bit set names widget 7, which is where a chip that
 * merged the two instead of refusing would put the packet.
 */
#define SPILL_BIT       (1ULL << 26)
#define SPILL_WIDGET    (DIRECT_WIDGET | (unsigned)(SPILL_BIT >> WIDGET_SHIFT))

/*
 * The Hub window an interrupt packet is addressed into with no widget field,
 * and the node stride its destination's node number is multiplied by.
 */
#define HUB_WINDOW      0x01800000ULL
#define HUB_INT_MODIFY  0x90
#define NODE_SHIFT      32
#define NODE1_BASE      (HUB_WINDOW + (1ULL << NODE_SHIFT))

/* Where the interrupt packets land, clear of the transfers above. */
#define PIN_OFF         0x02000
#define ERR_OFF         0x1c000

/*
 * The destination's upper register holds crosstalk address bits 47:32. Its
 * top ten are the ones a pin's own host-address register replaces, so the
 * tests fill those to tell replacing from merging, and set one below them so
 * a dropped upper half is a wrong address rather than the same one. Both sit
 * clear of the widget field at either shift.
 */
#define DEST_REPLACED   0xffc0u         /* upper 15:6, address 47:38 */
#define DEST_CARRIED    0x0002u         /* upper 1, address 33 */
#define DEST_UPPER      (DEST_REPLACED | DEST_CARRIED)
#define CARRIED_BIT     (1ULL << 33)
#define REPLACED_BIT    (1ULL << 38)

/* The pin these tests drive, and the bits its packets carry. */
#define PIN             3
#define PIN_NUMBER      0x45    /* the host bit a device interrupt sets */
#define ERR_NUMBER      0x53    /* and the one the chip's own error sets */
#define INT_SET         0x100   /* data bit 8: raise rather than clear */

/* A word nothing here writes, so "no packet" is an assertion of its own. */
#define SENTINEL        0xfeedfacecafebeefULL

/* The guest-error line a refusal owes, so that it is observable. */
#define REFUSED_FRAGMENT "reaches the destination widget"

#define DMA_LEN         16
#define DIRECT_SEED     0x30
#define ATE_SEED        0x50
#define WIDE_SEED       0x70

/*
 * Where the head under test keeps a widget number. head_start_with is its
 * only writer, so every address below is composed the way that head does.
 */
static unsigned int fabric_shift;

static uint64_t target(unsigned int widget)
{
    return (uint64_t)widget << fabric_shift;
}

/*
 * The two places a composed destination lands: the address the guest
 * programmed under its own widget, and the same with the top ten address
 * bits carrying what a pin's host-address register put there.
 */
static uint64_t dest_base(void)
{
    return target(WIDE_WIDGET) + CARRIED_BIT;
}

static uint64_t replaced_base(void)
{
    return dest_base() + REPLACED_BIT;
}

/*
 * The head, and whatever else the caller wants on the command line. A zero
 * shift asks for no widget field at all; the targets are laid out as though
 * one were there regardless, so a test can say a transfer did not reach them.
 */
static QTestState *head_start_with(unsigned int widget_shift, const char *extra)
{
    g_autofree char *option = widget_shift ?
        g_strdup_printf(",fabric-widget-shift=%u", widget_shift) : g_strdup("");
    QTestState *qts;

    fabric_shift = widget_shift ? widget_shift : WIDGET_SHIFT;
    qts = qtest_initf("-M none -cpu R10000 -S "
        "-device sgi-xtalk-test-target,id=plain,size=1M "
        "-device sgi-xtalk-test-target,id=hub,size=1M "
        "-device sgi-xtalk-test-target,id=node1,size=1M "
        "-device sgi-xtalk-test-target,id=w3,size=1M "
        "-device sgi-xtalk-test-target,id=w5,size=1M "
        "-device sgi-xtalk-test-target,id=w7,size=1M "
        "-device sgi-xtalk-test-target,id=wb,size=1M "
        "-device sgi-xtalk-test-target,id=pin,size=1M "
        "-device sgi-xtalk-test-target,id=replaced,size=1M "
        "-device sgi-bridge,id=bridge,board-part=" BOARD_PART ","
        "fabric=/machine/unattached/system[0],"
        "fabric-node-shift=" stringify(NODE_SHIFT) "%s "
        "-device sgi-bridge-dma-test,bus=pci,addr=%d %s",
        option, CARD_SLOT, extra);

    qtest_sysbus_mmio_map(qts, BRIDGE, 0, BRIDGE_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/plain", 0, PLAIN_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/hub", 0, HUB_WINDOW);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/node1", 0, NODE1_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/w3", 0,
                          target(DIRECT_WIDGET));
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/w5", 0,
                          target(ATE_WIDGET));
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/w7", 0,
                          target(SPILL_WIDGET));
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/wb", 0,
                          target(WIDE_WIDGET));
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/pin", 0, dest_base());
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/replaced", 0,
                          replaced_base());
    return qts;
}

static QTestState *head_start(unsigned int widget_shift)
{
    return head_start_with(widget_shift, "");
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
 * The three windows a device transfer can take, on a head that keeps no
 * widget field: each lands at its bare offset and the widget each of them
 * names changes nothing about where it went.
 */
static void test_transfers_land_at_their_offset(void)
{
    QTestState *qts = head_start(0);

    card_start(qts);
    aim_the_routes(qts);
    run_the_routes(qts);

    assert_landed(qts, DIRECT_OFF, DIRECT_SEED);
    assert_landed(qts, ATE_OFF, ATE_SEED);
    assert_landed(qts, WIDE_OFF, WIDE_SEED);

    assert_untouched(qts, target(DIRECT_WIDGET) + DIRECT_OFF);
    assert_untouched(qts, target(ATE_WIDGET) + ATE_OFF);
    assert_untouched(qts, target(WIDE_WIDGET) + WIDE_OFF);

    qtest_quit(qts);
}

/*
 * The same three transfers with a widget field in the address: each reaches
 * the widget its own route named, and only that one.
 */
static void assert_transfers_carry_their_widget(unsigned int shift)
{
    QTestState *qts = head_start(shift);

    card_start(qts);
    aim_the_routes(qts);
    run_the_routes(qts);

    assert_landed(qts, target(DIRECT_WIDGET) + DIRECT_OFF, DIRECT_SEED);
    assert_landed(qts, target(ATE_WIDGET) + ATE_OFF, ATE_SEED);
    assert_landed(qts, target(WIDE_WIDGET) + WIDE_OFF, WIDE_SEED);

    /* Nothing landed at the bare offset, and no route reached another's. */
    assert_untouched(qts, DIRECT_OFF);
    assert_untouched(qts, ATE_OFF);
    assert_untouched(qts, WIDE_OFF);
    assert_untouched(qts, target(DIRECT_WIDGET) + ATE_OFF);
    assert_untouched(qts, target(ATE_WIDGET) + WIDE_OFF);
    assert_untouched(qts, target(WIDE_WIDGET) + DIRECT_OFF);

    qtest_quit(qts);
}

static void test_transfers_carry_their_target_widget(void)
{
    assert_transfers_carry_their_widget(WIDGET_SHIFT);
}

/* And at the shift the machine's own address map uses. */
static void test_transfers_carry_a_widget_at_the_machine_shift(void)
{
    assert_transfers_carry_their_widget(MACHINE_SHIFT);
}

/*
 * A transfer whose own address reaches the widget field is one this
 * flattening cannot carry, so it is refused rather than delivered to the
 * widget the overlap names -- widget 7 here, which keeps what it had.
 */
static void test_a_transfer_over_the_widget_field_is_refused(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);
    uint64_t spilled = target(SPILL_WIDGET) + DIRECT_OFF;

    card_start(qts);
    aim_the_routes(qts);
    qtest_writeq(qts, spilled, SENTINEL);

    card_transfer(qts, 0x80000000ULL | SPILL_BIT | DIRECT_OFF, DIRECT_SEED);
    g_assert_cmphex(qtest_readq(qts, spilled), ==, SENTINEL);

    /* An address that clears the field still goes through, at the same head. */
    card_transfer(qts, 0x80000000ULL | DIRECT_OFF, DIRECT_SEED);
    assert_landed(qts, target(DIRECT_WIDGET) + DIRECT_OFF, DIRECT_SEED);

    qtest_quit(qts);
}

/* A refusal nothing reports is a transfer that vanished, so it is logged. */
static bool refusal_was_logged(uint64_t pci_addr)
{
    g_autofree char *path = g_strdup_printf("%s/sn0-bridge-widget-XXXXXX.log",
                                            g_get_tmp_dir());
    g_autofree char *option = NULL;
    g_autofree char *found = NULL;
    QTestState *qts;
    bool seen;
    int fd = g_mkstemp(path);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    option = g_strdup_printf("-d guest_errors -D %s", path);

    qts = head_start_with(WIDGET_SHIFT, option);
    card_start(qts);
    aim_the_routes(qts);
    card_transfer(qts, pci_addr, DIRECT_SEED);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &found, NULL, NULL));
    seen = strstr(found, REFUSED_FRAGMENT) != NULL;
    g_assert_cmpint(g_unlink(path), ==, 0);
    return seen;
}

static void test_a_refused_transfer_is_reported(void)
{
    g_assert_true(refusal_was_logged(0x80000000ULL | SPILL_BIT | DIRECT_OFF));
    /* The same head and the same route, one address bit lower: nothing said. */
    g_assert_false(refusal_was_logged(0x80000000ULL | DIRECT_OFF));
}

/*
 * One pin's own host address: the bit it is to set, under a field that
 * selects a node on a head with no widget field and replaces the
 * destination's top ten address bits on one that has.
 */
static void route_the_pin(QTestState *qts, unsigned int address)
{
    qtest_writel(qts, REG(BRIDGE_INT_HOST(PIN)), (address << 8) | PIN_NUMBER);
}

/* The widget-level destination, upper address bits and all. */
static void aim_the_destination(QTestState *qts, unsigned int widget,
                                unsigned int upper, uint32_t lower)
{
    qtest_writel(qts, REG(BRIDGE_INT_DEST_UPPER), (widget << 16) | upper);
    qtest_writel(qts, REG(BRIDGE_INT_DEST_LOWER), lower);
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
    QTestState *qts = head_start(0);

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
    QTestState *qts = head_start(0);

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
static void provoke_the_error(QTestState *qts, unsigned int widget,
                              unsigned int upper)
{
    aim_the_destination(qts, widget, upper, ERR_OFF);
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
    QTestState *qts = head_start(0);

    provoke_the_error(qts, 0, 0);
    g_assert_cmphex(qtest_readq(qts, ERR_OFF), ==, INT_SET | ERR_NUMBER);

    /* Not the pin register's node, and not the pin register's number. */
    g_assert_cmphex(qtest_readq(qts, NODE1_BASE + HUB_INT_MODIFY), ==, 0);

    qtest_quit(qts);
}

/*
 * With a widget field in the address, a pin's packet goes to the widget-level
 * destination under the target widget that pair names, carrying the pin's own
 * bit number with data bit 8 set.
 */
static void test_a_pin_packet_carries_its_target_widget(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);

    aim_the_destination(qts, WIDE_WIDGET, DEST_UPPER, PIN_OFF);
    route_the_pin(qts, 0);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 1u << PIN);

    /* Nothing has been sent yet. */
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==, 0);

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==,
                    INT_SET | PIN_NUMBER);

    /* The status register follows the pin whichever way the packet went. */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) & (1u << PIN),
                    ==, 1u << PIN);

    /* And the Hub window a head with no widget field addresses is not it. */
    g_assert_cmphex(qtest_readq(qts, HUB_WINDOW + HUB_INT_MODIFY), ==, 0);

    qtest_quit(qts);
}

/* The pin's host-address bits replace the destination's, they do not join. */
static void test_a_pin_host_address_replaces_the_destination_bits(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);

    aim_the_destination(qts, WIDE_WIDGET, DEST_UPPER, PIN_OFF);
    route_the_pin(qts, 1);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 1u << PIN);

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, replaced_base() + PIN_OFF), ==,
                    INT_SET | PIN_NUMBER);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==, 0);

    qtest_quit(qts);
}

/*
 * A destination whose own address reaches the widget field is refused the
 * same way a transfer is: widget 3 with that bit set names widget 7, which
 * keeps what it had.
 */
static void test_a_destination_over_the_widget_field_is_refused(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);
    uint64_t spilled = target(SPILL_WIDGET) + PIN_OFF;

    aim_the_destination(qts, DIRECT_WIDGET, 0, SPILL_BIT | PIN_OFF);
    route_the_pin(qts, 0);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 1u << PIN);
    qtest_writeq(qts, spilled, SENTINEL);

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, spilled), ==, SENTINEL);

    qtest_quit(qts);
}

/* A pin whose enable bit is clear sends nothing, however it is routed. */
static void test_a_masked_pin_sends_no_packet(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);

    aim_the_destination(qts, WIDE_WIDGET, DEST_UPPER, PIN_OFF);
    route_the_pin(qts, 0);
    /* The enable register is deliberately left at its reset value of zero. */
    qtest_writeq(qts, dest_base() + PIN_OFF, SENTINEL);

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==, SENTINEL);

    /* The pin is up all the same; only delivery was masked. */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) & (1u << PIN),
                    ==, 1u << PIN);

    qtest_quit(qts);
}

/*
 * With the mode bit set the falling edge sends a clearing packet to the same
 * address, told apart from the raise by data bit 8 alone.
 */
static void test_a_falling_pin_clears_through_the_same_address(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);

    aim_the_destination(qts, WIDE_WIDGET, DEST_UPPER, PIN_OFF);
    route_the_pin(qts, 0);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 1u << PIN);
    qtest_writel(qts, REG(BRIDGE_INT_MODE), 1u << PIN);

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==,
                    INT_SET | PIN_NUMBER);

    drive_the_pin(qts, 0);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==, PIN_NUMBER);

    qtest_quit(qts);
}

/*
 * Without that bit the guest clears the host itself and the chip sends none.
 * The destination is overwritten between the two edges: the raise left the
 * exact word a second raise would write, and a cell still holding it cannot
 * tell nothing-sent from sent-again.
 */
static void test_a_falling_pin_sends_nothing_without_the_mode_bit(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);

    aim_the_destination(qts, WIDE_WIDGET, DEST_UPPER, PIN_OFF);
    route_the_pin(qts, 0);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), 1u << PIN);
    /* The mode register is deliberately left at its reset value of zero. */

    drive_the_pin(qts, 1);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==,
                    INT_SET | PIN_NUMBER);

    qtest_writeq(qts, dest_base() + PIN_OFF, SENTINEL);
    drive_the_pin(qts, 0);
    g_assert_cmphex(qtest_readq(qts, dest_base() + PIN_OFF), ==, SENTINEL);

    qtest_quit(qts);
}

/* The chip's own error keeps taking the widget-level pair, not a pin's. */
static void test_a_chip_error_carries_the_widget_destination(void)
{
    QTestState *qts = head_start(WIDGET_SHIFT);

    qtest_writeq(qts, replaced_base() + ERR_OFF, SENTINEL);
    provoke_the_error(qts, WIDE_WIDGET, DEST_CARRIED);
    g_assert_cmphex(qtest_readq(qts, dest_base() + ERR_OFF), ==,
                    INT_SET | ERR_NUMBER);

    /* The address the programmed pin register would have made is untouched. */
    g_assert_cmphex(qtest_readq(qts, replaced_base() + ERR_OFF), ==, SENTINEL);

    qtest_quit(qts);
}

/*
 * A widget number is four bits wide, so 60 is the furthest one can be shifted
 * and still be inside a 64-bit address. Past that the machine has to be
 * refused where it is built, rather than have the number fall off the top.
 * The refusal lands before there is a qtest socket to ask, and a sysbus
 * device cannot be added to a machine already running, so this one runs the
 * emulator itself and reads what it said on the way out.
 */
static void test_a_shift_past_the_address_is_refused(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "none,accel=qtest", "-display", "none",
        "-device", "sgi-bridge,board-part=" BOARD_PART
        ",fabric=/machine/unattached/system[0],fabric-widget-shift=61", NULL
    };
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                               NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(error_output, "fabric widget shift 61"));
}

/* And the shift itself builds a head that comes up and answers. */
static void test_the_largest_shift_is_accepted(void)
{
    QTestState *qts = head_start(60);

    g_assert_cmphex((qtest_readl(qts, REG(BRIDGE_IDENTIFICATION)) >>
                     ID_PART_SHIFT) & 0xffff, ==, ID_PART);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/bridge-widget/transfer-offsets",
                   test_transfers_land_at_their_offset);
    qtest_add_func("/sn0/bridge-widget/transfer-widgets",
                   test_transfers_carry_their_target_widget);
    qtest_add_func("/sn0/bridge-widget/transfer-machine-shift",
                   test_transfers_carry_a_widget_at_the_machine_shift);
    qtest_add_func("/sn0/bridge-widget/transfer-overflow",
                   test_a_transfer_over_the_widget_field_is_refused);
    qtest_add_func("/sn0/bridge-widget/transfer-overflow-logged",
                   test_a_refused_transfer_is_reported);
    qtest_add_func("/sn0/bridge-widget/pin-node",
                   test_a_pin_packet_addresses_the_named_node);
    qtest_add_func("/sn0/bridge-widget/pin-node-stride",
                   test_a_pin_host_address_selects_a_node);
    qtest_add_func("/sn0/bridge-widget/error-destination",
                   test_a_chip_error_uses_the_widget_destination);
    qtest_add_func("/sn0/bridge-widget/pin-widget",
                   test_a_pin_packet_carries_its_target_widget);
    qtest_add_func("/sn0/bridge-widget/pin-host-replaces",
                   test_a_pin_host_address_replaces_the_destination_bits);
    qtest_add_func("/sn0/bridge-widget/destination-overflow",
                   test_a_destination_over_the_widget_field_is_refused);
    qtest_add_func("/sn0/bridge-widget/pin-masked",
                   test_a_masked_pin_sends_no_packet);
    qtest_add_func("/sn0/bridge-widget/pin-falling",
                   test_a_falling_pin_clears_through_the_same_address);
    qtest_add_func("/sn0/bridge-widget/pin-falling-unarmed",
                   test_a_falling_pin_sends_nothing_without_the_mode_bit);
    qtest_add_func("/sn0/bridge-widget/error-widget",
                   test_a_chip_error_carries_the_widget_destination);
    qtest_add_func("/sn0/bridge-widget/shift-limit",
                   test_a_shift_past_the_address_is_refused);
    qtest_add_func("/sn0/bridge-widget/shift-largest",
                   test_the_largest_shift_is_accepted);
    return g_test_run();
}
