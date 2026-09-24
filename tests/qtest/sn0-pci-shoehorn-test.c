/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register-contract tests for the SGI PCI-XTALK ADAPTER (the "PCI shoehorn",
 * hw/mips/sgi/board-pci-shoehorn.c) fitted with an SGI Cad Duo input card
 * (an sgi-ioc3, docs/datasheets/sgi-cad-duo-pci-card.md).
 *
 * The board attaches through crossbow0's spare fabric proxy exactly as
 * sgi-pci-expansion does (tests/qtest/sn-builder-test.c), so the spare widget
 * is 11: the lowest position crossbow0's startup-links-alive (0x8700) leaves
 * free. A processor reaches the internal Bridge and the fitted Cad Duo
 * through crossbow0's real small window for widget 11.
 *
 * Two things are checked. First, the Cad Duo enumerates: its PCI identity
 * answers through the widget window, the same config accesses a driver's own
 * probe issues. Second, its PS/2 keyboard interrupt reaches the guest: the
 * chip's SuperIO interrupt output must be wired to a Bridge pin for a
 * keystroke to be delivered, and the shoehorn wires it (to the pin the card's
 * own slot maps to). A Cad Duo behind a plain sgi-pci-expansion, which does
 * no such wiring, leaves that interrupt dead -- the RED baseline the
 * shoehorn's wiring turns GREEN.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-test.h"

/*
 * crossbow0's spare fabric widget for a `-device` XIO board on Origin 2000
 * (sn-builder-test.c: startup-links-alive 0x8700 leaves widget 11 first
 * free). Its own small window reaches the board's Bridge and the Cad Duo.
 */
#define SHOEHORN_WIDGET     11
#define BRIDGE_BASE         (SPACE_XTALK + ((uint64_t)SHOEHORN_WIDGET << 24))

/* The Bridge's own chip registers sit at a slot's upper half (+REG_DATA). */
#define REG_DATA            4
#define CFG0(dev)           (BRIDGE_BASE + 0x020000 + (dev) * 0x1000)
#define BRIDGE_DEVICE(n)    (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)

/* The Cad Duo's PCI slot in the adapter and its own 1 MB MMIO window. */
#define CAD_DUO_DEVICE      2
#define CAD_DUO_WINDOW      (BRIDGE_BASE + 0x600000)  /* device 2's window */
#define CAD_DUO_PCI_MMIO    0x1c000000u

/* The Cad Duo (sgi-ioc3) PCI identity: SGI 0x10a9, IOC3 0x0003. */
#define PCI_SUBSYSTEM       0x2c
#define CAD_DUO_ID_PAIR     0x000310a9u
#define CAD_DUO_SUBSYSTEM   0xffff10a9u   /* subsystem id 0xffff (uncaptured) */

/* IOC3 registers in the chip's own MMIO window (no +REG_DATA skew). */
#define CD_INT_STATUS       (CAD_DUO_WINDOW + 0x1c)
#define CD_INT_ENABLE_SET   (CAD_DUO_WINDOW + 0x20)
#define CD_KBD_TRANSMIT     (CAD_DUO_WINDOW + 0x0a8)
#define INT_KEYBOARD        (1u << 22)   /* SuperIO keyboard/mouse status bit */

/* The adapter Bridge's own interrupt-forwarding registers. */
#define BRIDGE_INT_ENABLE     (BRIDGE_BASE + 0x108 + REG_DATA)
#define BRIDGE_INT_DEST_UPPER (BRIDGE_BASE + 0x030 + REG_DATA)
#define BRIDGE_INT_DEST_LOWER (BRIDGE_BASE + 0x038 + REG_DATA)
#define BRIDGE_INT_HOST(n)    (BRIDGE_BASE + 0x130 + 8 * (n) + REG_DATA)

/*
 * Node 0's Hub sits at widget 9 on crossbow0; a packet-I/O host address with
 * nasid 0 aimed at the Hub's interrupt-modify offset latches an interrupt
 * number in node 0's own PI_INT_PEND0 (sn-builder-test.c's own recipe).
 */
#define HUB_WIDGET_ON_XBOW        9
#define HUB_INT_MODIFY_OFFSET     0x01800090
#define INT_HOST_PACKET_IO_NASID0 0x200
#define CAD_DUO_INT_NUMBER        45

/* A card behind the expansion Bridge is at the same slot either way. */
#define EXPANSION_ARGS \
    "-M origin2000 -display none -device sgi-pci-expansion," \
    "module=99,slot=11,fabric=/machine/crossbow0-spare-fabric[0]," \
    "fabric-widget-shift=48,pci-bus-name=xpci " \
    "-device sgi-ioc3,bus=xpci,addr=" G_STRINGIFY(CAD_DUO_DEVICE) \
    ",subsystem-id=0xffff"

#define SHOEHORN_ARGS \
    "-M origin2000 -display none -device sgi-pci-shoehorn," \
    "module=99,slot=11,fabric=/machine/crossbow0-spare-fabric[0]," \
    "fabric-widget-shift=48,cad-duo-slot=" G_STRINGIFY(CAD_DUO_DEVICE)

/*
 * Program the Cad Duo's BAR0, enable memory, and aim the Bridge window at it.
 */
static void cad_duo_map(QTestState *qts)
{
    qtest_writel(qts, CFG0(CAD_DUO_DEVICE) + PCI_BAR0, CAD_DUO_PCI_MMIO);
    qtest_writel(qts, CFG0(CAD_DUO_DEVICE) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(CAD_DUO_DEVICE),
                 DEV_MEMORY_SPACE | (CAD_DUO_PCI_MMIO >> 20));
}

/*
 * Arm the adapter Bridge so pin @pin's interrupt packet targets node 0's
 * Hub and latches CAD_DUO_INT_NUMBER, and enable the Cad Duo's keyboard
 * interrupt so a PS/2 reply raises its SuperIO output.
 */
static void arm_interrupt(QTestState *qts, unsigned int pin)
{
    qtest_writel(qts, CD_INT_ENABLE_SET, INT_KEYBOARD);
    qtest_writel(qts, BRIDGE_INT_DEST_UPPER, HUB_WIDGET_ON_XBOW << 16);
    qtest_writel(qts, BRIDGE_INT_DEST_LOWER, HUB_INT_MODIFY_OFFSET);
    qtest_writel(qts, BRIDGE_INT_HOST(pin),
                 (INT_HOST_PACKET_IO_NASID0 << 8) | CAD_DUO_INT_NUMBER);
    qtest_writel(qts, BRIDGE_INT_ENABLE, 1u << pin);
}

/*
 * The Cad Duo's PCI identity answers through the widget window: the board
 * attaches, the spare widget is an inbound destination, and a bus scan reads
 * the chip's own vendor/device and subsystem dwords.
 */
static void test_cad_duo_enumerates_through_shoehorn(const void *data)
{
    QTestState *qts = qtest_init(SHOEHORN_ARGS);

    g_assert_cmphex(qtest_readl(qts, CFG0(CAD_DUO_DEVICE)), ==,
                    CAD_DUO_ID_PAIR);
    g_assert_cmphex(qtest_readl(qts, CFG0(CAD_DUO_DEVICE) + PCI_SUBSYSTEM),
                    ==, CAD_DUO_SUBSYSTEM);
    qtest_quit(qts);
}

/*
 * The RED baseline: a Cad Duo plugged into a plain sgi-pci-expansion, whose
 * board does not wire the chip's SuperIO interrupt to any Bridge pin. The
 * keyboard reply raises the chip's own SuperIO output, but with nothing
 * carrying it to a Bridge pin the packet never leaves and the Hub never
 * latches. (The card's ethernet INTA is idle -- no network traffic -- so pin
 * 2 stays low.)
 */
static void test_cad_duo_ps2_interrupt_dead_bare(const void *data)
{
    QTestState *qts = qtest_init(EXPANSION_ARGS);

    cad_duo_map(qts);
    arm_interrupt(qts, CAD_DUO_DEVICE);

    qtest_writel(qts, CD_KBD_TRANSMIT, 0xff);
    g_assert_cmphex(qtest_readl(qts, CD_INT_STATUS) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0) &
                    (1ULL << CAD_DUO_INT_NUMBER), ==, 0);
    qtest_quit(qts);
}

/*
 * The GREEN result: the same Cad Duo fitted in the shoehorn, which wires the
 * chip's SuperIO output to the Bridge pin of the card's own slot. A keyboard
 * reply now raises that pin, the Bridge forwards an interrupt packet up
 * through crossbow0 to node 0's Hub, and PI_INT_PEND0 latches the number.
 */
static void test_cad_duo_ps2_interrupt_reaches_hub(const void *data)
{
    QTestState *qts = qtest_init(SHOEHORN_ARGS);

    cad_duo_map(qts);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0) &
                    (1ULL << CAD_DUO_INT_NUMBER), ==, 0);

    arm_interrupt(qts, CAD_DUO_DEVICE);

    qtest_writel(qts, CD_KBD_TRANSMIT, 0xff);
    g_assert_cmphex(qtest_readl(qts, CD_INT_STATUS) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0) &
                    (1ULL << CAD_DUO_INT_NUMBER), ==,
                    1ULL << CAD_DUO_INT_NUMBER);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/pci-shoehorn/cad-duo-enumerates", NULL,
                        test_cad_duo_enumerates_through_shoehorn);
    qtest_add_data_func("sn0/pci-shoehorn/cad-duo-ps2-interrupt-dead-bare",
                        NULL,
                        test_cad_duo_ps2_interrupt_dead_bare);
    qtest_add_data_func("sn0/pci-shoehorn/cad-duo-ps2-interrupt-reaches-hub",
                        NULL,
                        test_cad_duo_ps2_interrupt_reaches_hub);
    return g_test_run();
}
