/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * QTest: a system reset clears the RC4030 DMA translation table registers.
 *
 * The Jazz PROM reads them early and takes a non-zero value for a failed
 * start, so a stale one left by the previous run hangs the machine in its
 * LED loop.
 */
#include "qemu/osdep.h"
#include "libqtest.h"

#define RC4030_BASE      0x80000000
#define DMA_TL_BASE      (RC4030_BASE + 0x18)
#define DMA_TL_LIMIT     (RC4030_BASE + 0x20)

static void test_reset_clears_dma_translation_table(void)
{
    QTestState *qts = qtest_init("-M magnum");

    qtest_writel(qts, DMA_TL_BASE, 0x12345000);
    qtest_writel(qts, DMA_TL_LIMIT, 0x2000);
    g_assert_cmphex(qtest_readl(qts, DMA_TL_BASE), ==, 0x12345000);
    g_assert_cmphex(qtest_readl(qts, DMA_TL_LIMIT), ==, 0x2000);

    qtest_system_reset(qts);

    g_assert_cmphex(qtest_readl(qts, DMA_TL_BASE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, DMA_TL_LIMIT), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rc4030/reset", test_reset_clears_dma_translation_table);
    return g_test_run();
}
