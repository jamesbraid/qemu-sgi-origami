/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The machine-level limits of the per-node station address and per-module
 * serial helpers in hw/mips/sgi/board-mac.c. No catalogued machine reaches
 * them (the largest has 8 nodes and Origin 2000 machines have one module),
 * so the helpers the machines call are tested directly, with the exact
 * messages a machine that did would fail with.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "net/util.h"
#include "hw/mips/sgi/sgi-board-mac.h"

/*
 * The helpers parse addresses with net_parse_macaddr(), which lives with the
 * rest of the network layer; this test supplies the one function it needs.
 */
int net_parse_macaddr(uint8_t *macaddr, const char *p)
{
    unsigned int b[6];
    char tail;

    if (sscanf(p, "%x:%x:%x:%x:%x:%x%c", &b[0], &b[1], &b[2], &b[3], &b[4],
               &b[5], &tail) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        macaddr[i] = b[i];
    }
    return 0;
}

static SGIBoardId resolved(SGIBoardFamily family, unsigned int emu_id,
                           const char *hostid, const uint8_t *mac)
{
    SGIBoardId id;
    char message[SGI_BOARD_ERROR_SIZE];

    g_assert_true(sgi_board_id_resolve(family, SGI_BOARD_KIND, emu_id, NULL,
                                       hostid, mac, &id, message));
    return id;
}

/* The default address numbers 16 nodes or ports and refuses the 17th. */
static void test_node_limit(void)
{
    SGIBoardId id = resolved(SGI_BOARD_FAMILY_ADDRESS, 4095, NULL, NULL);
    Error *err = NULL;
    char *mac;

    for (unsigned int node = 0; node < 16; node++) {
        char *want = g_strdup_printf("08:00:69:e1:ff:f%x", node);

        mac = sgi_board_mac(NULL, &id, node, &error_abort);
        g_assert_cmpstr(mac, ==, want);
        g_free(mac);
        g_free(want);
    }
    g_assert_null(sgi_board_mac(NULL, &id, 16, &err));
    g_assert_cmpstr(error_get_pretty(err), ==, "the default station address "
                    "numbers at most 16 nodes or ports, and node 16 does not "
                    "fit; set mac=");
    error_free(err);
}

/* A user base that would carry out of the 08:00:69 block is refused. */
static void test_carry(void)
{
    static const uint8_t base[6] = { 0x08, 0x00, 0x69, 0xff, 0xff, 0xff };
    SGIBoardId id = resolved(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, base);
    Error *err = NULL;
    char *mac;

    mac = sgi_board_mac("08:00:69:ff:ff:ff", &id, 0, &error_abort);
    g_assert_cmpstr(mac, ==, "08:00:69:ff:ff:ff");
    g_free(mac);
    g_assert_null(sgi_board_mac("08:00:69:ff:ff:ff", &id, 1, &err));
    g_assert_cmpstr(error_get_pretty(err), ==, "node 1 of "
                    "mac=08:00:69:ff:ff:ff would leave the 08:00:69 block, "
                    "which this machine's station address must stay in; use "
                    "mac=08:00:69:xx:xx:xx with room for the nodes, or a "
                    "lower hostid");
    error_free(err);
    err = NULL;
    mac = sgi_board_mac("08:00:69:ff:ff:fe", &id, 1, &error_abort);
    g_assert_cmpstr(mac, ==, "08:00:69:ff:ff:ff");
    g_free(mac);

    /* An id from hostid carries the same way. */
    id = resolved(SGI_BOARD_FAMILY_ADDRESS, 0, "ffffff", NULL);
    g_assert_null(sgi_board_mac(NULL, &id, 1, &err));
    error_free(err);
    err = NULL;
    /* The text families keep any base. */
    id = resolved(SGI_BOARD_FAMILY_TEXT, 0, NULL, NULL);
    mac = sgi_board_mac("08:00:69:ff:ff:ff", &id, 1, &error_abort);
    g_assert_cmpstr(mac, ==, "08:00:6a:00:00:00");
    g_free(mac);
}

/* A module serial numbers 10 modules and refuses the 11th. */
static void test_module_limit(void)
{
    SGIBoardId id = resolved(SGI_BOARD_FAMILY_TEXT, 42, NULL, NULL);
    Error *err = NULL;
    char *text;

    for (unsigned int module = 0; module < 10; module++) {
        char *want = g_strdup_printf("KEMU10042%u", module);

        text = sgi_board_default_module_serial(&id, module, &error_abort);
        g_assert_cmpstr(text, ==, want);
        g_free(text);
        g_free(want);
    }
    g_assert_null(sgi_board_default_module_serial(&id, 10, &err));
    g_assert_cmpstr(error_get_pretty(err), ==, "module serials number at "
                    "most 10 modules, and module 10 does not fit");
    error_free(err);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-board-mac/node-limit", test_node_limit);
    g_test_add_func("/sgi-board-mac/carry", test_carry);
    g_test_add_func("/sgi-board-mac/module-limit", test_module_limit);
    return g_test_run();
}
