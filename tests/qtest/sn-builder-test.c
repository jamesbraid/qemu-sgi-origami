/*
 * The format-2 builder (sn-machine.c): the command-line contract every
 * product shares -- -numa refused, an unsupported -smp or -m population
 * rejected, and a topology belonging to a different product refused --
 * checked against the real Origin 200, Origin 200 (dual) and Origin 2000
 * machines, whose every accepted population is also checked to build.
 * The console field test drives tests/qtest/sn-builder-fault-test.c's
 * sn-builder-console-test machine, a real two-node build.
 *
 * The named structural faults sgi_machine_validate_topology used to
 * reject here (a minimal two-node fixture topology fed straight to that
 * validator) are gone along with it: hw/mips/sgi/sn-machine.c now trusts
 * the compiled descriptor, and tests/unit/test-sn-model.c checks those
 * invariants once instead. See docs/plans/sn-machine-model.md, "The
 * builder trusts the descriptor".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <sys/wait.h>
#include "libqtest.h"
#include "qobject/qdict.h"

static char *qom_get_str(QTestState *qts, const char *path, const char *prop)
{
    QDict *r = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                         "{'path': %s, 'property': %s}}", path, prop);
    char *value = g_strdup(qdict_get_str(r, "return"));

    qobject_unref(r);
    return value;
}

static void assert_qemu_rejects(const char *const args[], const char *message)
{
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    /*
     * A rejection reports through error_report and exits; it must not
     * reach the guest as a signal (SIGABRT from a failed assertion,
     * SIGSEGV from a wild pointer). WIFSIGNALED tells the two apart where
     * a bare nonzero exit status cannot.
     */
    g_assert_false(WIFSIGNALED(wait_status));
    g_assert_nonnull(strstr(error_output, message));
}

/*
 * The QOM type of the named child of a qom-list'd parent, or NULL if no
 * such child exists: proves which board class the builder actually
 * instantiated at a slot, independent of any property both board
 * classes happen to share.
 */
static char *qom_child_type(QTestState *qts, const char *parent,
                            const char *child)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'qom-list',"
                                     " 'arguments': { 'path': %s } }",
                                     parent);
    QList *children;
    const QListEntry *entry;
    char *found = NULL;

    g_assert_true(qdict_haskey(response, "return"));
    children = qdict_get_qlist(response, "return");
    for (entry = qlist_first(children); entry; entry = qlist_next(entry)) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));
        const char *type = qdict_get_try_str(item, "type");

        if (type && g_str_has_prefix(type, "child<") &&
            g_str_equal(qdict_get_str(item, "name"), child)) {
            found = g_strndup(type + strlen("child<"),
                              strlen(type) - strlen("child<") - 1);
        }
    }

    qobject_unref(response);
    return found;
}

static void test_numa_refused(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-m", "64M",
        "-object", "memory-backend-ram,id=ram0,size=64M",
        "-numa", "node,nodeid=0,memdev=ram0",
        "-display", "none", NULL
    };

    assert_qemu_rejects(args, "origin200: -numa is refused");
}

/*
 * sgi-pci-expansion (board-pci-expansion.c) is not user-creatable: its
 * "fabric" property has nowhere to resolve to before a `-device`
 * realizes (see that class's own header comment), so it stays reachable
 * only through the machine builder or the board-test fixture, not the
 * command line. Values do not matter; the check runs before qdev even
 * tries to set them. -device is processed after sgi_machine_init, so
 * this still needs a -bios that clears the stock-PROM requirement, the
 * same blank flash write_config_block_fixture's sibling in
 * sn0-address-map-test.c uses for the same reason.
 */
static void test_pci_expansion_not_user_creatable(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *blank = g_malloc0(0x100000);
    g_autofree char *path = NULL;
    int fd;
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000", "-bios", NULL,
        "-device", "sgi-pci-expansion,module=1,slot=9",
        "-display", "none", NULL
    };

    fd = g_file_open_tmp("sn-builder-blank-bios-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    close(fd);
    g_assert_true(g_file_set_contents(path, (const char *)blank, 0x100000,
                                      &error));
    g_assert_no_error(error);
    args[4] = path;

    assert_qemu_rejects(args, "Parameter 'driver' expects a pluggable "
                        "device type");
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_unsupported_smp_population_rejected(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-smp", "3",
        "-display", "none", NULL
    };

    assert_qemu_rejects(args,
                        "topology 'origin200' accepts 1 or 2 CPUs, not 3");
}

static void test_topology_from_another_product_refused(const void *data)
{
    /*
     * The compiled catalogue holds every product's topologies in one
     * list; topology= names one by string alone, so
     * sgi_machine_topology() also checks the resolved entry against
     * this machine's own product (sgi_machine_topology_matches_product)
     * before handing it back. Origin 2000's "origin2000-module"
     * descriptor exists and resolves by name just as well for Origin
     * 200's product class, so without that check Origin 200 would
     * build Origin 2000's four-node wiring under its own product.
     * The product mismatch is refused the way any other unknown name
     * is: sgi_machine_topology() returns NULL either way.
     */
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200,topology=origin2000-module",
        "-display", "none", NULL
    };

    assert_qemu_rejects(args, "unknown topology 'origin2000-module'");
}

/*
 * Every accepted -smp/-m population actually builds, for every topology
 * that ships today -- the positive side of the rejection tests above.
 * qtest_enabled() waives the stock-PROM/-bios requirement, so a bare
 * qtest_init() that returns at all, without QEMU exiting first, is the
 * pass: sgi_machine_init ran its validation and built every node.
 */
static void test_origin200_accepts_its_cpu_populations(const void *data)
{
    static const unsigned int cpus[] = { 1, 2 };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(cpus); i++) {
        g_autofree char *args = g_strdup_printf(
            "-M origin200 -smp %u -display none", cpus[i]);
        QTestState *qts = qtest_init(args);

        qtest_quit(qts);
    }
}

static void test_origin200_accepts_its_memory_populations(const void *data)
{
    static const unsigned int mib[] = { 512, 256, 128, 64 };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(mib); i++) {
        g_autofree char *args = g_strdup_printf(
            "-M origin200 -m %uM -display none", mib[i]);
        QTestState *qts = qtest_init(args);

        qtest_quit(qts);
    }
}

static void test_origin200_dual_accepts_its_populations(const void *data)
{
    /*
     * default_ram_size is origin200's own shipped total (one node), so
     * -m must be named explicitly here: left out, it would divide across
     * this topology's two real nodes and fail, not fall back to a value
     * this topology accepts.
     */
    static const unsigned int mib_per_node[] = { 512, 256, 128, 64 };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(mib_per_node); i++) {
        g_autofree char *args = g_strdup_printf(
            "-M origin200,topology=origin200-dual -smp 4 "
            "-m %uM -display none", mib_per_node[i] * 2);
        QTestState *qts = qtest_init(args);

        qtest_quit(qts);
    }
}

static void test_origin2000_module_accepts_its_populations(const void *data)
{
    QTestState *qts = qtest_init("-M origin2000 -smp 8 -display none");

    qtest_quit(qts);
    qts = qtest_init("-M origin2000 -smp 8 -m 256M -display none");
    qtest_quit(qts);
}

/*
 * sn-builder-console-test (sn-builder-fault-test.c) is a two-node topology
 * with console=1: node 1's IOC3 port A, not node 0's, must land on serial
 * line 0. Give the machine exactly one backend, at line 0, and read it
 * back off each node's own onboard sgi-baseio's own IOC3 "chardev-a"
 * property (sn0.c creates the Bridge and the sgi-baseio behind it
 * directly, as QOM children of the node object, since neither is a
 * removable board -- docs/designs/sn-machine-model-architecture.md,
 * "Node devices") -- a chardev property reports the backend's own id
 * once bound (qdev-properties-system.c's get_chr), empty string unbound,
 * so this fails the way the removed boot-and-quit version could not:
 * node 0 answering "probe" instead of node 1 would still exit 0.
 */
static void test_console_field_puts_its_node_on_serial_line_zero(
    const void *data)
{
    QTestState *qts = qtest_init(
        "-M sn-builder-console-test,activate=on -m 128M "
        "-smp 4 -cpu R10000 "
        "-chardev null,id=probe -serial chardev:probe -display none");
    g_autofree char *node0_chardev_a =
        qom_get_str(qts, "/machine/node0/baseio/ioc3", "chardev-a");
    g_autofree char *node1_chardev_a =
        qom_get_str(qts, "/machine/node1/baseio/ioc3", "chardev-a");

    g_assert_cmpstr(node1_chardev_a, ==, "probe");
    g_assert_cmpstr(node0_chardev_a, ==, "");

    qtest_quit(qts);
}

/*
 * tests/unit/test-sn-model.c checks xtalk_attachments/io_boards against
 * node_count once, for the compiled descriptor; this fixture corrupts a
 * copy at runtime instead, so a node missing its own crosstalk
 * attachment only fails once sgi_machine_index_xtalk_attachments looks
 * it up during real construction. This must surface as sgi_machine_
 * init's own reported error and a clean exit, not an aborting assertion
 * or a memory-region collision reaching the guest as a signal.
 */
static void test_node_without_xtalk_attachment_rejected(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M",
        "sn-builder-console-test,"
        "construction-fault=node-without-xtalk-attachment",
        "-m", "128M", "-smp", "4",
        "-cpu", "R10000",
        /*
         * The fault is reported before flash content is ever read, so
         * this only needs to clear the "-bios or pflash" precondition,
         * not name a real image.
         */
        "-bios", "/dev/null", "-display", "none", NULL
    };

    assert_qemu_rejects(args, "node 1 names no direct-attach I/O board or "
                        "crosstalk attachment");
}

/*
 * The builder dispatches io_boards[] entries on the descriptor's own
 * board_kind alone (sgi_machine_instantiate_io_board), not on how the
 * board attaches: Origin 200's onboard I/O and Origin 2000's BaseIO card
 * both, historically, attached at widget 8 on their own module, and both
 * happen to also differ by attachment kind (direct vs. Crossbow), which
 * is exactly what let a board_kind data defect go unnoticed before.
 * Checking what each carrier actually builds, not merely that something
 * builds, is what a heuristic that happened to agree on both shipped
 * products could not catch: Origin 200's onboard sgi-baseio must carry
 * the "ip29" preset, and Origin 2000's onboard board must be the
 * sgi-xio-baseio card class, not merely something at widget 8.
 */
static void test_origin200_onboard_board_is_ip29_preset(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -display none");
    g_autofree char *preset =
        qom_get_str(qts, "/machine/node0/baseio", "preset");

    g_assert_cmpstr(preset, ==, "ip29");
    qtest_quit(qts);
}

static void test_origin2000_onboard_board_is_xio_baseio(const void *data)
{
    QTestState *qts = qtest_init("-M origin2000 -display none");
    g_autofree char *type = qom_child_type(qts, "/machine/module0", "slot8");

    g_assert_cmpstr(type, ==, "sgi-xio-baseio");
    qtest_quit(qts);
}

/*
 * The node-types step lets a Bedrock topology carry more than one node in
 * the descriptor: nothing in the builder asserts node_count == 1 for
 * Bedrock any more. No shipped product exercises this yet (the
 * two-chassis Origin 300 stays reference-only in the compiled
 * catalogue), so this drives the real builder over
 * sn-builder-fault-test.c's own synthetic two-node fixture instead: two
 * Bedrock nodes, each with its own populated IO-8 board at its own
 * attachment, no controllers. The builder's population/memory
 * validation and the node loop's per-node board/widget selection both
 * run for two nodes here -- proving neither one is hardcoded to a
 * single Bedrock node any more -- and every node becomes a real,
 * configured device, each already at its own nasid-strided
 * address-space window (sn1-node.c), so realize no longer collides.
 * What still refuses, by name, is the cross-node NUMAlink fabric: no
 * code sources or models link traffic between Bedrock nodes, so a
 * second node would realize with no way to reach, or be reached by,
 * the first. This is the expected, current boundary, not a crash: the
 * machine reports it and exits cleanly.
 */
static void test_bedrock_topology_accepts_two_nodes(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "sn-builder-bedrock-test,activate=on",
        "-m", "128M", "-smp", "2,sockets=2,cores=1,threads=1",
        "-display", "none", NULL
    };

    assert_qemu_rejects(args, "names 2 Bedrock nodes; the cross-node "
                        "NUMAlink fabric between Bedrock nodes is not "
                        "modeled yet");
}

/*
 * The topology-acceptance test above proves the builder no longer
 * refuses a two-node Bedrock descriptor on structure; it cannot prove
 * the two nodes' address-space windows actually land clear of each
 * other, because the machine-level refusal (still in place for the
 * unmodeled cross-node NUMAlink fabric) stops every such topology
 * before any node past the first realizes. sn-builder-bedrock-nasid-
 * test (sn-builder-fault-test.c) bypasses that refusal by building two
 * sgi-sn1-node devices directly, at nasid 0 and nasid 1, the same way
 * sn-machine.c's own per-node loop would. Both nodes' board-
 * identification register -- one of the fixed windows sn1-node.c
 * strides by nasid -- must answer 0x8001 at its own node's address, and
 * node 0's answer must still be correct with node 1 realized alongside
 * it: if the two nodes' windows collided instead of landing at their
 * own nasid-strided addresses, realizing the second node would either
 * abort on the resulting overlapping memory region or leave one node's
 * register reachable at the other's expense, not answer both correctly
 * from two distinct addresses the way real per-nasid decoding does.
 */
#define NASID_TEST_SPACE_HUB UINT64_C(0x10000000000)
#define NASID_TEST_HUB_LOCAL_REGISTERS \
    (NASID_TEST_SPACE_HUB + UINT64_C(0x10000000))
#define NASID_TEST_BOARD_ID_LOCAL \
    (NASID_TEST_HUB_LOCAL_REGISTERS + UINT64_C(0x4000000))
#define NASID_TEST_BOARD_ID_VALUE UINT64_C(0x8001)
/* M mode's node field sits at physical address bit 33 (ip35-address-map.md). */
#define NASID_TEST_NODE_ADDRESS_SHIFT 33

static void test_bedrock_nodes_answer_at_their_own_nasid_window(
    const void *data)
{
    /*
     * -nodefaults: each node's L1 unconditionally takes serial_hd(0)
     * for its console (sn1_node_create_bedrock); the default machine
     * would hand both nodes the same backend and the second qdev_realize
     * would refuse it as already in use. Real per-node console routing
     * is unmodeled and out of this fixture's addressing-only scope.
     */
    QTestState *qts = qtest_init(
        "-M sn-builder-bedrock-nasid-test,activate=on -cpu R14000 "
        "-smp 2 -nodefaults -display none");
    uint64_t node1_offset = UINT64_C(1) << NASID_TEST_NODE_ADDRESS_SHIFT;

    g_assert_cmphex(qtest_readq(qts, NASID_TEST_BOARD_ID_LOCAL), ==,
                    NASID_TEST_BOARD_ID_VALUE);
    g_assert_cmphex(qtest_readq(qts, NASID_TEST_BOARD_ID_LOCAL +
                                node1_offset), ==,
                    NASID_TEST_BOARD_ID_VALUE);
    /* Node 0 must still answer correctly with node 1 realized too. */
    g_assert_cmphex(qtest_readq(qts, NASID_TEST_BOARD_ID_LOCAL), ==,
                    NASID_TEST_BOARD_ID_VALUE);

    qtest_quit(qts);
}

static void test_unsupported_memory_population_rejected(const void *data)
{
    /*
     * Origin 200 accepts 512, 256, 128, and 64 MiB per node.
     * -m 768M (single node, default topology) is not in the accepted list.
     */
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-m", "768M",
        "-display", "none", NULL
    };

    assert_qemu_rejects(args,
                        "topology 'origin200' does not accept 768 "
                        "MiB per node");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn/builder/numa-refused", NULL,
                        test_numa_refused);
    qtest_add_data_func("sn/builder/pci-expansion-not-user-creatable", NULL,
                        test_pci_expansion_not_user_creatable);
    qtest_add_data_func("sn/builder/unsupported-smp-population-rejected",
                        NULL, test_unsupported_smp_population_rejected);
    qtest_add_data_func("sn/builder/topology-from-another-product-refused",
                        NULL, test_topology_from_another_product_refused);
    qtest_add_data_func("sn/builder/console-node-gets-serial-line-zero", NULL,
                        test_console_field_puts_its_node_on_serial_line_zero);
    qtest_add_data_func("sn/builder/node-without-xtalk-attachment-rejected",
                        NULL, test_node_without_xtalk_attachment_rejected);
    qtest_add_data_func("sn/builder/unsupported-memory-population-rejected",
                        NULL, test_unsupported_memory_population_rejected);
    qtest_add_data_func("sn/builder/bedrock-topology-accepts-two-nodes",
                        NULL, test_bedrock_topology_accepts_two_nodes);
    qtest_add_data_func(
        "sn/builder/bedrock-nodes-answer-at-their-own-nasid-window", NULL,
        test_bedrock_nodes_answer_at_their_own_nasid_window);
    qtest_add_data_func("sn/builder/origin200-accepts-its-cpu-populations",
                        NULL, test_origin200_accepts_its_cpu_populations);
    qtest_add_data_func("sn/builder/origin200-accepts-its-memory-populations",
                        NULL, test_origin200_accepts_its_memory_populations);
    qtest_add_data_func("sn/builder/origin200-dual-accepts-its-populations",
                        NULL, test_origin200_dual_accepts_its_populations);
    qtest_add_data_func(
        "sn/builder/origin2000-module-accepts-its-populations", NULL,
        test_origin2000_module_accepts_its_populations);
    qtest_add_data_func("sn/builder/origin200-onboard-board-is-ip29-preset",
                        NULL, test_origin200_onboard_board_is_ip29_preset);
    qtest_add_data_func("sn/builder/origin2000-onboard-board-is-xio-baseio",
                        NULL, test_origin2000_onboard_board_is_xio_baseio);

    return g_test_run();
}
