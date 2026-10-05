/*
 * QTest virtual memory access tests.
 *
 * Copyright (c) 2026 James Braid
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define TEST_PHYS_ADDR 0x2000
#define MIPS_KSEG0_BASE UINT64_C(0xffffffff80000000)

static void test_virtual_memory_access(void)
{
    static const uint8_t initial[] = { 0x12, 0x34, 0x56, 0x78 };
    static const uint8_t updated[] = { 0x9a, 0xbc, 0xde, 0xf0 };
    uint8_t actual[sizeof(initial)];
    QTestState *qts = qtest_init("-M malta -m 64M");

    qtest_memwrite(qts, TEST_PHYS_ADDR, initial, sizeof(initial));
    g_assert_true(qtest_memread_cpu_debug(qts, 0,
                                          MIPS_KSEG0_BASE + TEST_PHYS_ADDR,
                                          actual, sizeof(actual)));
    g_assert_cmpmem(actual, sizeof(actual), initial, sizeof(initial));

    g_assert_true(qtest_memwrite_cpu_debug(qts, 0,
                                           MIPS_KSEG0_BASE + TEST_PHYS_ADDR,
                                           updated, sizeof(updated)));
    qtest_memread(qts, TEST_PHYS_ADDR, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), updated, sizeof(updated));

    g_assert_true(qtest_memread_cpu_debug(qts, 0, 0, NULL, 0));
    g_assert_true(qtest_memwrite_cpu_debug(qts, 0, 0, NULL, 0));
    g_assert_false(qtest_memread_cpu_debug(qts, 1,
                                           MIPS_KSEG0_BASE + TEST_PHYS_ADDR,
                                           actual, sizeof(actual)));
    g_assert_false(qtest_memwrite_cpu_debug(qts, 1,
                                            MIPS_KSEG0_BASE + TEST_PHYS_ADDR,
                                            updated, sizeof(updated)));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("virtual-memory-access", test_virtual_memory_access);
    return g_test_run();
}
