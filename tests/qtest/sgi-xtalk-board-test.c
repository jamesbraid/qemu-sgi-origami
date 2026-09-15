/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-xtalk-board: property validation, re-parenting under its module,
 * and the deferred attach hook that lets a `-device` board reach a
 * Crossbow that must realize only once every board is in.
 *
 * -M none's main system bus refuses to hot-plug a sysbus device at all
 * (no dynamic-sysbus allowance is wired up without a machine, and this
 * packet adds none), so every device here is a cold-plugged `-device` on
 * the qtest command line, never a QMP device_add sent afterward. A board
 * whose properties fail realize takes the whole process down as a fatal
 * command-line error, so the invalid-property cases run as trapped
 * subprocesses and check the process's own failure, the way
 * test_parent_l1_invalid_region() in sn1-bedrock-test.c checks a
 * different device's realize-time refusal.
 *
 * The attach-pending case uses sgi-xtalk-board-fixture, a test-only
 * stand-in for a machine builder: it owns one Crossbow, created
 * unrealized, and (with auto-attach left at its default) calls
 * sgi_xtalk_board_attach_pending() from the real machine-init-done
 * notifier the way a builder would, then realizes the Crossbow. A second
 * fixture with auto-attach=false does neither on its own; its
 * "realize-now" and "retry" properties let the qtest drive the same two
 * steps by hand, which is the only way to put a Crossbow into "already
 * realized, nothing attached" without a hot-plug -M none refuses.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

/*
 * Matches SGI_XTALK_BOARD_FIXTURE_SPAN in hw/mips/sgi/xtalk-test-board.c:
 * how much address space the fixture's Crossbow gives each widget.
 */
#define WIDGET_SPAN (32ULL * 1024 * 1024)

/*
 * Board slot 8 is what fixture0's Crossbow keeps alive alongside 9, the
 * port this test probes from (a link never routes to itself).
 */
#define BOARD_WIDGET 8
#define PROBE_LINK   1 /* widget 9, the second live port */

#define CPU_BASE    0x40000000ULL
#define TARGET_BASE 0x80000000ULL

#define FIXTURE0 "/machine/peripheral/fixture0"
#define FIXTURE1 "/machine/peripheral/fixture1"

static char *qom_get_str(QTestState *qts, const char *path, const char *prop)
{
    QDict *r = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                         "{'path': %s, 'property': %s}}", path, prop);
    char *value = g_strdup(qdict_get_str(r, "return"));

    qobject_unref(r);
    return value;
}

static void test_bad_module(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*a crosstalk board requires a module index*");
        return;
    }
    /* No module= at all: the property keeps its unset sentinel. */
    s = qtest_init("-M none -device sgi-xtalk-test-board,id=b,slot=8 -S");
    qtest_quit(s);
}

static void test_bad_slot_low(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*a crosstalk board slot must name a Crossbow widget*");
        return;
    }
    /* Below the Crossbow's first widget. */
    s = qtest_init(
        "-M none -device sgi-xtalk-test-board,id=b,module=0,slot=7 -S");
    qtest_quit(s);
}

static void test_bad_slot_high(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*a crosstalk board slot must name a Crossbow widget*");
        return;
    }
    /* At and beyond the last one. */
    s = qtest_init(
        "-M none -device sgi-xtalk-test-board,id=b,module=0,slot=16 -S");
    qtest_quit(s);
}

static void test_slot_taken_twice(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*a crosstalk board already occupies module 0 slot 8*");
        return;
    }
    s = qtest_init(
        "-M none -device sgi-xtalk-test-board,id=a,module=0,slot=8 "
        "-device sgi-xtalk-test-board,id=b,module=0,slot=8 -S");
    qtest_quit(s);
}

static void test_slot_reused_in_another_module(void)
{
    /*
     * The same slot number in a different module is unrelated and free:
     * a success case, so it runs directly rather than trapped.
     */
    QTestState *qts = qtest_init(
        "-M none -device sgi-xtalk-test-board,id=a,module=0,slot=8 "
        "-device sgi-xtalk-test-board,id=b,module=1,slot=8 -S");

    g_assert_true(qtest_qom_get_bool(qts, "/machine/module0/a", "realized"));
    g_assert_true(qtest_qom_get_bool(qts, "/machine/module1/b", "realized"));
    qtest_quit(qts);
}

static void test_reparent(void)
{
    QTestState *qts = qtest_init(
        "-M none -device sgi-xtalk-test-board,id=solo,module=3,slot=10 -S");

    g_assert_true(
        qtest_qom_get_bool(qts, "/machine/module3/solo", "realized"));
    qtest_quit(qts);
}

/*
 * A board built before the machine-init-done notifier fires attaches
 * through sgi_xtalk_board_attach_pending() and is reachable through the
 * Crossbow once it realizes; a board whose Crossbow the qtest realizes
 * by hand first (standing in for one that already realized before this
 * board could reach it) is refused when the hook is then run against it.
 */
static void test_attach_pending(void)
{
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S "
        "-device sgi-xtalk-board-fixture,id=fixture0,module=0 "
        "-device sgi-xtalk-test-board,id=board0,module=0,slot=%d "
        "-device sgi-xtalk-board-fixture,id=fixture1,module=1,"
        "auto-attach=false "
        "-device sgi-xtalk-test-board,id=board1,module=1,slot=8",
        BOARD_WIDGET);
    g_autofree char *error = NULL;

    /*
     * fixture0's own machine-init-done notifier already ran by the time
     * this process answers its first QMP command.
     */
    g_assert_true(qtest_qom_get_bool(qts, FIXTURE0, "attached"));
    error = qom_get_str(qts, FIXTURE0, "error");
    g_assert_cmpstr(error, ==, "");

    /*
     * The Crossbow sees the widget at the slot's widget number: write
     * through the probe port's view of widget 8, read back through the
     * test target's own mapping of the same underlying RAM.
     */
    qtest_sysbus_mmio_map(qts, FIXTURE0 "/crossbow", PROBE_LINK, CPU_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/module0/board0/widget-module0-slot8",
                          0, TARGET_BASE);
    qtest_writeq(qts, CPU_BASE + BOARD_WIDGET * WIDGET_SPAN,
                0x1122334455667788ULL);
    g_assert_cmphex(qtest_readq(qts, TARGET_BASE), ==,
                    0x1122334455667788ULL);

    /*
     * board1 realized and re-parented fine -- it never touched a
     * Crossbow at its own realize -- but fixture1 left its Crossbow
     * unrealized (auto-attach=false), so board1 is still pending.
     */
    g_assert_true(
        qtest_qom_get_bool(qts, "/machine/module1/board1", "realized"));
    g_assert_false(qtest_qom_get_bool(qts, FIXTURE1, "attached"));

    /*
     * Put fixture1's Crossbow where a real one would be if it had
     * already realized before board1 could reach it, then run the same
     * hook board1 was always going to need.
     */
    qtest_qom_set_bool(qts, FIXTURE1, "realize-now", true);
    qtest_qom_set_bool(qts, FIXTURE1, "retry", true);
    g_free(error);
    error = qom_get_str(qts, FIXTURE1, "error");
    g_assert_true(*error != '\0');

    /* fixture0's earlier success is untouched by fixture1's failure. */
    g_assert_true(qtest_qom_get_bool(qts, FIXTURE0, "attached"));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sgi-xtalk-board/bad-module", test_bad_module);
    qtest_add_func("/sgi-xtalk-board/bad-slot-low", test_bad_slot_low);
    qtest_add_func("/sgi-xtalk-board/bad-slot-high", test_bad_slot_high);
    qtest_add_func("/sgi-xtalk-board/slot-taken-twice",
                  test_slot_taken_twice);
    qtest_add_func("/sgi-xtalk-board/slot-reused-in-another-module",
                  test_slot_reused_in_another_module);
    qtest_add_func("/sgi-xtalk-board/reparent", test_reparent);
    qtest_add_func("/sgi-xtalk-board/attach-pending", test_attach_pending);

    return g_test_run();
}
