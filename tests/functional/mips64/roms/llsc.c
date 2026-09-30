/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check load-linked/store-conditional results, alignment, and the effect
 * of ERET. See the R10000 User's Manual version 2.0 and MIPS64 instruction
 * set reference MD00087 revision 6.06.
 */

#include "rom.h"

const char rom_name[] = "llsc";

/*
 * The uncached and cached views of the compatibility segments differ in
 * one
 * address bit.
 */
#define UNCACHED_BIT 0x20000000ull
#define CACHED(p) ((uint64_t)(p) & ~UNCACHED_BIT)

/*
 * Doubleword aligned so the doubleword forms can use the same words.
 */
static volatile uint64_t sync_a __attribute__((aligned(8)));
/*
 * Volatile keeps shared-memory accesses emitted; sync instructions
 * order
 * them.
 */
static volatile uint64_t sync_b __attribute__((aligned(8)));

#define OLD_WORD 0x11112222u
#define NEW_WORD 0x33334444u
#define OLD_DOUBLE 0x1111222233334444ull
#define NEW_DOUBLE 0x5555666677778888ull

/*
 * A word read and a word write through an address the caller supplies,
 * so
 * that
 * every access to a synchronisation word goes through the cached view.
 */
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

/*
 * Return from exception, in the middle of straight-line code. The
 * exception
 * program counter is pointed at the instruction after the return, the
 * exception level bit is raised so that the return has an exception to
 * return
 * from, and the return itself clears that bit again. The two kernel
 * scratch
 * registers are the only ones disturbed.
 */
#define ERET_HERE                                                             \
    "lui $26, %%hi(8f)\n\t"                                                   \
    "daddiu $26, $26, %%lo(8f)\n\t"                                           \
    "dmtc0 $26, $14\n\t"                                                      \
    "mfc0 $27, $12\n\t"                                                       \
    "ori $26, $27, 2\n\t"                                                     \
    "mtc0 $26, $12\n\t"                                                       \
    "eret\n\t"

void rom_main(void)
{
    uint64_t a = CACHED(&sync_a);
    uint64_t b = CACHED(&sync_b);
    uint64_t got, res, marker;
    uint32_t before;

    /*
     * Before relying on that return-from-exception sequence, check it
     * behaves:
     * it must not execute the instruction that follows it, and it must
     * land
     * on
     * the instruction whose address was placed in the exception program
     * counter. Bit 0 of the marker is set by the instruction after the
     * return
     * and bit 1 by the landing point, so a working sequence yields 2.
     */
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         "move %[m], $0\n\t"
                         ERET_HERE
                         "ori %[m], %[m], 1\n"
                         "8:\tori %[m], %[m], 2\n\t"
                         ".set pop"
                         : [m] "=&r"(marker)
                         :
                         : "$26", "$27");
    rom_assert_eq(1, 2, marker);

    /*
     * A sequence with nothing in between succeeds and writes the word.
     */
    poke_word(a, OLD_WORD);
    res = NEW_WORD;
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         "ll %[g], 0(%[p])\n\t"
                         "sc %[r], 0(%[p])\n\t"
                         ".set pop"
                         : [g] "=&r"(got), [r] "+&r"(res)
                         : [p] "r"(a)
                         : "memory");
    rom_assert_eq(2, (uint64_t)(int32_t)OLD_WORD, got);
    rom_assert_eq(3, 1, res);
    rom_assert_eq(4, NEW_WORD, peek_word(a));

    /*
     * The doubleword forms behave the same way.
     */
    poke_double(a, OLD_DOUBLE);
    res = NEW_DOUBLE;
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         "lld %[g], 0(%[p])\n\t"
                         "scd %[r], 0(%[p])\n\t"
                         ".set pop"
                         : [g] "=&r"(got), [r] "+&r"(res)
                         : [p] "r"(a)
                         : "memory");
    rom_assert_eq(5, OLD_DOUBLE, got);
    rom_assert_eq(6, 1, res);
    rom_assert_eq(7, NEW_DOUBLE, peek_double(a));

    /*
     * A return from exception in the middle makes the store-conditional
     * fail
     * and leaves the word alone.
     */
    poke_word(a, OLD_WORD);
    res = NEW_WORD;
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         "ll %[g], 0(%[p])\n\t"
                         ERET_HERE
                         "nop\n"
                         "8:\tsc %[r], 0(%[p])\n\t"
                         ".set pop"
                         : [g] "=&r"(got), [r] "+&r"(res)
                         : [p] "r"(a)
                         : "$26", "$27", "memory");
    rom_assert_eq(8, (uint64_t)(int32_t)OLD_WORD, got);
    rom_assert_eq(9, 0, res);
    rom_assert_eq(10, OLD_WORD, peek_word(a));

    /*
     * The doubleword form fails on a return from exception too.
     */
    poke_double(a, OLD_DOUBLE);
    res = NEW_DOUBLE;
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         "lld %[g], 0(%[p])\n\t"
                         ERET_HERE
                         "nop\n"
                         "8:\tscd %[r], 0(%[p])\n\t"
                         ".set pop"
                         : [g] "=&r"(got), [r] "+&r"(res)
                         : [p] "r"(a)
                         : "$26", "$27", "memory");
    rom_assert_eq(11, OLD_DOUBLE, got);
    rom_assert_eq(12, 0, res);
    rom_assert_eq(13, OLD_DOUBLE, peek_double(a));

    /*
     * A store-conditional whose address differs from the load-linked
     * that
     * started the sequence must fail, and must not write either word.
     * This is
     * the one rule the instruction set reference states without an
     * implementation-dependent escape.
     */
    poke_word(a, OLD_WORD);
    poke_word(b, OLD_WORD);
    res = NEW_WORD;
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         "ll %[g], 0(%[pa])\n\t"
                         "sc %[r], 0(%[pb])\n\t"
                         ".set pop"
                         : [g] "=&r"(got), [r] "+&r"(res)
                         : [pa] "r"(a), [pb] "r"(b)
                         : "memory");
    rom_assert_eq(14, 0, res);
    rom_assert_eq(15, OLD_WORD, peek_word(b));
    rom_assert_eq(16, OLD_WORD, peek_word(a));

    /*
     * A plain store to the same word between the two instructions.
     * Whether it
     * breaks the sequence is not predictable on these processors, so
     * the only
     * claim made is that the outcome is consistent: a reported success
     * wrote
     * the new value, a reported failure left the value the plain store
     * put
     * there.
     */
    poke_word(a, OLD_WORD);
    res = NEW_WORD;
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         "ll %[g], 0(%[p])\n\t"
                         "sw %[q], 0(%[p])\n\t"
                         "sc %[r], 0(%[p])\n\t"
                         ".set pop"
                         : [g] "=&r"(got), [r] "+&r"(res)
                         : [p] "r"(a), [q] "r"((uint64_t)(int32_t)0x0badc0deu)
                         : "memory");
    before = peek_word(a);
    rom_assert(17, (res == 1 && before == NEW_WORD) ||
                   (res == 0 && before == 0x0badc0deu));

    /*
     * A store-conditional with no sequence in progress, the previous
     * one
     * having
     * been completed already. Nothing is predictable here either, so
     * again
     * only
     * the all-or-nothing property is checked.
     */
    poke_word(b, OLD_WORD);
    res = NEW_WORD;
    __asm__ __volatile__("sc %[r], 0(%[p])"
                         : [r] "+&r"(res)
                         : [p] "r"(b)
                         : "memory");
    before = peek_word(b);
    rom_assert(18, (res == 1 && before == NEW_WORD) ||
                   (res == 0 && before == OLD_WORD));
}
