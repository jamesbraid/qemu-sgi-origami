/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test Origin 2000 PCI Carrier Assembly placement and access. */
/*
 * The owner's guide specifies three slots but no PCI device numbers; the
 * catalogue chooses devices 0-2 in IO2 at crossbow0 widget 12. Each answers;
 * device 3 is refused.
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
#define CARRIER_BUS         "pci.carrier1"

#define CARRIER_MACHINE \
    "-M origin2000,topology=origin2000-pci-carrier -display none"

static char *card_args(unsigned int device)
{
    return g_strdup_printf(CARRIER_MACHINE " -device sgi-ioc3,bus="
                           CARRIER_BUS ",addr=%u", device);
}

/*
 * The carrier's widget is alive from reset like the BaseIO's: part of
 * crossbow0's configured startup population (0x8700: BaseIO 8, host nodes 9
 * and 10, SI Viz 15), which is what lets the stock PROM discover it.
 */
static void test_widget_alive_at_startup(void)
{
    QTestState *qts = qtest_init(CARRIER_MACHINE);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': '/machine/crossbow0',"
        "'property': 'startup-links-alive'}}");

    g_assert_false(qdict_haskey(response, "error"));
    g_assert_cmphex(qdict_get_int(response, "return"), ==,
                    0x8700 | (1u << CARRIER_WIDGET));
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
        qtest_qemu_binary(NULL), "-M",
        "origin2000,topology=origin2000-pci-carrier",
        "-display", "none", "-accel", "qtest", "-qmp", "stdio",
        "-device", "sgi-ioc3,bus=" CARRIER_BUS ",addr=3", NULL,
    };

    assert_launch_rejected(args, "reserved");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sn0/pci-carrier/widget-alive-at-startup",
                   test_widget_alive_at_startup);
    qtest_add_func("sn0/pci-carrier/card-enumerates-in-each-slot",
                   test_card_enumerates_in_each_slot);
    qtest_add_func("sn0/pci-carrier/fourth-slot-refused",
                   test_fourth_slot_refused);
    return g_test_run();
}
