/*
 * Origin 2000 Crosstalk wiring, on the real machine.
 *
 * The family builder (hw/mips/sgi/sn-machine.c) builds each crossbar the
 * topology names as its own direct child of the machine and realizes it
 * only at the deferred machine-init-done hook (sgi_machine_attach_
 * crossbars), once the onboard BaseIO board built alongside it -- and any
 * `-device sgi-pci-expansion` a command line adds -- has had its chance
 * to attach; this checks that wiring is there once origin2000-module has
 * actually been built, rather than through a dedicated fixture machine's
 * private "verified" flag.
 *
 * links-present now reads every widget position (0xff00) on both
 * Crossbows regardless of what this shipped module populates: a real
 * Crossbow physically wires all eight external links (docs/datasheets/
 * xbow.md, "Host ownership and port width"), so a later board has one to
 * attach to without widening this constant, and startup-links-alive
 * keeps reporting only what is actually populated at boot -- the same
 * masks this test checked before the board-devices step's own board-
 * wiring part widened the first property (0x700/0x600, unchanged).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

static uint64_t xtalk_uint(QTestState *qts, const char *crossbow,
                           const char *property)
{
    g_autofree char *path = g_strdup_printf("/machine/%s", crossbow);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': %s,"
        "'property': %s}}", path, property);

    g_assert_false(qdict_haskey(response, "error"));
    return qdict_get_int(response, "return");
}

static void assert_crossbow(QTestState *qts, const char *crossbow,
                            uint16_t startup_alive)
{
    g_autofree char *path = g_strdup_printf("/machine/%s", crossbow);

    g_assert_cmphex(xtalk_uint(qts, crossbow, "span"), ==,
                    UINT64_C(0x400000000));
    g_assert_cmphex(xtalk_uint(qts, crossbow, "links-present"), ==, 0xff00);
    g_assert_cmphex(xtalk_uint(qts, crossbow, "startup-links-alive"), ==,
                    startup_alive);
    g_assert_false(qtest_qom_get_bool(qts, path, "stop-on-link-reset"));
    g_assert_true(qtest_qom_get_bool(qts, path,
                                     "complete-posted-error-address-writes"));
    g_assert_cmphex(xtalk_uint(qts, crossbow, "link-reset-restore-alive"),
                    ==, startup_alive);
}

/*
 * The shipped module: Crossbow 1 has three links alive at boot (widgets
 * 8-10, BaseIO plus its two host nodes), Crossbow 2 has two (widgets
 * 9-10, its two host nodes only); no XIO position is populated on
 * either. BaseIO itself re-parents under its module the way every
 * sgi-xtalk-board does (include/hw/mips/sgi/xtalk-board.h), landing at
 * its own slot name since this builder gives it no id.
 */
static void test_owner(void)
{
    QTestState *qts = qtest_initf("-M origin2000 -m 256M "
                                  "-nodefaults -display none -serial none");

    assert_crossbow(qts, "crossbow0", 0x700);
    assert_crossbow(qts, "crossbow1", 0x600);
    /* A qom-list on a path that does not exist fails; this one must not. */
    qobject_unref(qtest_qmp_assert_success_ref(qts,
        "{'execute': 'qom-list', 'arguments': {"
        "'path': '/machine/module0/slot8'}}"));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/origin2000/xtalk-owner/prepare", test_owner);
    return g_test_run();
}
