/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check that absent-node reads raise bus errors while posted writes complete.
 */

#include "rom.h"

const char rom_name[] = "buserror";

#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))

#define ATTR_XTALK      1               /* the attribute that means crosstalk */
#define HUB_NODE        0x01800000ull   /* "the Hub of the node I name" */
#define NODE(n)         ((uint64_t)(n) << 32)

/* Valid scratch-register offset; only node 1 is absent on supported setup. */
#define IO_SCRATCH      0x400150ull

/* Address of node 1's Hub scratch register. */
#define HOLE \
    XKPHYS(ROM_CACHE_UNCACHED, ATTR_XTALK, \
           NODE(1) + HUB_NODE + IO_SCRATCH)

#define VEC_BOOTSTRAP_GENERAL (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)

/* Record the block's start and resume after it, regardless of faulting op. */
#define TRY_AT(first, insns, ...)                                             \
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                    \
                         "lui $26, %%hi(9f)\n\t"                              \
                         "daddiu $26, $26, %%lo(9f)\n\t"                      \
                         "sd $26, 0(%[res])\n\t"                              \
                         "lui %[at], %%hi(8f)\n\t"                            \
                         "daddiu %[at], %[at], %%lo(8f)\n"                    \
                         "8:\t" insns "\n"                                    \
                         "9:\t.set pop"                                       \
                         : [at] "=&r"(first), ##__VA_ARGS__                   \
                         : [res] "r"(&rom_exc.resume),                        \
                           [p] "r"((unsigned long)HOLE)                       \
                         : "$26", "memory")

void rom_main(void)
{
    uint64_t first, junk;

    /* Reach the node-qualified address through the 64-bit unmapped segment. */
    rom_set_status(rom_get_status() | ROM_ST_KX);

    /* Posted doubleword write to the absent node completes without fault. */
    rom_exc_reset();
    TRY_AT(first, "sd $0, 0(%[p])");
    rom_assert_eq(1, 0, rom_exc.count);

    /* Partial-word writes are posted too. */
    rom_exc_reset();
    TRY_AT(first, "sw $0, 0(%[p])");
    rom_assert_eq(2, 0, rom_exc.count);

    /*
     * BadVAddr is unspecified for bus errors; check vector, code, EPC and
     * BD so the write behavior cannot mask a broken read fault.
     */
    rom_exc_reset();
    TRY_AT(first, "ld %[j], 0(%[p])", [j] "=&r"(junk));
    rom_assert_eq(3, 1, rom_exc.count);
    rom_assert_eq(4, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(5, ROM_EXC_DBE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(6, 0, rom_exc.cause & ROM_CAUSE_BD);
    rom_assert_eq(7, first, rom_exc.epc);

    /* Check the word-load fault separately from the doubleword load. */
    rom_exc_reset();
    TRY_AT(first, "lw %[j], 0(%[p])", [j] "=&r"(junk));
    rom_assert_eq(8, 1, rom_exc.count);
    rom_assert_eq(9, ROM_EXC_DBE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(10, first, rom_exc.epc);
}
