/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register-contract tests for the SGI MENET XIO board: a Bridge and four
 * IOC3s giving a module four Fast Ethernet ports and six serial ports
 * (docs/datasheets/sgi-ioc3-boards.md in the Origami tree). The board is
 * the "menet" preset of sgi-xio-baseio, built from the existing Bridge and
 * IOC3 models.
 *
 * Two placements run the same checks. A `-device sgi-xio-baseio,
 * preset=menet` board attaches through crossbow0's spare fabric widget 11,
 * as the PCI shoehorn's own test does. The origin2000-menet topology fits
 * the board in XIO slot IO5, crossbow0 widget 14, as a startup-attached
 * board.
 *
 * The contract, from real IRIX inventories and IRIX 6.5.30's own MENET
 * setup routine:
 *   - IOC3s (0x10a9:0x0003) at PCI devices 0-3;
 *   - devices 0-2 carry Ethernet and both serial ports, device 3
 *     Ethernet alone; none carries a clock;
 *   - each chip's SuperIO interrupt reaches Bridge pin d + 4 (device 3
 *     has none), away from its Ethernet pin d;
 *   - subsystem ids 0xc305 (devices 0-2) and 0xc306 (device 3), the values
 *     Linux substitutes for this board;
 *   - each chip's one-wire bus carries only its station-address part,
 *     each with its own serial.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-test.h"

/* crossbow0's spare fabric widget for a `-device` XIO board. */
#define DEVICE_WIDGET       11
/* Origin 2000 IO5 (origin2000-module.yaml): crossbow0 widget 14. */
#define PLACED_WIDGET       14

#define MENET_IOC3S         4

typedef struct {
    const char *args;
    unsigned int widget;
    const char *board_path;
} MenetCase;

#define WIDGET_BASE(c)      (SPACE_XTALK + ((uint64_t)(c)->widget << 24))
#define REG_DATA            4
#define CFG(c, d)           (WIDGET_BASE(c) + 0x020000 + (d) * 0x1000)
#define BRIDGE_DEVICE(c, d) (WIDGET_BASE(c) + 0x200 + 8 * (d) + REG_DATA)
#define DEVIO_OFFSET(d)     ((d) < 2 ? 0x200000 + (d) * 0x200000 : \
                             0x600000 + ((d) - 2) * 0x100000)
#define IOC3_WINDOW(c, d)   (WIDGET_BASE(c) + DEVIO_OFFSET(d))
#define IOC3_PCI_MMIO(d)    (0x1c000000u + (d) * 0x100000u)

#define PCI_SUBSYSTEM       0x2c
#define IOC3_ID_PAIR        0x000310a9u
#define MENET_SUBSYSTEM     0xc30510a9u
#define MENET4_SUBSYSTEM    0xc30610a9u

/* The SuperIO block and serial port A's 16550, in the chip's window. */
#define INT_STATUS(c, d)     (IOC3_WINDOW(c, d) + 0x1c)
#define INT_ENABLE_SET(c, d) (IOC3_WINDOW(c, d) + 0x20)
#define SERIAL_A_IER(c, d)   (IOC3_WINDOW(c, d) + 0x020178 + 1)
#define IER_THRI             0x02
#define SRC_A_PASSTHROUGH    (1u << 6)

/* The Bridge's interrupt-forwarding registers. */
#define BRIDGE_INT_STATUS(c)     (WIDGET_BASE(c) + 0x100 + REG_DATA)
#define BRIDGE_INT_ENABLE(c)     (WIDGET_BASE(c) + 0x108 + REG_DATA)
#define BRIDGE_INT_DEST_UPPER(c) (WIDGET_BASE(c) + 0x030 + REG_DATA)
#define BRIDGE_INT_DEST_LOWER(c) (WIDGET_BASE(c) + 0x038 + REG_DATA)
#define BRIDGE_INT_HOST(c, pin)  (WIDGET_BASE(c) + 0x130 + 8 * (pin) + \
                                  REG_DATA)

/* Node 0's Hub on crossbow0, and its interrupt-modify offset. */
#define HUB_WIDGET_ON_XBOW        9
#define HUB_INT_MODIFY_OFFSET     0x01800090
#define INT_HOST_PACKET_IO_NASID0 0x200
#define MENET_INT_NUMBER          46

#define DEVICE_ARGS \
    "-M origin2000 -display none -device sgi-xio-baseio,preset=menet," \
    "module=99,slot=" G_STRINGIFY(DEVICE_WIDGET) \
    ",fabric=/machine/crossbow0-spare-fabric[0],fabric-widget-shift=48"

#define PLACED_ARGS \
    "-M origin2000,topology=origin2000-menet -display none"

static const MenetCase device_case = {
    DEVICE_ARGS, DEVICE_WIDGET, "/machine/module99/slot11/baseio",
};
static const MenetCase placed_case = {
    PLACED_ARGS, PLACED_WIDGET, "/machine/module0/slot14/baseio",
};

static void ioc3_map(QTestState *qts, const MenetCase *c, unsigned int d)
{
    qtest_writel(qts, CFG(c, d) + PCI_BAR0, IOC3_PCI_MMIO(d));
    qtest_writel(qts, CFG(c, d) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(c, d),
                 DEV_MEMORY_SPACE | (IOC3_PCI_MMIO(d) >> 20));
}

/* True when the QOM object at `path` has a child property named `child`. */
static bool qom_has_child(QTestState *qts, const char *path,
                          const char *child)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'qom-list', 'arguments': {'path': %s}}", path);
    QList *list;
    QListEntry *entry;

    g_assert_false(qdict_haskey(response, "error"));
    list = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(list, entry) {
        QDict *prop = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(prop, "name"), child)) {
            return true;
        }
    }
    return false;
}

/* Four IOC3s answer at devices 0-3, with the MENET and MENET4 identities. */
static void test_ioc3s_enumerate(const void *data)
{
    const MenetCase *c = data;
    QTestState *qts = qtest_init(c->args);
    unsigned int d;

    for (d = 0; d < MENET_IOC3S; d++) {
        g_assert_cmphex(qtest_readl(qts, CFG(c, d)), ==, IOC3_ID_PAIR);
        g_assert_cmphex(qtest_readl(qts, CFG(c, d) + PCI_SUBSYSTEM), ==,
                        d < 3 ? MENET_SUBSYSTEM : MENET4_SUBSYSTEM);
    }
    qtest_quit(qts);
}

/*
 * Devices 0-2 carry Ethernet and both serial ports; device 3 Ethernet only.
 * No chip carries a clock, a keyboard controller or a parallel port.
 */
static void test_populations(const void *data)
{
    const MenetCase *c = data;
    QTestState *qts = qtest_init(c->args);
    unsigned int d;

    for (d = 0; d < MENET_IOC3S; d++) {
        g_autofree char *path = g_strdup_printf("%s/ioc3-%u", c->board_path,
                                                d);
        bool superio = d < 3;

        g_assert_true(qom_has_child(qts, path, "eth"));
        g_assert_true(qom_has_child(qts, path, "onewire-part"));
        g_assert_cmpint(qom_has_child(qts, path, "serial[0]"), ==, superio);
        g_assert_cmpint(qom_has_child(qts, path, "serial[1]"), ==, superio);
        g_assert_false(qom_has_child(qts, path, "rtc"));
        g_assert_false(qom_has_child(qts, path, "ds1685"));
        g_assert_false(qom_has_child(qts, path, "kbc"));
        g_assert_false(qom_has_child(qts, path, "parallel"));
        g_assert_false(qom_has_child(qts, path, "onewire-board-part"));
    }
    qtest_quit(qts);
}

/* Each chip's one-wire station-address part carries its own serial. */
static void test_onewire_serials_distinct(const void *data)
{
    const MenetCase *c = data;
    QTestState *qts = qtest_init(c->args);
    uint64_t serial[MENET_IOC3S];
    unsigned int d, e;

    for (d = 0; d < MENET_IOC3S; d++) {
        g_autofree char *path = g_strdup_printf("%s/ioc3-%u/onewire-part",
                                                c->board_path, d);
        g_autoptr(QDict) response = qtest_qmp(qts,
            "{'execute': 'qom-get', 'arguments': {'path': %s,"
            "'property': 'serial'}}", path);

        g_assert_false(qdict_haskey(response, "error"));
        serial[d] = qdict_get_int(response, "return");
        for (e = 0; e < d; e++) {
            g_assert_cmphex(serial[e], !=, serial[d]);
        }
    }
    qtest_quit(qts);
}

/*
 * A serial interrupt on device d (0-2) raises the chip's SuperIO output,
 * which the board wires to Bridge pin d + 4; the Bridge forwards it to node
 * 0's Hub. The chip's own Ethernet pin d stays low. One machine checks all
 * three chips, each pin forwarding its own interrupt number.
 */
static void test_superio_reaches_hub(const void *data)
{
    const MenetCase *c = data;
    QTestState *qts = qtest_init(c->args);
    uint32_t enabled = 0;
    unsigned int d;

    qtest_writel(qts, BRIDGE_INT_DEST_UPPER(c), HUB_WIDGET_ON_XBOW << 16);
    qtest_writel(qts, BRIDGE_INT_DEST_LOWER(c), HUB_INT_MODIFY_OFFSET);
    for (d = 0; d < 3; d++) {
        unsigned int pin = d + 4;
        uint64_t hub_bit = 1ULL << (MENET_INT_NUMBER + d);

        ioc3_map(qts, c, d);
        qtest_writel(qts, BRIDGE_INT_HOST(c, pin),
                     (INT_HOST_PACKET_IO_NASID0 << 8) |
                     (MENET_INT_NUMBER + d));
        enabled |= 1u << pin;
        qtest_writel(qts, BRIDGE_INT_ENABLE(c), enabled);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0) & hub_bit,
                        ==, 0);

        qtest_writel(qts, INT_ENABLE_SET(c, d), SRC_A_PASSTHROUGH);
        qtest_writeb(qts, SERIAL_A_IER(c, d), IER_THRI);
        g_assert_cmphex(qtest_readl(qts, INT_STATUS(c, d)) &
                        SRC_A_PASSTHROUGH, ==, SRC_A_PASSTHROUGH);

        g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS(c)) &
                        ((1u << pin) | (1u << d)), ==, 1u << pin);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0) & hub_bit,
                        ==, hub_bit);
    }
    qtest_quit(qts);
}

/* The catalogue-placed board's widget is alive from reset. */
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

    qtest_add_data_func("sn0/menet/ioc3s-enumerate", &device_case,
                        test_ioc3s_enumerate);
    qtest_add_data_func("sn0/menet/populations", &device_case,
                        test_populations);
    qtest_add_data_func("sn0/menet/onewire-serials-distinct", &device_case,
                        test_onewire_serials_distinct);
    qtest_add_data_func("sn0/menet/superio-reaches-hub", &device_case,
                        test_superio_reaches_hub);
    qtest_add_data_func("sn0/menet/placed/widget-alive-at-startup", NULL,
                        test_placed_widget_is_alive_at_startup);
    qtest_add_data_func("sn0/menet/placed/ioc3s-enumerate", &placed_case,
                        test_ioc3s_enumerate);
    qtest_add_data_func("sn0/menet/placed/populations", &placed_case,
                        test_populations);
    qtest_add_data_func("sn0/menet/placed/superio-reaches-hub", &placed_case,
                        test_superio_reaches_hub);
    return g_test_run();
}
