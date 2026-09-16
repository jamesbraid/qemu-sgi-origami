/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/mips/sgi/sn1-topology.h"

/* The shipped two-CPU Origin 300 model: slices A and C, PI1's B unfitted. */
static void test_explicit_mask_dense_with_gap(void)
{
    const SN1MachineModel model = {
        .cpu_count = 2, .cpu_population_mask = 0x5,
    };

    g_assert_cmpuint(sn1_cpu_slot(&model, 0), ==, 0);
    g_assert_cmpuint(sn1_cpu_slot(&model, 1), ==, 2);
}

/* An explicit mask naming two physically adjacent slots, A and B. */
static void test_explicit_mask_contiguous(void)
{
    const SN1MachineModel model = {
        .cpu_count = 2, .cpu_population_mask = 0x3,
    };

    g_assert_cmpuint(sn1_cpu_slot(&model, 0), ==, 0);
    g_assert_cmpuint(sn1_cpu_slot(&model, 1), ==, 1);
}

/*
 * A model that leaves cpu_population_mask at its zero default -- either a
 * one-CPU model that never had a reason to set it, or a model written
 * before the mask field existed. Both must resolve through the legacy
 * contiguous mask implied by cpu_count, not abort construction.
 */
static void test_legacy_default_one_cpu(void)
{
    const SN1MachineModel model = { .cpu_count = 1 };

    g_assert_cmpuint(sn1_cpu_slot(&model, 0), ==, 0);
}

static void test_legacy_default_two_cpu(void)
{
    const SN1MachineModel model = { .cpu_count = 2 };

    g_assert_cmpuint(sn1_cpu_slot(&model, 0), ==, 0);
    g_assert_cmpuint(sn1_cpu_slot(&model, 1), ==, 1);
}

/* A one-CPU model whose single populated slot is not the first one. */
static void test_explicit_mask_one_cpu_non_default_slot(void)
{
    const SN1MachineModel model = {
        .cpu_count = 1, .cpu_population_mask = 0x4,
    };

    g_assert_cmpuint(sn1_cpu_slot(&model, 0), ==, 2);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sn1/topology/explicit-mask/dense-with-gap",
                    test_explicit_mask_dense_with_gap);
    g_test_add_func("/sn1/topology/explicit-mask/contiguous",
                    test_explicit_mask_contiguous);
    g_test_add_func("/sn1/topology/legacy-default/one-cpu",
                    test_legacy_default_one_cpu);
    g_test_add_func("/sn1/topology/legacy-default/two-cpu",
                    test_legacy_default_two_cpu);
    g_test_add_func("/sn1/topology/explicit-mask/one-cpu-non-default-slot",
                    test_explicit_mask_one_cpu_non_default_slot);
    return g_test_run();
}
