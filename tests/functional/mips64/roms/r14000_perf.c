/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check the four performance-counter pairs and selected cycle-counting
 * behavior. The NEC VR10000 Series User's Manual, 4th edition
 * (U10278EJ4V0UM, March 2001), documents the counter fields and event
 * numbers used below. Zeroed reset values are an emulator compatibility
 * choice. Interrupt behavior is not tested.
 */

#include "rom.h"

const char rom_name[] = "r14000_perf";

#define PC_U   (1u << 3)
#define PC_IE  (1u << 4)
#define PC_K   (1u << 1)
#define PC_EXL (1u << 0)
#define PC_EVENT(n) ((uint32_t)(n) << 5)
/* Event numbers from the NEC manual, Table 11-24. */
#define EV_SCACHE_ECC_CORRECTED 8
#define EV_PREFETCH_EXECUTED    16
#define POLL_LIMIT 50000000

#define ROM_PERF(name, sel)                                                   \
    static inline uint64_t rom_get_##name(void)                               \
    {                                                                         \
        uint64_t v;                                                           \
        __asm__ __volatile__(".set push\n\t.set mips32\n\t"                 \
                             "mfc0 %0, $25, " #sel "\n\t"                   \
                             ".set pop" : "=r"(v));                          \
        return v;                                                             \
    }                                                                         \
    static inline void rom_set_##name(uint64_t v)                             \
    {                                                                         \
        __asm__ __volatile__(".set push\n\t.set mips32\n\t"                 \
                             "mtc0 %0, $25, " #sel "\n\t"                   \
                             ".set pop" : : "r"(v));                         \
    }

ROM_PERF(ctl0, 0)
ROM_PERF(cnt0, 1)
ROM_PERF(ctl1, 2)
ROM_PERF(cnt1, 3)
ROM_PERF(ctl2, 4)
ROM_PERF(cnt2, 5)
ROM_PERF(ctl3, 6)
ROM_PERF(cnt3, 7)

static void wait_for_movement(uint64_t (*read_counter)(void), int id)
{
    uint32_t first = (uint32_t)read_counter();
    uint32_t next = first;
    int i;

    for (i = 0; i < POLL_LIMIT && next == first; i++) {
        next = (uint32_t)read_counter();
    }
    rom_assert(id, next != first);
}

void rom_main(void)
{
    uint64_t high = 0x1234567887654321ull;
    uint32_t held;

    rom_exc_reset();

    rom_assert_eq(1, 0, rom_get_ctl0());
    rom_assert_eq(2, 0, rom_get_cnt0());
    rom_assert_eq(3, 0, rom_get_ctl1());
    rom_assert_eq(4, 0, rom_get_cnt1());
    rom_assert_eq(5, 0, rom_get_ctl2());
    rom_assert_eq(6, 0, rom_get_cnt2());
    rom_assert_eq(7, 0, rom_get_ctl3());
    rom_assert_eq(8, 0, rom_get_cnt3());

    rom_set_ctl0(0);
    rom_set_ctl1(0);
    rom_set_ctl2(0);
    rom_set_ctl3(0);
    rom_set_cnt0(0x11111111u);
    rom_set_cnt1(0x22222222u);
    rom_set_cnt2(high);
    rom_set_cnt3(0x80000003u);

    rom_assert_eq(9, 0x0000000011111111ull, rom_get_cnt0());
    rom_assert_eq(10, 0x0000000022222222ull, rom_get_cnt1());
    rom_assert_eq(11, 0xffffffff87654321ull, rom_get_cnt2());
    rom_assert_eq(12, 0xffffffff80000003ull, rom_get_cnt3());

    /*
     * Each control is independent and only cleared qualifiers are used.
     */
    rom_set_ctl0(PC_K);
    rom_set_ctl1(PC_U);
    rom_set_ctl2(PC_EXL);
    rom_set_ctl3(PC_IE | PC_K | PC_U);
    rom_assert_eq(13, PC_K, rom_get_ctl0());
    rom_assert_eq(14, PC_U, rom_get_ctl1());
    rom_assert_eq(15, PC_EXL, rom_get_ctl2());
    rom_assert_eq(16, PC_IE | PC_K | PC_U, rom_get_ctl3());

    rom_set_cnt0(0);
    rom_set_cnt3(0);
    wait_for_movement(rom_get_cnt0, 17);
    wait_for_movement(rom_get_cnt3, 18);

    /*
     * User-only and EXL-only counters are stable in ordinary kernel
     * mode.
     */
    rom_set_cnt1(0x13572468u);
    rom_set_cnt2(0x24681357u);
    held = (uint32_t)rom_get_cnt0();
    while ((uint32_t)rom_get_cnt0() - held < 10000u) {
    }
    rom_assert_eq(19, 0x13572468u, (uint32_t)rom_get_cnt1());
    rom_assert_eq(20, 0x24681357u, (uint32_t)rom_get_cnt2());

    /*
     * Event select occupies bits 9:5 (NEC manual, Figure 11-23 and
     * Table 11-27). This ROM does not generate events 8 or 16, so their
     * counters must retain their preloads while pair 0 counts cycles.
     * Event 16 also checks that bit 9 is retained.
     */
    rom_set_ctl3(0);
    rom_set_ctl2(0);
    rom_set_ctl3(PC_EVENT(EV_SCACHE_ECC_CORRECTED) | PC_IE | PC_K | PC_U);
    rom_set_ctl2(PC_EVENT(EV_PREFETCH_EXECUTED) | PC_K);
    rom_set_cnt3(0x7fffffffu);
    rom_set_cnt2(0x00000100u);
    held = (uint32_t)rom_get_cnt0();
    while ((uint32_t)rom_get_cnt0() - held < 10000u) {
    }
    rom_assert_eq(21, 0x000000007fffffffull, rom_get_cnt3());
    rom_assert_eq(22, 0x0000000000000100ull, rom_get_cnt2());
    rom_assert_eq(23, PC_EVENT(EV_SCACHE_ECC_CORRECTED) | PC_IE | PC_K | PC_U,
                  rom_get_ctl3());
    rom_assert_eq(24, PC_EVENT(EV_PREFETCH_EXECUTED) | PC_K, rom_get_ctl2());

    rom_set_ctl0(0);
    rom_set_ctl2(0);
    rom_set_ctl3(0);
    rom_assert_eq(25, 0, rom_exc.count);
}
