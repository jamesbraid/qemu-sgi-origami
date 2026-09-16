/*
 * Origin 2000 Crosstalk assembly ownership, on the real machine.
 *
 * The builder wires two Crossbows and a BaseIO board under /machine/xtalk
 * the same way the deleted two-phase driver's own prepare/verify step did;
 * this checks the wiring is there once origin2000-module has actually been
 * built, rather than through a dedicated fixture machine's private
 * "verified" flag.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

static uint64_t xtalk_uint(QTestState *qts, const char *crossbow,
                           const char *property)
{
    g_autofree char *path = g_strdup_printf("/machine/xtalk/%s", crossbow);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': %s,"
        "'property': %s}}", path, property);

    return qdict_get_int(response, "return");
}

static void assert_crossbow(QTestState *qts, const char *crossbow,
                            uint16_t links)
{
    g_autofree char *path = g_strdup_printf("/machine/xtalk/%s", crossbow);

    g_assert_cmphex(xtalk_uint(qts, crossbow, "span"), ==,
                    UINT64_C(0x400000000));
    g_assert_cmphex(xtalk_uint(qts, crossbow, "links-present"), ==, links);
    g_assert_cmphex(xtalk_uint(qts, crossbow, "startup-links-alive"), ==,
                    links);
    g_assert_false(qtest_qom_get_bool(qts, path, "stop-on-link-reset"));
    g_assert_true(qtest_qom_get_bool(qts, path,
                                     "complete-posted-error-address-writes"));
    g_assert_cmphex(xtalk_uint(qts, crossbow, "link-reset-restore-alive"),
                    ==, links);
}

/* The shipped module: Crossbow 1 keeps three links (widgets 8-10), Crossbow
 * 2 keeps two (widgets 9-10); no XIO position is populated on either. */
static void test_owner(void)
{
    QTestState *qts = qtest_initf("-M origin2000 -m 256M "
                                  "-nodefaults -display none -serial none");

    assert_crossbow(qts, "crossbow0", 0x700);
    assert_crossbow(qts, "crossbow1", 0x600);
    /* A qom-list on a path that does not exist fails; this one must not. */
    qobject_unref(qtest_qmp_assert_success_ref(qts,
        "{'execute': 'qom-list', 'arguments': {"
        "'path': '/machine/xtalk/baseio0'}}"));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/origin2000/xtalk-owner/prepare", test_owner);
    return g_test_run();
}
