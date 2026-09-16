/*
 * The format-2 builder (sn-machine.c): the named faults
 * sgi_machine_validate_topology rejects before creating any device
 * (sn-builder-fault-test's fixture, a minimal two-node topology),
 * plus the command-line contract every product shares -- -numa
 * refused, an unsupported -smp population rejected, and a topology
 * belonging to a different product refused -- checked against the
 * real Origin 200 machine. The console field test drives the same
 * fixture file's sn-builder-console-test machine, a real two-node
 * build the fault fixture never realizes.
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

static void assert_fault_rejected(const char *fault, const char *message)
{
    g_autofree char *machine = g_strdup_printf("sn-builder-fault-test,"
                                               "fault=%s", fault);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", machine, "-display", "none", NULL
    };

    assert_qemu_rejects(args, message);
}

static void test_link_end_out_of_range(const void *data)
{
    assert_fault_rejected("link-end-range",
                          "link 'link0' end 0 names vertex 99, past 2");
}

static void test_node_in_two_modules(const void *data)
{
    assert_fault_rejected("node-in-two-modules",
                          "node 0 is in two modules");
}

static void test_cpu_slot_beyond_interfaces(const void *data)
{
    assert_fault_rejected("cpu-slot-range",
                          "names a CPU slot beyond its ASIC's 1 "
                          "interface(s) of 2 each");
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
     * table; topology= names one by string alone, with no other
     * check to keep a product on its own wiring. Origin 2000's
     * "origin2000-module" descriptor already sits in that table (the
     * format-2 goldens merge), unreachable through its own product
     * only because Origin 2000 has no machine class here yet -- once
     * it does, the same name would resolve just as well for Origin
     * 200. This is refused the way any other unknown name is.
     */
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200,topology=origin2000-module",
        "-display", "none", NULL
    };

    assert_qemu_rejects(args, "unknown topology 'origin2000-module'");
}

/*
 * sn-builder-console-test (sn-builder-fault-test.c) is a two-node topology
 * with console=1: node 1's IOC3 port A, not node 0's, must land on serial
 * line 0. Give the machine exactly one backend, at line 0, and read it
 * back off each node's IOC3 "chardev-a" property -- a chardev property
 * reports the backend's own id once bound (qdev-properties-system.c's
 * get_chr), empty string unbound, so this fails the way the removed
 * boot-and-quit version could not: node 0 answering "probe" instead of
 * node 1 would still exit 0.
 */
static void test_console_field_puts_its_node_on_serial_line_zero(
    const void *data)
{
    QTestState *qts = qtest_init(
        "-M sn-builder-console-test,activate=on -m 128M "
        "-smp 4,sockets=2,cores=2,threads=1 -cpu R10000 "
        "-chardev null,id=probe -serial chardev:probe -display none");
    g_autofree char *node0_chardev_a =
        qom_get_str(qts, "/machine/node0/ioc3", "chardev-a");
    g_autofree char *node1_chardev_a =
        qom_get_str(qts, "/machine/node1/ioc3", "chardev-a");

    g_assert_cmpstr(node1_chardev_a, ==, "probe");
    g_assert_cmpstr(node0_chardev_a, ==, "");

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
                        "topology 'origin200' does not accept 768 MiB per node");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn/builder/link-end-out-of-range", NULL,
                        test_link_end_out_of_range);
    qtest_add_data_func("sn/builder/node-in-two-modules", NULL,
                        test_node_in_two_modules);
    qtest_add_data_func("sn/builder/cpu-slot-beyond-interfaces", NULL,
                        test_cpu_slot_beyond_interfaces);
    qtest_add_data_func("sn/builder/unknown-fault-reported", NULL,
                        test_unknown_fault_reported);
    qtest_add_data_func("sn/builder/numa-refused", NULL,
                        test_numa_refused);
    qtest_add_data_func("sn/builder/unsupported-smp-population-rejected",
                        NULL, test_unsupported_smp_population_rejected);
    qtest_add_data_func("sn/builder/topology-from-another-product-refused",
                        NULL, test_topology_from_another_product_refused);
    qtest_add_data_func("sn/builder/console-node-gets-serial-line-zero", NULL,
                        test_console_field_puts_its_node_on_serial_line_zero);
    qtest_add_data_func("sn/builder/unsupported-memory-population-rejected",
                        NULL, test_unsupported_memory_population_rejected);

    return g_test_run();
}
