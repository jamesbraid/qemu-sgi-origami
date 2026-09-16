/*
 * The format-2 builder (sn-machine.c): the named faults
 * sgi_machine_validate_topology rejects before creating any device
 * (sn-builder-fault-test's fixture, a minimal two-node topology),
 * plus the command-line contract every product shares -- -numa
 * refused and an unsupported -smp population rejected -- checked
 * against the real Origin 200 machine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

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

    return g_test_run();
}
