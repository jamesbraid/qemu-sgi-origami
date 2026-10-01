/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The machine gives a placed XIO board its graphics credit link: a board
 * with a "pi" link fitted by slot name is linked to the Hub of the node that
 * controls the slot, and one given a link outright keeps it. The board is
 * sgi-xio-pi-test-board, the test board with that link and nothing else.
 *
 * On an Origin 2000 io3 is controlled by node 0 and io4 by node 2. A placed
 * board moves under its module, so it is found by its id alone.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

static char *board_pi(QTestState *qts)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'qom-get',"
        "  'arguments': { 'path': 'b',"
        "  'property': 'pi' } }");

    g_assert(qdict_haskey(response, "return"));
    return g_strdup(qdict_get_str(response, "return"));
}

static void check_link(const char *device, const char *expected)
{
    QTestState *qts = qtest_initf("-M origin2000 -display none -S "
                                  "-device %s", device);
    g_autofree char *pi = board_pi(qts);

    g_assert_cmpstr(pi, ==, expected);
    qtest_quit(qts);
}

/* A slot node 0 controls: node 0's Hub. */
static void test_slot_on_node_0(void)
{
    check_link("sgi-xio-pi-test-board,id=b,slot=io3", "/machine/node0/hub");
}

/* A slot another node controls: that node's Hub, not node 0's. */
static void test_slot_on_node_2(void)
{
    check_link("sgi-xio-pi-test-board,id=b,slot=io4", "/machine/node2/hub");
}

/* A board given its link on the command line keeps it. */
static void test_preset_link_is_kept(void)
{
    check_link("sgi-xio-pi-test-board,id=b,slot=io4,pi=/machine/node1/hub",
               "/machine/node1/hub");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/pi-link/slot-on-node-0", test_slot_on_node_0);
    qtest_add_func("sn0/pi-link/slot-on-node-2", test_slot_on_node_2);
    qtest_add_func("sn0/pi-link/preset-link-is-kept",
                   test_preset_link_is_kept);
    return g_test_run();
}
