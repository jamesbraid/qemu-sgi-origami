/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "hw/misc/sgi-odyssey-clock.h"

static void test_rate(void)
{
    /* 3 GHz over 100 ms, 1 s, and a gap past 2^32 ns (4.29 s). */
    g_assert_cmpuint(sgi_odyssey_ticks_per_ms(300000000, 100LL * SCALE_MS),
                     ==, 3000000);
    g_assert_cmpuint(sgi_odyssey_ticks_per_ms(3000000000LL, 1000LL * SCALE_MS),
                     ==, 3000000);
    g_assert_cmpuint(sgi_odyssey_ticks_per_ms(15000000000LL, 5000LL * SCALE_MS),
                     ==, 3000000);
    /* A division with a remainder, past 2^32 ns. */
    g_assert_cmpuint(sgi_odyssey_ticks_per_ms(15000000001LL,
                                              5000000007LL), ==, 2999999);
    /* The tick counter that is the clock itself, 1 tick per ns. */
    g_assert_cmpuint(sgi_odyssey_ticks_per_ms(100LL * SCALE_MS,
                                              100LL * SCALE_MS),
                     ==, 1000000);
}

static void test_rejected(void)
{
    g_assert_cmpuint(sgi_odyssey_ticks_per_ms(3000000, 1LL * SCALE_MS), ==, 0);
    g_assert_cmpuint(sgi_odyssey_ticks_per_ms(3000000000LL,
                                              20000LL * SCALE_MS), ==, 0);
    g_assert_cmpuint(sgi_odyssey_ticks_per_ms(0, 1000LL * SCALE_MS), ==, 0);
    g_assert_cmpuint(sgi_odyssey_ticks_per_ms(-5, 1000LL * SCALE_MS), ==, 0);
    /* 100 kHz and 1 THz counters. */
    g_assert_cmpuint(sgi_odyssey_ticks_per_ms(100000, 1000LL * SCALE_MS),
                     ==, 0);
    g_assert_cmpuint(sgi_odyssey_ticks_per_ms(1000000000000000LL,
                                              1000LL * SCALE_MS), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi/odyssey-clock/rate", test_rate);
    g_test_add_func("/sgi/odyssey-clock/rejected", test_rejected);
    return g_test_run();
}
