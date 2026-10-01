/* SPDX-License-Identifier: BSD-3-Clause */
/* Test Origin 2000 PCI Carrier Assembly placement and access. */
/*
 * Register-contract tests for the Origin 2000 PCI Carrier Assembly fitted in
 * IO2, crossbow0 widget 12: sgi-pci-carrier, an sgi-pci-expansion with
 * three PCI slots and the carrier's own board record.
 *
 * The owner's guide gives the carrier three slots; which PCI device number
 * each slot is wired to is not captured, so the catalogue selects devices 0,
 * 1 and 2. These tests hold the model to that: a card at each of the three
 * answers through the carrier's widget, and a card at device 3 is refused.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-launch.h"
#include "sgi-test.h"

#define CARRIER_WIDGET      12
#define WIDGET_BASE         (SPACE_IO + ((uint64_t)CARRIER_WIDGET << 24))
#define CFG0(dev)           (WIDGET_BASE + 0x020000 + (dev) * 0x1000)

/* Any PCI card will do; the IOC3 is the one this machine already builds. */
#define CARD_ID_PAIR        0x000310a9u
#define CARRIER_BUS         "io2.0"
#define CARRIER_IOC3_DEVICE 1
#define CARRIER_IOC3_MMIO   0x10000000u
#define CARRIER_IOC3_WINDOW 0x400000
#define KBC_KBD_RECEIVE     0x0a0
#define KBC_RX_ONE(b)       ((1u << 31) | ((uint32_t)(b) << 16))
#define SCAN_A              0x1c
#define CARRIER_KBC         "/machine/peripheral/carrier-ioc3/kbc"

#define CARRIER_MACHINE \
    "-M origin2000 -display none -device sgi-pci-carrier,slot=io2"

static char *card_args(unsigned int device)
{
    return g_strdup_printf(CARRIER_MACHINE " -device sgi-ioc3,bus="
                           CARRIER_BUS ",addr=%u", device);
}

static uint32_t carrier_read(QTestState *qts, uint64_t offset)
{
    uint8_t bytes[sizeof(uint32_t)];

    g_assert_true(qtest_memread_cpu(qts, 0, WIDGET_BASE + offset,
                                    bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

static void carrier_write(QTestState *qts, uint64_t address, uint32_t value)
{
    uint8_t bytes[sizeof(uint32_t)];

    stl_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 0, address, bytes, sizeof(bytes)));
}

static void carrier_ioc3_attach(QTestState *qts)
{
    carrier_write(qts, CFG0(CARRIER_IOC3_DEVICE) + PCI_BAR0,
                  CARRIER_IOC3_MMIO);
    carrier_write(qts, CFG0(CARRIER_IOC3_DEVICE) + PCI_COMMAND,
                  PCI_COMMAND_MEMORY);
    carrier_write(qts, WIDGET_BASE + 0x200 + 8 * CARRIER_IOC3_DEVICE + 4,
                  DEV_MEMORY_SPACE | (CARRIER_IOC3_MMIO >> 20));
}

static void test_input_reaches_carrier_ioc3_kbc(void)
{
    const char *const machine = CARRIER_MACHINE " -device sgi-ioc3,"
        "id=carrier-ioc3,bus=" CARRIER_BUS ",addr=1,subsystem-id=0xffff";
    QTestState *qts = qtest_init(machine);
    QDict *response;

    carrier_ioc3_attach(qts);
    g_assert_cmphex(carrier_read(qts, CARRIER_IOC3_WINDOW + KBC_KBD_RECEIVE),
                    ==, 0);

    response = qtest_qmp(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s,"
        "'property': 'activate-keyboard', 'value': true}}", CARRIER_KBC);
    g_assert_false(qdict_haskey(response, "error"));
    qobject_unref(response);

    response = qtest_qmp(qts,
        "{'execute': 'input-send-event', 'arguments': {'events': [{"
        "'type': 'key', 'data': {'down': true, 'key': {"
        "'type': 'qcode', 'data': 'a'}}}]}}");
    g_assert_false(qdict_haskey(response, "error"));
    qobject_unref(response);

    g_assert_cmphex(carrier_read(qts, CARRIER_IOC3_WINDOW + KBC_KBD_RECEIVE),
                    ==, KBC_RX_ONE(SCAN_A));
    qtest_quit(qts);
}

/*
 * The carrier's widget reads present and alive once the machine is up in
 * this model.
 */
static void test_widget_alive(void)
{
    QTestState *qts = qtest_init(CARRIER_MACHINE);

    g_assert_cmphex(qtest_readl(qts, SPACE_IO + 0x138 +
                                (CARRIER_WIDGET - 8) * 0x40 + 4) & (1u << 5),
                                ==, 1u << 5);
    g_assert_cmphex(qtest_readl(qts, SPACE_IO + 0x110 +
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
    qtest_add_func("sn0/pci-carrier/input-reaches-ioc3-kbc",
                   test_input_reaches_carrier_ioc3_kbc);
    qtest_add_func("sn0/pci-carrier/card-enumerates-in-each-slot",
                   test_card_enumerates_in_each_slot);
    qtest_add_func("sn0/pci-carrier/fourth-slot-refused",
                   test_fourth_slot_refused);
    return g_test_run();
}
