/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test WatchLo and WatchHi address matching, Watch exceptions, and
 * delayed exceptions while EXL or ERL is set.
 *
 * Source: R10000 User's Manual, version 2.0, sections 14.16 and 17,
 * and tables 14-13 and 14-16.
 */

#include "rom.h"

const char rom_name[] = "watch";

/* Reserved physical pages, above the harness data and stack. */
#define WATCH_PA  0x00340000ull         /* the watched doubleword */
#define OTHER_PA  0x00340008ull         /* the doubleword next to it */

/*
 * A virtual page in the user segment, mapped onto the watched page to
 * show
 * the comparison is physical.
 */
#define VIRT_WIN  0x10000000ull

#define VEC_BOOTSTRAP_GENERAL (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)

#define MAP_RW (ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G)

/*
 * WatchLo enable bits, from Figure 14-18.
 */
#define WATCHLO_R (1u << 1)
#define WATCHLO_W (1u << 0)

/*
 * The uncached window onto a physical address.
 */
static volatile uint64_t *phys64(uint64_t paddr)
{
    /*
     * Volatile preserves accesses through the uncached physical-memory
     * mapping.
     */
    return (volatile uint64_t *)(unsigned long)(0xffffffffa0000000ull + paddr);
}

/*
 * Volatile preserves accesses through the uncached physical-memory
 * mapping.
 */
static volatile uint32_t *phys32(uint64_t paddr)
{
    /*
     * Volatile preserves accesses through the uncached physical-memory
     * mapping.
     */
    return (volatile uint32_t *)(unsigned long)(0xffffffffa0000000ull + paddr);
}

/* Return the faulting instruction address for EPC assertions. */
#define TRY_SD(vaddr, val, insn_addr)                                         \
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                    \
                         "lui $26, %%hi(9f)\n\t"                              \
                         "daddiu $26, $26, %%lo(9f)\n\t"                      \
                         "sd $26, 0(%[res])\n\t"                              \
                         "lui %[ia], %%hi(1f)\n\t"                            \
                         "daddiu %[ia], %[ia], %%lo(1f)\n"                    \
                         "1:\tsd %[v], 0(%[a])\n"                             \
                         "9:\t.set pop"                                       \
                         : [ia] "=&r"(insn_addr)                              \
                         : [res] "r"(&rom_exc.resume), [a] "r"(vaddr),        \
                           [v] "r"(val)                                       \
                         : "$26", "memory")

#define TRY_LD(vaddr, dest, insn_addr)                                        \
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                    \
                         "lui $26, %%hi(9f)\n\t"                              \
                         "daddiu $26, $26, %%lo(9f)\n\t"                      \
                         "sd $26, 0(%[res])\n\t"                              \
                         "lui %[ia], %%hi(1f)\n\t"                            \
                         "daddiu %[ia], %[ia], %%lo(1f)\n"                    \
                         "1:\tld %[d], 0(%[a])\n"                             \
                         "9:\t.set pop"                                       \
                         : [ia] "=&r"(insn_addr), [d] "=&r"(dest)             \
                         : [res] "r"(&rom_exc.resume), [a] "r"(vaddr)         \
                         : "$26", "memory")

#define TRY_SW(vaddr, val, insn_addr)                                         \
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                    \
                         "lui $26, %%hi(9f)\n\t"                              \
                         "daddiu $26, $26, %%lo(9f)\n\t"                      \
                         "sd $26, 0(%[res])\n\t"                              \
                         "lui %[ia], %%hi(1f)\n\t"                            \
                         "daddiu %[ia], %[ia], %%lo(1f)\n"                    \
                         "1:\tsw %[v], 0(%[a])\n"                             \
                         "9:\t.set pop"                                       \
                         : [ia] "=&r"(insn_addr)                              \
                         : [res] "r"(&rom_exc.resume), [a] "r"(vaddr),        \
                           [v] "r"(val)                                       \
                         : "$26", "memory")

/* Delay Watch until EXL clears; EPC identifies the next instruction. */
#define DEFER_BLOCK(vaddr, val, st_exl, st_base, after_addr)                  \
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                    \
                         "lui $26, %%hi(9f)\n\t"                              \
                         "daddiu $26, $26, %%lo(9f)\n\t"                      \
                         "sd $26, 0(%[res])\n\t"                              \
                         "lui %[aa], %%hi(1f)\n\t"                            \
                         "daddiu %[aa], %[aa], %%lo(1f)\n\t"                  \
                         "mtc0 %[se], $12\n\t"                                \
                         "sd %[v], 0(%[a])\n\t"                               \
                         "mtc0 %[sb], $12\n"                                  \
                         "1:\tnop\n"                                          \
                         "9:\t.set pop"                                       \
                         : [aa] "=&r"(after_addr)                             \
                         : [res] "r"(&rom_exc.resume), [a] "r"(vaddr),        \
                           [v] "r"(val), [se] "r"(st_exl), [sb] "r"(st_base)  \
                         : "$26", "memory")

#define DEFER_CLEAR_BLOCK(vaddr, val, wlo, st_exl, st_base)                   \
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                    \
                         "lui $26, %%hi(9f)\n\t"                              \
                         "daddiu $26, $26, %%lo(9f)\n\t"                      \
                         "sd $26, 0(%[res])\n\t"                              \
                         "mtc0 %[se], $12\n\t"                                \
                         "sd %[v], 0(%[a])\n\t"                               \
                         "mtc0 %[wl], $18\n\t"                                \
                         "mtc0 %[sb], $12\n\t"                                \
                         "nop\n"                                              \
                         "9:\t.set pop"                                       \
                         :                                                    \
                         : [res] "r"(&rom_exc.resume), [a] "r"(vaddr),        \
                           [v] "r"(val), [wl] "r"(wlo), [se] "r"(st_exl),     \
                           [sb] "r"(st_base)                                  \
                         : "$26", "memory")

void rom_main(void)
{
    uint64_t insn_addr, after_addr, got;
    uint32_t base;

    rom_exc_reset();
    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_wired(0);
    rom_set_entryhi(0);
    base = rom_get_status();

    rom_assert_eq(1, 0, rom_get_watchlo());
    rom_assert_eq(2, 0, rom_get_watchhi());

    /* WatchLo bit 2 is ignored; WatchHi keeps bits 7:0 (Figure 14-18). */
    rom_set_watchlo(0xffffffffu);
    rom_assert_eq(3, 0xfffffffbu, rom_get_watchlo());
    rom_set_watchhi(0xffffffffu);
    rom_assert_eq(4, 0x000000ffu, rom_get_watchhi());

    rom_set_watchlo(0);
    rom_set_watchhi(0);
    rom_assert_eq(5, 0, rom_get_watchlo());
    rom_assert_eq(6, 0, rom_get_watchhi());

    rom_exc_reset();
    *phys64(WATCH_PA) = 0x0123456789abcdefull;
    *phys64(OTHER_PA) = 0x1111111111111111ull;
    rom_assert_eq(7, 0, rom_exc.count);

    /* A watched store traps before retiring; EPC identifies the store. */
    rom_set_watchlo((uint32_t)WATCH_PA | WATCHLO_W);
    rom_exc_reset();
    TRY_SD(phys64(WATCH_PA), 0x5105105105105105ull, insn_addr);
    rom_assert_eq(8, 1, rom_exc.count);
    rom_assert_eq(9, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(10, ROM_EXC_WATCH, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(11, insn_addr, rom_exc.epc);
    rom_assert_eq(12, 0x0123456789abcdefull, *phys64(WATCH_PA));

    rom_exc_reset();
    *phys64(OTHER_PA) = 0x2222222222222222ull;
    rom_assert_eq(13, 0, rom_exc.count);

    /* WatchLo.W traps stores, not loads. */
    rom_exc_reset();
    got = *phys64(WATCH_PA);
    rom_assert_eq(14, 0x0123456789abcdefull, got);
    rom_assert_eq(15, 0, rom_exc.count);

    /* A word access within the watched doubleword also matches. */
    rom_exc_reset();
    TRY_SW(phys32(WATCH_PA + 4), 0xfeedf00du, insn_addr);
    rom_assert_eq(16, 1, rom_exc.count);
    rom_assert_eq(17, ROM_EXC_WATCH, ROM_CAUSE_EXC(rom_exc.cause));

    rom_set_watchlo(0);
    rom_exc_reset();
    *phys64(WATCH_PA) = 0x5105105105105105ull;
    rom_assert_eq(18, 0, rom_exc.count);
    rom_assert_eq(19, 0x5105105105105105ull, *phys64(WATCH_PA));

    /* WatchLo.R traps loads, not stores. */
    rom_set_watchlo((uint32_t)WATCH_PA | WATCHLO_R);
    rom_exc_reset();
    TRY_LD(phys64(WATCH_PA), got, insn_addr);
    rom_assert_eq(20, 1, rom_exc.count);
    rom_assert_eq(21, ROM_EXC_WATCH, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(22, insn_addr, rom_exc.epc);

    rom_exc_reset();
    *phys64(WATCH_PA) = 0xabcdabcdabcdabcdull;
    rom_assert_eq(23, 0, rom_exc.count);

    rom_exc_reset();
    got = *phys64(OTHER_PA);
    rom_assert_eq(24, 0x2222222222222222ull, got);
    rom_assert_eq(25, 0, rom_exc.count);

    /* WatchHi[39:32] participates in the physical address match. */
    rom_set_watchlo((uint32_t)WATCH_PA | WATCHLO_W);
    rom_set_watchhi(1);
    rom_exc_reset();
    *phys64(WATCH_PA) = 0x3333333333333333ull;
    rom_assert_eq(26, 0, rom_exc.count);
    rom_set_watchhi(0);

    /* A TLB alias of the watched physical page also matches. */
    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_pagemask(ROM_PAGEMASK_4K);
    rom_set_entryhi(VIRT_WIN);
    rom_set_entrylo0(rom_entrylo(WATCH_PA & ~0xfffull, MAP_RW,
                                 ROM_CACHE_NONCOHERENT));
    rom_set_entrylo1(rom_entrylo((WATCH_PA & ~0xfffull) + 0x1000, MAP_RW,
                                 ROM_CACHE_NONCOHERENT));
    rom_set_index(0);
    __asm__ __volatile__("tlbwi" : : : "memory");
    rom_set_entryhi(0);

    rom_exc_reset();
    TRY_SD(VIRT_WIN | (WATCH_PA & 0xfffull), 0x4444444444444444ull, insn_addr);
    rom_assert_eq(27, 1, rom_exc.count);
    rom_assert_eq(28, ROM_EXC_WATCH, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(29, insn_addr, rom_exc.epc);
    rom_assert_eq(30, 0x3333333333333333ull, *phys64(WATCH_PA));
    rom_tlb_park(ROM_TLB_ENTRIES);

    /* With EXL set, the store retires; delayed EPC points past the MTC0. */
    rom_exc_reset();
    DEFER_BLOCK(phys64(WATCH_PA), 0x5555555555555555ull,
                base | ROM_ST_EXL, base, after_addr);
    rom_assert_eq(31, 1, rom_exc.count);
    rom_assert_eq(32, ROM_EXC_WATCH, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(33, after_addr, rom_exc.epc);
    rom_assert_eq(34, 0x5555555555555555ull, *phys64(WATCH_PA));

    /* Writing WatchLo while EXL is set cancels the delayed exception. */
    rom_exc_reset();
    DEFER_CLEAR_BLOCK(phys64(WATCH_PA), 0x6666666666666666ull,
                      (uint32_t)WATCH_PA | WATCHLO_W,
                      base | ROM_ST_EXL, base);
    rom_assert_eq(35, 0, rom_exc.count);
    rom_assert_eq(36, 0x6666666666666666ull, *phys64(WATCH_PA));

    rom_set_watchlo(0);
    rom_set_watchhi(0);
    rom_exc_reset();
    *phys64(WATCH_PA) = 0;
    rom_assert_eq(37, 0, rom_exc.count);
}
