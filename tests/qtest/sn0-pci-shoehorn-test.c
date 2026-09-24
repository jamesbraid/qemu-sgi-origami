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
 * keystroke to be delivered, and the shoehorn wires it (to the card's INTB,
 * which the Bridge routes to pin slot ^ 4). A Cad Duo behind a plain
 * sgi-pci-expansion, which does no such wiring, leaves that interrupt dead
 * -- the RED baseline the shoehorn's wiring turns GREEN.
 *
 * The same checks then run on the board the origin2000-pci-shoehorn
 * topology places itself: the compiled catalogue fits the shoehorn into
 * Origin 2000's IO2 position, crossbow0 widget 12, as a startup-attached
 * board, with its Cad Duo at PCI device 0. That widget must be alive from
 * reset like the BaseIO's, which is what the stock PROM needs to discover
 * it rather than report the link bad.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-test.h"

/*
 * crossbow0's spare fabric widget for a `-device` XIO board on Origin 2000
 * (sn-builder-test.c: startup-links-alive 0x8700 leaves widget 11 first
 * free). Its own small window reaches the board's Bridge and the Cad Duo.
 */
#define SHOEHORN_WIDGET     11

/*
 * The catalogue's own placement: Origin 2000 IO2, crossbow0 widget 12
 * (docs/machines/layers/modules/origin2000-module.yaml, the pci-expansion
 * connector), with the Cad Duo at PCI device 1, where real Origin 2000
 * hardware graphs show a card behind this adapter
 * (.../slot/io5/xtalk_pci/pci/1/pckb in nekonomicon thread 5749).
 */
#define PLACED_WIDGET       12
#define PLACED_CAD_DUO      1

/*
 * One fixture: which widget the board answers at and which PCI device its
 * Cad Duo occupies.
 */
typedef struct {
    const char *args;
    unsigned int widget;
    unsigned int device;
} ShoehornCase;

#define WIDGET_BASE(w)      (SPACE_XTALK + ((uint64_t)(w) << 24))

/* The Bridge's own chip registers sit at a slot's upper half (+REG_DATA). */
#define REG_DATA            4
#define CFG0(c)             (WIDGET_BASE((c)->widget) + 0x020000 + \
                             (c)->device * 0x1000)
#define BRIDGE_DEVICE(c)    (WIDGET_BASE((c)->widget) + 0x200 + \
                             8 * (c)->device + REG_DATA)

/* The Cad Duo's PCI slot in the adapter and its own 1 MB MMIO window. */
#define CAD_DUO_DEVICE      2
/* Bridge device windows: 2 MB for devices 0 and 1, then 1 MB each. */
#define DEVIO_OFFSET(d)     ((d) < 2 ? 0x200000 + (d) * 0x200000 : \
                             0x600000 + ((d) - 2) * 0x100000)
#define CAD_DUO_WINDOW(c)   (WIDGET_BASE((c)->widget) + \
                             DEVIO_OFFSET((c)->device))
#define CAD_DUO_PCI_MMIO    0x1c000000u

/* The Cad Duo (sgi-ioc3) PCI identity: SGI 0x10a9, IOC3 0x0003. */
#define PCI_SUBSYSTEM       0x2c
#define CAD_DUO_ID_PAIR     0x000310a9u
#define CAD_DUO_SUBSYSTEM   0xffff10a9u   /* subsystem id 0xffff (uncaptured) */

/* IOC3 registers in the chip's own MMIO window (no +REG_DATA skew). */
#define CD_INT_STATUS(c)     (CAD_DUO_WINDOW(c) + 0x1c)
#define CD_INT_ENABLE_SET(c) (CAD_DUO_WINDOW(c) + 0x20)
#define CD_KBD_TRANSMIT(c)   (CAD_DUO_WINDOW(c) + 0x0a8)
#define INT_KEYBOARD        (1u << 22)   /* SuperIO keyboard/mouse status bit */

/* The adapter Bridge's own interrupt-forwarding registers. */
#define BRIDGE_INT_ENABLE(c)     (WIDGET_BASE((c)->widget) + 0x108 + REG_DATA)
#define BRIDGE_INT_DEST_UPPER(c) (WIDGET_BASE((c)->widget) + 0x030 + REG_DATA)
#define BRIDGE_INT_DEST_LOWER(c) (WIDGET_BASE((c)->widget) + 0x038 + REG_DATA)
#define BRIDGE_INT_STATUS(c)     (WIDGET_BASE((c)->widget) + 0x100 + REG_DATA)
#define BRIDGE_INT_HOST(c, pin)  (WIDGET_BASE((c)->widget) + 0x130 + \
                                  8 * (pin) + REG_DATA)

/*
 * A plug-in card's second interrupt leaves on its INTB, which the Bridge
 * routes to pin (slot ^ 4); INTA goes to pin slot. Public Linux SN Bridge
 * driver (Linux 2.5.0 arch/ia64/sn/io/pcibr.c, the INTB/INTD case); an
 * add-in IOC3 raises SuperIO on INTB (OpenBSD sys/arch/sgi/pci/ioc.c).
 */
#define SUPERIO_PIN(c)           ((c)->device ^ 4)

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

#define PLACED_ARGS \
    "-M origin2000,topology=origin2000-pci-shoehorn -display none"

static const ShoehornCase expansion_case = {
    EXPANSION_ARGS, SHOEHORN_WIDGET, CAD_DUO_DEVICE,
};
static const ShoehornCase device_case = {
    SHOEHORN_ARGS, SHOEHORN_WIDGET, CAD_DUO_DEVICE,
};
static const ShoehornCase placed_case = {
    PLACED_ARGS, PLACED_WIDGET, PLACED_CAD_DUO,
};

/*
 * Program the Cad Duo's BAR0, enable memory, and aim the Bridge window at it.
 */
static void cad_duo_map(QTestState *qts, const ShoehornCase *c)
{
    qtest_writel(qts, CFG0(c) + PCI_BAR0, CAD_DUO_PCI_MMIO);
    qtest_writel(qts, CFG0(c) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(c),
                 DEV_MEMORY_SPACE | (CAD_DUO_PCI_MMIO >> 20));
}

/*
 * Arm the adapter Bridge so the card's SuperIO pin sends an interrupt
 * packet to node 0's Hub latching CAD_DUO_INT_NUMBER, and enable the Cad
 * Duo's keyboard interrupt so a PS/2 reply raises its SuperIO output.
 */
static void arm_interrupt(QTestState *qts, const ShoehornCase *c)
{
    qtest_writel(qts, CD_INT_ENABLE_SET(c), INT_KEYBOARD);
    qtest_writel(qts, BRIDGE_INT_DEST_UPPER(c), HUB_WIDGET_ON_XBOW << 16);
    qtest_writel(qts, BRIDGE_INT_DEST_LOWER(c), HUB_INT_MODIFY_OFFSET);
    qtest_writel(qts, BRIDGE_INT_HOST(c, SUPERIO_PIN(c)),
                 (INT_HOST_PACKET_IO_NASID0 << 8) | CAD_DUO_INT_NUMBER);
    qtest_writel(qts, BRIDGE_INT_ENABLE(c), 1u << SUPERIO_PIN(c));
}

/* Send a keyboard byte and report whether node 0's Hub latched it. */
static bool keystroke_reaches_hub(QTestState *qts, const ShoehornCase *c)
{
    qtest_writel(qts, CD_KBD_TRANSMIT(c), 0xff);
    g_assert_cmphex(qtest_readl(qts, CD_INT_STATUS(c)) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);
    return qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0) &
           (1ULL << CAD_DUO_INT_NUMBER);
}

/*
 * The Cad Duo's PCI identity answers through the widget window: the board
 * attaches, its widget is an inbound destination, and a bus scan reads the
 * chip's own vendor/device and subsystem dwords.
 */
static void test_cad_duo_enumerates(const void *data)
{
    const ShoehornCase *c = data;
    QTestState *qts = qtest_init(c->args);

    g_assert_cmphex(qtest_readl(qts, CFG0(c)), ==, CAD_DUO_ID_PAIR);
    g_assert_cmphex(qtest_readl(qts, CFG0(c) + PCI_SUBSYSTEM), ==,
                    CAD_DUO_SUBSYSTEM);
    qtest_quit(qts);
}

/*
 * The RED baseline: a Cad Duo plugged into a plain sgi-pci-expansion, whose
 * board does not wire the chip's SuperIO interrupt to any Bridge pin. The
 * keyboard reply raises the chip's own SuperIO output, but with nothing
 * carrying it to a Bridge pin the packet never leaves and the Hub never
 * latches. (The card's ethernet INTA is idle -- no network traffic -- so its
 * pin stays low.)
 */
static void test_cad_duo_ps2_interrupt_dead_bare(const void *data)
{
    const ShoehornCase *c = data;
    QTestState *qts = qtest_init(c->args);

    cad_duo_map(qts, c);
    arm_interrupt(qts, c);
    g_assert_false(keystroke_reaches_hub(qts, c));
    qtest_quit(qts);
}

/*
 * The GREEN result: the same Cad Duo fitted in the shoehorn, which wires the
 * chip's SuperIO output to the card's INTB, Bridge pin (slot ^ 4). A
 * keyboard reply now raises that pin, the Bridge forwards an interrupt
 * packet up through crossbow0 to node 0's Hub, and PI_INT_PEND0 latches
 * the number.
 */
static void test_cad_duo_ps2_interrupt_reaches_hub(const void *data)
{
    const ShoehornCase *c = data;
    QTestState *qts = qtest_init(c->args);

    cad_duo_map(qts, c);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0) &
                    (1ULL << CAD_DUO_INT_NUMBER), ==, 0);
    arm_interrupt(qts, c);
    g_assert_true(keystroke_reaches_hub(qts, c));
    /*
     * The keystroke must not also hold the card's INTA pin, which a guest
     * services with the Ethernet handler alone: IRIX 6.5.30 looped in that
     * handler forever when SuperIO shared the pin.
     */
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS(c)) &
                    (1u << c->device), ==, 0);
    qtest_quit(qts);
}

/*
 * The catalogue-placed board's widget is alive from reset, like the BaseIO
 * at widget 8, the two host nodes at 9 and 10 and SI Viz at 15: it is part
 * of crossbow0's configured startup population, not a link brought up after
 * machine init the way a `-device` board's spare widget is. That is the
 * difference the stock PROM sees between a board it enumerates and "IO link
 * bad".
 */
static void test_placed_widget_is_alive_at_startup(const void *data)
{
    QTestState *qts = qtest_init(PLACED_ARGS);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': '/machine/crossbow0',"
        "'property': 'startup-links-alive'}}");

    g_assert_false(qdict_haskey(response, "error"));
    g_assert_cmphex(qdict_get_int(response, "return"), ==,
                    0x8700 | (1u << PLACED_WIDGET));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/pci-shoehorn/cad-duo-enumerates", &device_case,
                        test_cad_duo_enumerates);
    qtest_add_data_func("sn0/pci-shoehorn/cad-duo-ps2-interrupt-dead-bare",
                        &expansion_case,
                        test_cad_duo_ps2_interrupt_dead_bare);
    qtest_add_data_func("sn0/pci-shoehorn/cad-duo-ps2-interrupt-reaches-hub",
                        &device_case,
                        test_cad_duo_ps2_interrupt_reaches_hub);
    qtest_add_data_func("sn0/pci-shoehorn/placed/widget-alive-at-startup",
                        NULL, test_placed_widget_is_alive_at_startup);
    qtest_add_data_func("sn0/pci-shoehorn/placed/cad-duo-enumerates",
                        &placed_case, test_cad_duo_enumerates);
    qtest_add_data_func("sn0/pci-shoehorn/placed/cad-duo-interrupt-reaches-hub",
                        &placed_case, test_cad_duo_ps2_interrupt_reaches_hub);
    return g_test_run();
}
