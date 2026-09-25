/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register-contract tests for an Origin 200 converted for a GIGAchannel
 * expansion cabinet and run in loopback (origin200-gigachannel-loopback),
 * from docs/datasheets/sgi-gigachannel.md in the Origami repository.
 *
 * The IP27 PROM finds a cabinet by reading widget 0's identity through
 * the node's Hub: a Crossbow's part number there marks the node, a
 * Bridge's does not. In loopback there is no Crossbow, and the IP29 Bridge
 * answers the plain direct path, widget 0 included, as on an unconverted
 * module.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define XTALK_BASE UINT64_C(0x20000000000)
#define XTALK_REG_DATA 4
#define XTALK_WIDGET_SIZE UINT64_C(0x1000000)
#define WIDGET_ID 0x0000
#define XTALK_ID_PART(v) (((v) >> 12) & 0xffff)
#define BRIDGE_PART 0xc002

#define BASEIO_WIDGET 8
#define BRIDGE_CFG0(w, dev) ((uint64_t)(w) * XTALK_WIDGET_SIZE + 0x020000 + \
                             (dev) * 0x1000)
#define IOC3_ID_PAIR 0x000310a9u
#define IP29_IOC3_DEVICE 2

#define LOOPBACK "-M origin200,topology=origin200-gigachannel-loopback"
#define PLAIN "-M origin200"

/* A 32-bit crosstalk read through CPU 0's own Hub. */
static uint32_t xtalk_read(QTestState *qts, uint64_t offset)
{
    uint8_t bytes[sizeof(uint32_t)];

    g_assert_true(qtest_memread_cpu(qts, 0, XTALK_BASE + offset,
                                    bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

/* A widget register, which answers in the low word of its doubleword. */
static uint32_t widget_id(QTestState *qts, unsigned int widget)
{
    return xtalk_read(qts, widget * XTALK_WIDGET_SIZE + WIDGET_ID +
                      XTALK_REG_DATA);
}

static QTestState *start(const char *machine, const char *extra)
{
    return qtest_initf("%s -display none -d unimp,guest_errors %s",
                       machine, extra);
}

/*
 * In loopback, as on the plain machine, widget 0 answers a Bridge's part,
 * so the PROM takes the plain BASEIO PROM load.
 */
static void test_widget0_is_the_bridge(void)
{
    QTestState *qts = start(PLAIN, "");

    g_assert_cmphex(XTALK_ID_PART(widget_id(qts, 0)), ==, BRIDGE_PART);
    qtest_quit(qts);

    qts = start(LOOPBACK, "");
    g_assert_cmphex(XTALK_ID_PART(widget_id(qts, 0)), ==, BRIDGE_PART);
    qtest_quit(qts);
}

/* In loopback the IP29 Bridge sits at widget 8 on the direct path. */
static void test_loopback_takes_the_plain_path(void)
{
    QTestState *qts = start(LOOPBACK, "");
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'qom-list', 'arguments': {'path': '/machine/crossbow0'}}");

    g_assert_true(qdict_haskey(response, "error"));
    g_assert_cmphex(xtalk_read(qts, BRIDGE_CFG0(BASEIO_WIDGET,
                                                IP29_IOC3_DEVICE)), ==,
                    IOC3_ID_PAIR);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sn0/gigachannel/widget0-is-the-bridge-in-loopback",
                   test_widget0_is_the_bridge);
    qtest_add_func("sn0/gigachannel/loopback-takes-the-plain-path",
                   test_loopback_takes_the_plain_path);
    return g_test_run();
}
