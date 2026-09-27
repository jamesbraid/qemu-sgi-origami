/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * R14000 exception and address behavior selected from the R12000A
 * contract.
 *
 * Oracle: docs/datasheets/r14000.md at Origami commit 14dbec407f.  ERL
 * limits
 * xkuseg to its low 2 GiB, exception entry breaks LL/SC, and uncached
 * stores
 * are posted while a load from the same unimplemented address raises
 * DBE.
 * Warm-reset and Cache Error entry require an external injection path
 * and are
 * covered separately once such a path is available.
 */

#include "rom.h"

const char rom_name[] = "r14000_exceptions";

#define UNCACHED_BIT 0x20000000ull
#define CACHED(p) ((uint64_t)(p) & ~UNCACHED_BIT)
#define XKUSEG_FIRST_HIGH 0x0000000080000000ull
#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))
#define HOLE XKPHYS(ROM_CACHE_UNCACHED, 1,                                    \
                    0x100000000ull + 0x01800000ull + 0x400150ull)

/*
 * Volatile keeps shared-memory accesses emitted; sync instructions
 * order
 * them.
 */
static volatile uint32_t sync_word __attribute__((aligned(8)));

static void probe_load(uint64_t addr, uint64_t status_set)
{
    uint64_t dead;

    rom_exc_reset();
    rom_exc.status_set = status_set;
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         "lui $26, %%hi(9f)\n\t"
                         "daddiu $26, $26, %%lo(9f)\n\t"
                         "sd $26, 0(%[res])\n\t"
                         "ld %[d], 0(%[a])\n"
                         "9:\t.set pop"
                         : [d] "=&r"(dead)
                         : [res] "r"(&rom_exc.resume), [a] "r"(addr)
                         : "$26", "memory");
    (void)dead;
}

#define TRY_BLOCK(insns, ...)                                                 \
    ROM_TRY(".set noreorder\n\t" insns, ##__VA_ARGS__)

void rom_main(void)
{
    uint64_t addr = CACHED(&sync_word);
    uint64_t loaded;
    uint32_t result;
    uint32_t status = rom_get_status();

    /*
     * ERL rejects the first address above the low 2 GiB xkuseg window.
     */
    rom_set_status(status | ROM_ST_ERL | ROM_ST_UX);
    probe_load(XKUSEG_FIRST_HIGH, status);
    rom_assert_eq(1, 1, rom_exc.count);
    rom_assert_eq(2, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_set_status(status | ROM_ST_KX);

    /*
     * A breakpoint between LL and SC clears the processor's link.
     */
    sync_word = 0x11223344u;
    result = 0xaabbccddu;
    rom_exc_reset();
    rom_exc.jump = 1;
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         "lui $26, %%hi(8f)\n\t"
                         "daddiu $26, $26, %%lo(8f)\n\t"
                         "sd $26, 0(%[resume])\n\t"
                         "li $26, 1\n\t"
                         "sd $26, 0(%[jump])\n\t"
                         "ll %[got], 0(%[addr])\n\t"
                         "break\n"
                         "8:\tsc %[result], 0(%[addr])\n\t"
                         ".set pop"
                         : [got] "=&r"(loaded), [result] "+&r"(result)
                         : [addr] "r"(addr), [resume] "r"(&rom_exc.resume),
                           [jump] "r"(&rom_exc.jump)
                         : "$26", "$27", "memory");
    rom_assert_eq(3, 1, rom_exc.count);
    rom_assert_eq(4, ROM_EXC_BP, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(5, 0, result);
    rom_assert_eq(6, 0x11223344u, sync_word);

    /*
     * The unimplemented node accepts a posted store but rejects a load.
     */
    rom_exc_reset();
    TRY_BLOCK("sd $0, 0(%[p])", [p] "r"((unsigned long)HOLE));
    rom_assert_eq(7, 0, rom_exc.count);
    rom_exc_reset();
    TRY_BLOCK("ld $0, 0(%[p])", [p] "r"((unsigned long)HOLE));
    rom_assert_eq(8, 1, rom_exc.count);
    rom_assert_eq(9, ROM_EXC_DBE, ROM_CAUSE_EXC(rom_exc.cause));
}
