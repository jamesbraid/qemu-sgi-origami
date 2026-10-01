/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * A breakpoint between R10000 LL/SC must invalidate the reservation
 * (User's Manual 2.0).
 */

#include "rom.h"

const char rom_name[] = "llscexc";

/* Cached and uncached compatibility-segment addresses differ at this bit. */
#define UNCACHED_BIT 0x20000000ull
#define CACHED(p) ((uint64_t)(p) & ~UNCACHED_BIT)

/* Shared by word and doubleword forms. */
static volatile uint64_t sync_word __attribute__((aligned(8)));

#define OLD_WORD 0x11112222u
#define NEW_WORD 0x33334444u
#define OLD_DOUBLE 0x1111222233334444ull
#define NEW_DOUBLE 0x5555666677778888ull

static uint32_t peek_word(uint64_t addr)
{
    uint32_t v;

    __asm__ __volatile__("lw %0, 0(%1)" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static void poke_word(uint64_t addr, uint32_t v)
{
    __asm__ __volatile__("sw %1, 0(%0)" : : "r"(addr), "r"(v) : "memory");
}

static uint64_t peek_double(uint64_t addr)
{
    uint64_t v;

    __asm__ __volatile__("ld %0, 0(%1)" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static void poke_double(uint64_t addr, uint64_t v)
{
    __asm__ __volatile__("sd %1, 0(%0)" : : "r"(addr), "r"(v) : "memory");
}

/* Arm handler state before LL so only the breakpoint interrupts the pair. */
#define LL_BREAK_SC(ll, sc)                                                   \
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                    \
                         "lui $26, %%hi(8f)\n\t"                              \
                         "daddiu $26, $26, %%lo(8f)\n\t"                      \
                         "sd $26, 0(%[resume])\n\t"                           \
                         "li $26, 1\n\t"                                      \
                         "sd $26, 0(%[jump])\n\t"                             \
                         ll " %[g], 0(%[p])\n\t"                              \
                         "break\n"                                            \
                         "8:\t" sc " %[r], 0(%[p])\n\t"                       \
                         ".set pop"                                           \
                         : [g] "=&r"(got), [r] "+&r"(res)                     \
                         : [p] "r"(a), [resume] "r"(&rom_exc.resume),         \
                           [jump] "r"(&rom_exc.jump)                          \
                         : "$26", "$27", "memory")

void rom_main(void)
{
    uint64_t a = CACHED(&sync_word);
    uint64_t got, res;

    /* Verify the pair succeeds without an exception. */
    poke_word(a, OLD_WORD);
    res = NEW_WORD;
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         "ll %[g], 0(%[p])\n\t"
                         "sc %[r], 0(%[p])\n\t"
                         ".set pop"
                         : [g] "=&r"(got), [r] "+&r"(res)
                         : [p] "r"(a)
                         : "memory");
    rom_assert_eq(1, 1, res);
    rom_assert_eq(2, NEW_WORD, peek_word(a));

    /* The handler accesses memory too; this checks the full exception path. */
    rom_exc_reset();
    poke_word(a, OLD_WORD);
    res = NEW_WORD;
    LL_BREAK_SC("ll", "sc");
    rom_assert_eq(3, (uint64_t)(int32_t)OLD_WORD, got);
    rom_assert_eq(4, 1, rom_exc.count);
    rom_assert_eq(5, ROM_EXC_BP, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(6, 0, rom_exc.jump);
    rom_assert_eq(7, 0, rom_get_status() & ROM_ST_EXL);
    rom_assert_eq(8, 0, res);
    rom_assert_eq(9, OLD_WORD, peek_word(a));

    /* The same exception must invalidate LLD/SCD. */
    rom_exc_reset();
    poke_double(a, OLD_DOUBLE);
    res = NEW_DOUBLE;
    LL_BREAK_SC("lld", "scd");
    rom_assert_eq(10, OLD_DOUBLE, got);
    rom_assert_eq(11, 1, rom_exc.count);
    rom_assert_eq(12, 0, res);
    rom_assert_eq(13, OLD_DOUBLE, peek_double(a));
}
