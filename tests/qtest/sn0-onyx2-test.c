/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Onyx2 machine's two topologies, as the Onyx2 datasheet in the Origami
 * repository describes them (docs/datasheets/onyx2.md): the rack is the
 * Origin 2000 eight-processor module with a BaseIO-G in IO1 (Crossbow 0
 * widget 8) and the pipe's KONA widget behind an Xtown in IO3 (widget F);
 * the deskside has one Crossbow whose table puts the BaseIO-G at widget F
 * and the pipe directly at widget 8. Neither fits an SI graphics board.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-test.h"

#define RACK_ARGS     "-M onyx2 -display none"
#define DESKSIDE_ARGS \
    "-M onyx2,topology=onyx2-deskside -smp 4 -m 128M -display none"

#define WIDGET_SIZE   UINT64_C(0x1000000)
#define WIDGET_ID     4

/* revision:part:manufacturer:1, the identification register's layout. */
#define WIDGET_PART(id) (((id) >> 12) & 0xffff)
#define WIDGET_MFG(id)  (((id) >> 1) & 0x7ff)

#define KONA_PART     0xc102
#define KONA_MFG      0x2aa
#define BRIDGE_PART   0xc002
#define BRIDGE_MFG    0x036

static uint32_t widget_id(QTestState *qts, unsigned int widget)
{
    uint8_t bytes[4];

    /* Through CPU 0's Hub, which every topology here attaches to Crossbow 0. */
    g_assert_true(qtest_memread_cpu(qts, 0, SPACE_XTALK +
                                    widget * WIDGET_SIZE + WIDGET_ID,
                                    bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

static void assert_widget(QTestState *qts, unsigned int widget,
                          unsigned int part, unsigned int mfg)
{
    uint32_t id = widget_id(qts, widget);

    g_assert_cmphex(WIDGET_PART(id), ==, part);
    g_assert_cmphex(WIDGET_MFG(id), ==, mfg);
}

static uint64_t qom_uint(QTestState *qts, const char *path, const char *prop)
{
    g_autoptr(QDict) r = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments':"
                                   " {'path': %s, 'property': %s}}",
                                   path, prop);

    g_assert_false(qdict_haskey(r, "error"));
    return qdict_get_int(r, "return");
}

static bool qom_has_child(QTestState *qts, const char *parent,
                          const char *child)
{
    g_autoptr(QDict) r = qtest_qmp(qts, "{'execute': 'qom-list',"
                                   " 'arguments': {'path': %s}}", parent);
    const QListEntry *entry;

    for (entry = qlist_first(qdict_get_qlist(r, "return")); entry;
         entry = qlist_next(entry)) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(item, "name"), child)) {
            return true;
        }
    }
    return false;
}

static void test_rack_places_kona_at_io3(void)
{
    QTestState *qts = qtest_init(RACK_ARGS);

    assert_widget(qts, 15, KONA_PART, KONA_MFG);
    assert_widget(qts, 8, BRIDGE_PART, BRIDGE_MFG);
    g_assert_cmphex(qom_uint(qts, "/machine/crossbow0",
                             "startup-links-alive") &
                    0xff00, ==, 0x8700);
    g_assert_false(qom_has_child(qts, "/machine", "mgras"));
    qtest_quit(qts);
}

static void test_deskside_places_kona_at_io4(void)
{
    QTestState *qts = qtest_init(DESKSIDE_ARGS);

    assert_widget(qts, 8, KONA_PART, KONA_MFG);
    assert_widget(qts, 15, BRIDGE_PART, BRIDGE_MFG);
    /* Widgets 9 and A are the two Hubs; nothing else is fitted. */
    g_assert_cmphex(qom_uint(qts, "/machine/crossbow0",
                             "startup-links-alive") &
                    0xff00, ==, 0x8700);
    g_assert_false(qom_has_child(qts, "/machine", "crossbow1"));
    g_assert_false(qom_has_child(qts, "/machine", "mgras"));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/onyx2/rack-places-kona-at-io3",
                   test_rack_places_kona_at_io3);
    qtest_add_func("sn0/onyx2/deskside-places-kona-at-io4",
                   test_deskside_places_kona_at_io4);
    return g_test_run();
}
