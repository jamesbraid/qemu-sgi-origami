/*
 * The format-2 builder (sn-machine.c): the named faults
 * sgi_machine_validate_topology rejects before creating any device
 * (sn-builder-fault-test's fixture, a minimal two-node topology), plus
 * the command-line contract every product shares -- -numa refused, an
 * unsupported -smp or -m population rejected, and a topology belonging
 * to a different product refused -- checked against the real Origin
 * 200, Origin 200 (dual) and Origin 2000 machines, whose every accepted
 * population is also checked to build. The console field test drives
 * the same fixture file's sn-builder-console-test machine, a real
 * two-node build the fault fixture never realizes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
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

static void assert_fault_rejected(const char *fault, const char *message)
{
    g_autofree char *machine = g_strdup_printf("sn-builder-fault-test,"
                                               "fault=%s", fault);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", machine, "-display", "none", NULL
    };

    assert_qemu_rejects(args, message);
}

static void test_empty_topology(const void *data)
{
    assert_fault_rejected("empty-topology", "names no nodes");
}

static void test_module_node_out_of_range(const void *data)
{
    assert_fault_rejected("module-node-range",
                          "module 'm1' names node 99, past its 2 nodes");
}

static void test_node_in_two_modules(const void *data)
{
    assert_fault_rejected("node-in-two-modules",
                          "node 0 is in two modules");
}

static void test_node_in_no_module(const void *data)
{
    assert_fault_rejected("node-in-no-module", "node 1 is in no module");
}

static void test_unknown_asic_kind(const void *data)
{
    assert_fault_rejected("unknown-asic-kind",
                          "node 0 names an unknown node ASIC kind");
}

static void test_cpu_slot_beyond_interfaces(const void *data)
{
    assert_fault_rejected("cpu-slot-range",
                          "names a CPU slot beyond its ASIC's 1 "
                          "interface(s) of 2 each");
}

static void test_link_end_out_of_range(const void *data)
{
    assert_fault_rejected("link-end-range",
                          "link 'link0' end 0 names vertex 99, past 2");
}

static void test_unknown_vertex_kind(const void *data)
{
    assert_fault_rejected("unknown-vertex-kind",
                          "link 'link0' names an unknown fabric vertex kind");
}

static void test_router_port_out_of_range(const void *data)
{
    assert_fault_rejected("router-port-range",
                          "link 'link0' end 0 names port 99, outside "
                          "router 0's 1-2 hardware-numbered ports");
}

static void test_duplicate_link_end(const void *data)
{
    assert_fault_rejected("duplicate-link-end",
                          "links 'link1' and 'link0' share one fabric end");
}

static void test_unsupported_io_board_kind(const void *data)
{
    assert_fault_rejected("unsupported-io-board-kind",
                          "io board 'test-board' names an unsupported "
                          "board kind");
}

static void test_unknown_fault_reported(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "sn-builder-fault-test,fault=bogus",
        "-display", "none", NULL
    };

    assert_qemu_rejects(args, "unknown sn-builder-fault-test fault 'bogus'");
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
                        "topology 'origin200' does not accept 3 CPUs "
                        "as 1 socket(s)");
}

static void test_topology_from_another_product_refused(const void *data)
{
    /*
     * sgi_model_entries[] holds every product's topologies in one
     * table; topology= names one by string alone, so
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
            "-M origin200,topology=origin200-dual -smp 4,sockets=2,cores=2 "
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
 * back off each node's onboard board's own IOC3 "chardev-a" property (the
 * board re-parents under its own module, per include/hw/mips/sgi/xtalk-
 * board.h, landing at its own slot name since neither board is given an
 * id) -- a chardev property reports the backend's own id once bound
 * (qdev-properties-system.c's get_chr), empty string unbound, so this
 * fails the way the removed boot-and-quit version could not: node 0
 * answering "probe" instead of node 1 would still exit 0.
 */
static void test_console_field_puts_its_node_on_serial_line_zero(
    const void *data)
{
    QTestState *qts = qtest_init(
        "-M sn-builder-console-test,activate=on -m 128M "
        "-smp 4,sockets=2,cores=2,threads=1 -cpu R10000 "
        "-chardev null,id=probe -serial chardev:probe -display none");
    g_autofree char *node0_chardev_a =
        qom_get_str(qts, "/machine/module0/slot8/ioc3", "chardev-a");
    g_autofree char *node1_chardev_a =
        qom_get_str(qts, "/machine/module1/slot8/ioc3", "chardev-a");

    g_assert_cmpstr(node1_chardev_a, ==, "probe");
    g_assert_cmpstr(node0_chardev_a, ==, "");

    qtest_quit(qts);
}

/*
 * The builder dispatches io_boards[] entries on the descriptor's own
 * board_kind alone (sgi_machine_io_board_type), not on how the board
 * attaches: Origin 200's onboard board and Origin 2000's BaseIO both
 * attach at widget 8 on their own module, and both happen to also
 * differ by attachment kind (direct vs. Crossbow), which is exactly
 * what let a board_kind data defect go unnoticed before. Checking the
 * actual QOM type this builds, not merely that something builds, is
 * what a heuristic that happened to agree on both shipped products
 * could not catch.
 */
static void test_origin200_onboard_board_is_ip29_io(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -display none");
    g_autofree char *type = qom_child_type(qts, "/machine/module0", "slot8");

    g_assert_cmpstr(type, ==, "sgi-ip29-io");
    qtest_quit(qts);
}

static void test_origin2000_onboard_board_is_baseio(const void *data)
{
    QTestState *qts = qtest_init("-M origin2000 -display none");
    g_autofree char *type = qom_child_type(qts, "/machine/module0", "slot8");

    g_assert_cmpstr(type, ==, "sgi-baseio");
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

    qtest_add_data_func("sn/builder/empty-topology", NULL,
                        test_empty_topology);
    qtest_add_data_func("sn/builder/module-node-out-of-range", NULL,
                        test_module_node_out_of_range);
    qtest_add_data_func("sn/builder/node-in-two-modules", NULL,
                        test_node_in_two_modules);
    qtest_add_data_func("sn/builder/node-in-no-module", NULL,
                        test_node_in_no_module);
    qtest_add_data_func("sn/builder/unknown-asic-kind", NULL,
                        test_unknown_asic_kind);
    qtest_add_data_func("sn/builder/cpu-slot-beyond-interfaces", NULL,
                        test_cpu_slot_beyond_interfaces);
    qtest_add_data_func("sn/builder/link-end-out-of-range", NULL,
                        test_link_end_out_of_range);
    qtest_add_data_func("sn/builder/unknown-vertex-kind", NULL,
                        test_unknown_vertex_kind);
    qtest_add_data_func("sn/builder/router-port-out-of-range", NULL,
                        test_router_port_out_of_range);
    qtest_add_data_func("sn/builder/duplicate-link-end", NULL,
                        test_duplicate_link_end);
    qtest_add_data_func("sn/builder/unsupported-io-board-kind", NULL,
                        test_unsupported_io_board_kind);
    qtest_add_data_func("sn/builder/unknown-fault-reported", NULL,
                        test_unknown_fault_reported);
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
    qtest_add_data_func("sn/builder/unsupported-memory-population-rejected",
                        NULL, test_unsupported_memory_population_rejected);
    qtest_add_data_func("sn/builder/origin200-accepts-its-cpu-populations",
                        NULL, test_origin200_accepts_its_cpu_populations);
    qtest_add_data_func("sn/builder/origin200-accepts-its-memory-populations",
                        NULL, test_origin200_accepts_its_memory_populations);
    qtest_add_data_func("sn/builder/origin200-dual-accepts-its-populations",
                        NULL, test_origin200_dual_accepts_its_populations);
    qtest_add_data_func(
        "sn/builder/origin2000-module-accepts-its-populations", NULL,
        test_origin2000_module_accepts_its_populations);
    qtest_add_data_func("sn/builder/origin200-onboard-board-is-ip29-io", NULL,
                        test_origin200_onboard_board_is_ip29_io);
    qtest_add_data_func("sn/builder/origin2000-onboard-board-is-baseio", NULL,
                        test_origin2000_onboard_board_is_baseio);

    return g_test_run();
}
