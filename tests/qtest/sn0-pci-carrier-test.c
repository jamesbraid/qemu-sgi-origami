/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test Origin 2000 PCI Carrier Assembly placement and access. */
/*
 * Register-contract tests for the Origin 2000 PCI Carrier Assembly fitted in
 * IO2, crossbow0 widget 12: sgi-pci-carrier, an sgi-pci-expansion with
 * three PCI slots and the carrier's own board record
 * (docs/datasheets/sgi-pci-shoebox.md in the Origami repository).
 *
 * The owner's guide gives the carrier three slots; which PCI device number
 * each slot is wired to is not captured, so the catalogue selects devices 0,
 * 1 and 2. These tests hold the model to that: a card at each of the three
 * answers through the carrier's widget, and a card at device 3 is refused.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-launch.h"
#include "sgi-test.h"

#define CARRIER_WIDGET      12
#define WIDGET_BASE         (SPACE_XTALK + ((uint64_t)CARRIER_WIDGET << 24))
#define CFG0(dev)           (WIDGET_BASE + 0x020000 + (dev) * 0x1000)

/* Any PCI card will do; the IOC3 is the one this machine already builds. */
#define CARD_ID_PAIR        0x000310a9u
#define CARRIER_BUS         "io2.0"

#define CARRIER_MACHINE \
    "-M origin2000 -display none -device sgi-pci-carrier,slot=io2"

static char *card_args(unsigned int device)
{
    return g_strdup_printf(CARRIER_MACHINE " -device sgi-ioc3,bus="
                           CARRIER_BUS ",addr=%u", device);
}

/*
 * The carrier's widget reads present and alive once the machine is up,
 * which is what lets the stock PROM discover it (docs/datasheets/xbow.md,
 * link auxiliary status bit 5 and link status bit 31).
 */
static void test_widget_alive(void)
{
    QTestState *qts = qtest_init(CARRIER_MACHINE);

    g_assert_cmphex(qtest_readl(qts, SPACE_XTALK + 0x138 +
                                (CARRIER_WIDGET - 8) * 0x40 + 4) & (1u << 5),
                    ==, 1u << 5);
    g_assert_cmphex(qtest_readl(qts, SPACE_XTALK + 0x110 +
                                (CARRIER_WIDGET - 8) * 0x40 + 4) & (1u << 31),
                    ==, 1u << 31);
    qtest_quit(qts);
}

/* A card in each of the carrier's three slots enumerates through it. */
static void test_card_enumerates_in_each_slot(void)
{
    unsigned int device;

    for (device = 0; device < 3; device++) {
        g_autofree char *args = card_args(device);
        QTestState *qts = qtest_init(args);

        g_assert_cmphex(qtest_readl(qts, CFG0(device)), ==, CARD_ID_PAIR);
        qtest_quit(qts);
    }
}

/* The carrier has no fourth slot: a card at device 3 is refused. */
static void test_fourth_slot_refused(void)
{
    const char *const args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000",
        "-display", "none", "-accel", "qtest", "-qmp", "stdio",
        "-device", "sgi-pci-carrier,slot=io2",
        "-device", "sgi-ioc3,bus=" CARRIER_BUS ",addr=3", NULL,
    };

    assert_launch_rejected(args, "reserved");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sn0/pci-carrier/widget-alive",
                   test_widget_alive);
    qtest_add_func("sn0/pci-carrier/card-enumerates-in-each-slot",
                   test_card_enumerates_in_each_slot);
    qtest_add_func("sn0/pci-carrier/fourth-slot-refused",
                   test_fourth_slot_refused);
    return g_test_run();
}
