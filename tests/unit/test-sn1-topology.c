/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/mips/sgi/sn1-topology.h"

/*
 * Every test node below names the full four-slot shape (interface 0's A/B,
 * interface 1's C/D) explicitly, an assembly for a populated slot and NULL
 * for an empty one -- what a real format-2 topology entry always carries,
 * so there is no separate "population mask" to default when it is left
 * unset the way the pre-format-2 SN1MachineModel had.
 */
static const SGIModelProcessor sn1_topology_test_cpu = {
    .id = "topology-test-cpu",
};

/* The shipped two-CPU Origin 300 node: slices A and C, PI1's B unfitted. */
static void test_dense_with_gap(void)
{
    static const SGIModelCpuSlot slots[] = {
        { .interface = 0, .slot = 0, .assembly = &sn1_topology_test_cpu },
        { .interface = 0, .slot = 1, .assembly = NULL },
        { .interface = 1, .slot = 0, .assembly = &sn1_topology_test_cpu },
        { .interface = 1, .slot = 1, .assembly = NULL },
    };
    const SGIModelNode node = {
        .cpu_slots = slots, .cpu_slot_count = ARRAY_SIZE(slots),
    };

    g_assert_cmpuint(sn1_cpu_slot(&node, 0), ==, 0);
    g_assert_cmpuint(sn1_cpu_slot(&node, 1), ==, 2);
    g_assert_cmphex(sn1_cpu_population_mask(&node), ==, 0x5);
}

/* Two physically adjacent slots populated, A and B. */
static void test_contiguous(void)
{
    static const SGIModelCpuSlot slots[] = {
        { .interface = 0, .slot = 0, .assembly = &sn1_topology_test_cpu },
        { .interface = 0, .slot = 1, .assembly = &sn1_topology_test_cpu },
        { .interface = 1, .slot = 0, .assembly = NULL },
        { .interface = 1, .slot = 1, .assembly = NULL },
    };
    const SGIModelNode node = {
        .cpu_slots = slots, .cpu_slot_count = ARRAY_SIZE(slots),
    };

    g_assert_cmpuint(sn1_cpu_slot(&node, 0), ==, 0);
    g_assert_cmpuint(sn1_cpu_slot(&node, 1), ==, 1);
    g_assert_cmphex(sn1_cpu_population_mask(&node), ==, 0x3);
}

/* A one-CPU node whose single populated slot is not the first one. */
static void test_one_cpu_non_default_slot(void)
{
    static const SGIModelCpuSlot slots[] = {
        { .interface = 0, .slot = 0, .assembly = NULL },
        { .interface = 0, .slot = 1, .assembly = NULL },
        { .interface = 1, .slot = 0, .assembly = &sn1_topology_test_cpu },
        { .interface = 1, .slot = 1, .assembly = NULL },
    };
    const SGIModelNode node = {
        .cpu_slots = slots, .cpu_slot_count = ARRAY_SIZE(slots),
    };

    g_assert_cmpuint(sn1_cpu_slot(&node, 0), ==, 2);
    g_assert_cmphex(sn1_cpu_population_mask(&node), ==, 0x4);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sn1/topology/dense-with-gap", test_dense_with_gap);
    g_test_add_func("/sn1/topology/contiguous", test_contiguous);
    g_test_add_func("/sn1/topology/one-cpu-non-default-slot",
                    test_one_cpu_non_default_slot);
    return g_test_run();
}
