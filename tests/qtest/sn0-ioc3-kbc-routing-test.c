/*
 * IOC3 keyboard routing on BaseIO-G: Bridge device 2 and MediaIO device 6
 * each have a keyboard; host input without a console binding reaches only
 * the IOC3 selected by activate-keyboard. Check its receive register and
 * absence of delivery to the other chip.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-test.h"

#define BASEIO_WIDGET   8
#define BASEIO_BASE     (SPACE_XTALK + ((uint64_t)BASEIO_WIDGET << 24))
#define REG_DATA        4         /* registers sit at a slot's upper half */
#define BRIDGE_CFG0(n)  (0x020000 + (n) * 0x1000)
#define BRIDGE_DEVICE(n) (0x200 + 8 * (n) + REG_DATA)

/* The two chips, each given a BAR and its own 1 MB Bridge device window. */
#define BOARD_IOC3_DEVICE 2
#define BOARD_IOC3_MMIO   0x10000000u
#define BOARD_IOC3_WINDOW 0x600000
#define MIO_IOC3_DEVICE   6
#define MIO_IOC3_MMIO     0x11000000u
#define MIO_IOC3_WINDOW   0xa00000

#define KBC_KBD_RECEIVE   0x0a0

/* One character, in the oldest of the receive register's three slots. */
#define KBC_RX_ONE(b)     ((1u << 31) | ((uint32_t)(b) << 16))

/* The generic ps2 keyboard's scan code set 2 make code for "a". */
#define SCAN_A            0x1c

#define BOARD_KBC "/machine/module0/slot8/baseio/ioc3/kbc"
#define MIO_KBC   "/machine/module0/slot8/baseio/mio-ioc3/kbc"

/* One io6g machine serves both cases; neither leaves anything behind. */
static QTestState *qts;

static uint32_t baseio_read(QTestState *qts, uint64_t offset)
{
    uint8_t bytes[sizeof(uint32_t)];

    g_assert_true(qtest_memread_cpu(qts, 0, BASEIO_BASE + offset,
                                    bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

static void baseio_write(QTestState *qts, uint64_t offset, uint32_t value)
{
    uint8_t bytes[sizeof(uint32_t)];

    stl_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 0, BASEIO_BASE + offset,
                                     bytes, sizeof(bytes)));
}

static void attach_ioc3(QTestState *qts, unsigned int device, uint32_t mmio)
{
    baseio_write(qts, BRIDGE_CFG0(device) + PCI_BAR0, mmio);
    baseio_write(qts, BRIDGE_CFG0(device) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    baseio_write(qts, BRIDGE_DEVICE(device), DEV_MEMORY_SPACE | (mmio >> 20));
}

static void activate_keyboard(QTestState *qts, const char *path)
{
    QDict *resp = qtest_qmp(qts,
        "{ 'execute': 'qom-set', 'arguments': "
        "{ 'path': %s, 'property': 'activate-keyboard', 'value': true } }",
        path);

    g_assert_true(qdict_haskey(resp, "return"));
    qobject_unref(resp);
}

/* A key down with no device argument: whichever keyboard is first takes it. */
static void key_down_a(QTestState *qts)
{
    QDict *resp = qtest_qmp(qts,
        "{ 'execute': 'input-send-event', 'arguments': "
        "{ 'events': [ { 'type': 'key', 'data': "
        "{ 'down': true, 'key': { 'type': 'qcode', 'data': 'a' } } } "
        "] } }");

    g_assert_true(qdict_haskey(resp, "return"));
    qobject_unref(resp);
}

static void test_activate_keyboard_routes_keys(void)
{
    attach_ioc3(qts, BOARD_IOC3_DEVICE, BOARD_IOC3_MMIO);
    attach_ioc3(qts, MIO_IOC3_DEVICE, MIO_IOC3_MMIO);

    /* Both holdings start empty. */
    g_assert_cmphex(baseio_read(qts, BOARD_IOC3_WINDOW + KBC_KBD_RECEIVE),
                    ==, 0);
    g_assert_cmphex(baseio_read(qts, MIO_IOC3_WINDOW + KBC_KBD_RECEIVE),
                    ==, 0);

    activate_keyboard(qts, MIO_KBC);
    key_down_a(qts);
    g_assert_cmphex(baseio_read(qts, MIO_IOC3_WINDOW + KBC_KBD_RECEIVE),
                    ==, KBC_RX_ONE(SCAN_A));
    g_assert_cmphex(baseio_read(qts, BOARD_IOC3_WINDOW + KBC_KBD_RECEIVE),
                    ==, 0);

    activate_keyboard(qts, BOARD_KBC);
    key_down_a(qts);
    g_assert_cmphex(baseio_read(qts, BOARD_IOC3_WINDOW + KBC_KBD_RECEIVE),
                    ==, KBC_RX_ONE(SCAN_A));
    g_assert_cmphex(baseio_read(qts, MIO_IOC3_WINDOW + KBC_KBD_RECEIVE),
                    ==, 0);
}

/* The property is host routing only: nothing reads it back. */
static void test_activate_keyboard_is_write_only(void)
{
    QDict *resp = qtest_qmp(qts,
        "{ 'execute': 'qom-get', 'arguments': "
        "{ 'path': %s, 'property': 'activate-keyboard' } }", MIO_KBC);

    g_assert_true(qdict_haskey(resp, "error"));
    qobject_unref(resp);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sn0/ioc3-kbc/activate-keyboard-routes-keys",
                   test_activate_keyboard_routes_keys);
    qtest_add_func("sn0/ioc3-kbc/activate-keyboard-is-write-only",
                   test_activate_keyboard_is_write_only);

    qts = qtest_init("-M origin2000,baseio-preset=io6g -m 256M");
    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
