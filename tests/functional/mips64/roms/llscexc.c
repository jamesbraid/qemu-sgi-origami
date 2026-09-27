/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Taking an exception breaks a load-linked sequence on the R10000.
 *
 * Oracle: the R10000 User's Manual version 2.0, which states the rule
 * twice
 * and without an escape clause both times. The LL/SC section of chapter
 * 1
 * lists the events the sequence "must not contain ... or the Store
 * Conditional will fail", beginning with "exception", and the
 * implementation notes at the end of the same chapter repeat that any
 * of
 * those events "will cause the Store Conditional to fail". The generic
 * instruction set reference leaves this case predictable only on
 * processors
 * that advertise the later link-bit behaviour in their fifth
 * configuration
 * register, which the R4000 does not have, so this ROM runs against the
 * R10000 alone and the shared llsc ROM stays silent about it.
 *
 * The shared ROM pins the return-from-exception rule, but an ERET
 * breaks
 * the sequence by itself, so a test that faults and comes back through
 * the
 * handler's ordinary exit cannot say anything about the entry. Here the
 * handler is asked to come back by a plain jump instead — the harness
 * grew
 * that return for this test — so no ERET runs between the load-linked
 * and
 * the store-conditional and what remains is the exception itself. A
 * breakpoint instruction provides one deliberately.
 *
 * One honest limit: the manual also names same-processor loads and
 * stores
 * between the pair as link breakers, and the handler does both to
 * record
 * the exception, so this ROM cannot separate "the entry broke the link"
 * from "the handler's bookkeeping broke it" — on the real processor
 * every
 * event in that window kills the sequence, which is exactly why the
 * assertion holds there too. What it pins is the observable contract: a
 * sequence a fault landed in must not succeed.
 *
 * The synchronisation word is reached through the cached view of
 * memory,
 * as in the shared llsc ROM: atomic sequences are restricted to cached
 * locations, so the harness data window's uncached bit is masked off.
 */

#include "rom.h"

const char rom_name[] = "llscexc";

/*
 * The uncached and cached views of the compatibility segments differ in
 * one
 * address bit.
 */
#define UNCACHED_BIT 0x20000000ull
#define CACHED(p) ((uint64_t)(p) & ~UNCACHED_BIT)

/*
 * Doubleword aligned so the doubleword forms can use the same word.
 */
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

/*
 * A load-linked/store-conditional pair with a breakpoint in the middle.
 * The resume address and the jump-return flag are armed before the
 * load-linked so that nothing but the fault itself sits inside the
 * sequence; the handler consumes both and jumps to the
 * store-conditional
 * with the exception level cleared by hand rather than by ERET.
 */
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

    /*
     * An undisturbed pair succeeds, so a failure below is the
     * exception's
     * doing and not a sequence that could never complete.
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
    rom_assert_eq(1, 1, res);
    rom_assert_eq(2, NEW_WORD, peek_word(a));

    /*
     * The same pair with a fault inside it must fail and leave the word
     * alone.
     */
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

    /*
     * The doubleword forms follow the same rule.
     */
    rom_exc_reset();
    poke_double(a, OLD_DOUBLE);
    res = NEW_DOUBLE;
    LL_BREAK_SC("lld", "scd");
    rom_assert_eq(10, OLD_DOUBLE, got);
    rom_assert_eq(11, 1, rom_exc.count);
    rom_assert_eq(12, 0, res);
    rom_assert_eq(13, OLD_DOUBLE, peek_double(a));
}
