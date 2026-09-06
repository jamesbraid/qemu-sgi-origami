/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise CPU-local physical access using the two independent Hub adapters. */

#include "qemu/osdep.h"
#include "libqtest.h"

#define DATA_BASE 0x20001220000ULL
#define MACHINE_DUAL \
    "-M origin200 -m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"

static void write_register(QTestState *qts, unsigned int cpu, uint64_t addr,
                           uint8_t value)
{
    uint8_t bytes[8] = { 0 };

    bytes[7] = value;
    g_assert_true(qtest_memwrite_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
}

static uint8_t read_register(QTestState *qts, unsigned int cpu, uint64_t addr)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
    for (unsigned int i = 0; i < 7; i++) {
        g_assert_cmphex(bytes[i], ==, 0);
    }
    return bytes[7];
}

static void test_cpu_local_registers(void)
{
    QTestState *qts = qtest_init(MACHINE_DUAL);
    const uint8_t sentinel[8] = { 0xaa, 1, 2, 3, 4, 5, 6, 7 };
    uint8_t invalid[8];
    uint8_t expected[8] = { 0 };
    uint8_t actual[8];

    /* Disable serial operation before selecting each adapter's own address. */
    for (unsigned int node = 0; node < 2; node++) {
        write_register(qts, node * 2, DATA_BASE + 8, 0x80);
        write_register(qts, node * 2, DATA_BASE, 0x10 + node);
    }
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        g_assert_cmphex(read_register(qts, cpu, DATA_BASE), ==, 0x10 + cpu / 2);
    }
    expected[7] = 0x10;
    qtest_memread(qts, DATA_BASE, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    expected[7] = 0x20;
    qtest_memwrite(qts, DATA_BASE, expected, sizeof(expected));
    g_assert_cmphex(read_register(qts, 0, DATA_BASE), ==, 0x20);
    g_assert_cmphex(read_register(qts, 1, DATA_BASE), ==, 0x20);
    g_assert_cmphex(read_register(qts, 2, DATA_BASE), ==, 0x11);

    memcpy(invalid, sentinel, sizeof(invalid));
    g_assert_false(qtest_memwrite_cpu(qts, 4, DATA_BASE,
                                     invalid, sizeof(invalid)));
    g_assert_false(qtest_memread_cpu(qts, UINT_MAX, DATA_BASE,
                                    invalid, sizeof(invalid)));
    g_assert_cmpmem(invalid, sizeof(invalid), sentinel, sizeof(sentinel));
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        g_assert_cmphex(read_register(qts, cpu, DATA_BASE),
                        ==, cpu < 2 ? 0x20 : 0x11);
    }
    g_assert_true(qtest_memread_cpu(qts, UINT_MAX, DATA_BASE, invalid, 0));
    g_assert_true(qtest_memwrite_cpu(qts, UINT_MAX, DATA_BASE, invalid, 0));

    qtest_system_reset(qts);
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        g_assert_cmphex(read_register(qts, cpu, DATA_BASE), ==, 0);
        g_assert_cmphex(read_register(qts, cpu, DATA_BASE + 8), ==, 0x80);
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/qtest/cpu-local-registers", test_cpu_local_registers);
    return g_test_run();
}
