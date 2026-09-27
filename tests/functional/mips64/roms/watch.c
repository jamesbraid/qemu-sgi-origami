/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The WatchLo and WatchHi registers, and the Watch exception they
 * raise.
 *
 * The pair names one doubleword of physical memory; with an enable bit
 * set,
 * a load from it or a store to it raises the Watch exception. Kernels
 * use it
 * as a hardware data breakpoint, and a model that stores the registers
 * without ever comparing an access against them turns that debugging
 * aid
 * into silence.
 *
 * Oracle: the R10000 User's Manual version 2.0.
 *  - Its WatchLo and WatchHi registers section (section 14.16, Figure
 * 14-18,
 *    Table 14-16): both are 32-bit read/write registers holding a
 * physical
 *    address of a doubleword. WatchLo carries physical address bits
 * 31:3 in
 *    bits 31:3, the R bit (trap on load) in bit 1 and the W bit (trap
 * on
 *    store) in bit 0; WatchHi carries physical address bits 39:32 in
 * bits
 *    7:0. The remaining bits of each register are ignored on write and
 * read
 *    as zero.
 *  - Its Cause register exception code table (Table 14-13): code 23,
 * WATCH,
 *    "Reference to WatchHi/WatchLo address".
 *  - Its Watch exception section (chapter 17): the exception uses the
 * common
 *    exception vector. It is postponed while either EXL or ERL is set
 * in the
 *    Status register: the referencing instruction is executed and the
 *    exception is delayed until both are clear, with EPC holding the
 * address
 *    of the next unexecuted instruction. A delayed Watch exception is
 *    cleared by system reset or by an MTC0 to the WatchLo register.
 *
 * The comparison is against the physical address, which two assertions
 * pin
 * from opposite sides: an access through a TLB mapping of the watched
 * page
 * must trap even though its virtual address shares nothing with the
 * uncached window the watch was armed through, and an access to the
 * watched
 * doubleword with a different WatchHi — naming a physical address this
 * machine's memory does not even reach — must not.
 *
 * The registers exist in this form on the R10000 family alone (the
 * R4000's
 * pair lays its enables out differently and QEMU's R4000 model does not
 * implement them), so this ROM runs against the R10000 alone.
 */

#include "rom.h"

const char rom_name[] = "watch";

/*
 * Physical pages nothing else in a ROM uses: the harness keeps its data
 * below
 * physical 0x200000 and its stack just under that.
 */
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

/*
 * A doubleword store or load with a return address armed, so that a
 * Watch
 * exception continues after it, and with the address of the memory
 * instruction itself returned so EPC can be checked exactly.
 */
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

/*
 * The postponement block: raise EXL, execute the store, then clear EXL,
 * all
 * with a return address armed past the block. If the exception is
 * delayed
 * as the manual says, it arrives at the first instruction boundary
 * after
 * the Status write, so EPC must hold the address of the label right
 * after
 * the MTC0 — the next unexecuted instruction. The optional MTC0 to
 * WatchLo
 * between the store and the unmask is the manual's way of clearing a
 * delayed Watch exception; writing the register's current value back
 * keeps
 * the arming unchanged while still being a write.
 */
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

    /*
     * Nothing armed the pair, so both read zero out of reset.
     */
    rom_assert_eq(1, 0, rom_get_watchlo());
    rom_assert_eq(2, 0, rom_get_watchhi());

    /*
     * The field masks, from Figure 14-18: writing all ones leaves
     * WatchLo
     * with everything but bit 2, which is ignored on write and reads as
     * zero, and WatchHi with only its low eight bits.
     */
    rom_set_watchlo(0xffffffffu);
    rom_assert_eq(3, 0xfffffffbu, rom_get_watchlo());
    rom_set_watchhi(0xffffffffu);
    rom_assert_eq(4, 0x000000ffu, rom_get_watchhi());

    rom_set_watchlo(0);
    rom_set_watchhi(0);
    rom_assert_eq(5, 0, rom_get_watchlo());
    rom_assert_eq(6, 0, rom_get_watchhi());

    /*
     * A baseline store with nothing armed: no exception.
     */
    rom_exc_reset();
    *phys64(WATCH_PA) = 0x0123456789abcdefull;
    *phys64(OTHER_PA) = 0x1111111111111111ull;
    rom_assert_eq(7, 0, rom_exc.count);

    /*
     * Arm the W bit on the doubleword and store to it: exactly one
     * exception, at the common vector, carrying the WATCH code, with
     * EPC
     * naming the store itself — and the store must not have retired, so
     * the memory still holds the old value.
     */
    rom_set_watchlo((uint32_t)WATCH_PA | WATCHLO_W);
    rom_exc_reset();
    TRY_SD(phys64(WATCH_PA), 0x5105105105105105ull, insn_addr);
    rom_assert_eq(8, 1, rom_exc.count);
    rom_assert_eq(9, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(10, ROM_EXC_WATCH, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(11, insn_addr, rom_exc.epc);
    rom_assert_eq(12, 0x0123456789abcdefull, *phys64(WATCH_PA));

    /*
     * The doubleword next to it is not watched.
     */
    rom_exc_reset();
    *phys64(OTHER_PA) = 0x2222222222222222ull;
    rom_assert_eq(13, 0, rom_exc.count);

    /*
     * The W bit traps stores alone: a load from the watched doubleword
     * passes.
     */
    rom_exc_reset();
    got = *phys64(WATCH_PA);
    rom_assert_eq(14, 0x0123456789abcdefull, got);
    rom_assert_eq(15, 0, rom_exc.count);

    /*
     * A word store inside the doubleword is a reference to it: the
     * watched
     * unit is the doubleword, not just its first byte.
     */
    rom_exc_reset();
    TRY_SW(phys32(WATCH_PA + 4), 0xfeedf00du, insn_addr);
    rom_assert_eq(16, 1, rom_exc.count);
    rom_assert_eq(17, ROM_EXC_WATCH, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * Disarmed, the same store goes through.
     */
    rom_set_watchlo(0);
    rom_exc_reset();
    *phys64(WATCH_PA) = 0x5105105105105105ull;
    rom_assert_eq(18, 0, rom_exc.count);
    rom_assert_eq(19, 0x5105105105105105ull, *phys64(WATCH_PA));

    /*
     * The R bit, the same way: a load traps precisely, a store does
     * not.
     */
    rom_set_watchlo((uint32_t)WATCH_PA | WATCHLO_R);
    rom_exc_reset();
    TRY_LD(phys64(WATCH_PA), got, insn_addr);
    rom_assert_eq(20, 1, rom_exc.count);
    rom_assert_eq(21, ROM_EXC_WATCH, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(22, insn_addr, rom_exc.epc);

    rom_exc_reset();
    *phys64(WATCH_PA) = 0xabcdabcdabcdabcdull;
    rom_assert_eq(23, 0, rom_exc.count);

    /*
     * A load from the neighbouring doubleword passes.
     */
    rom_exc_reset();
    got = *phys64(OTHER_PA);
    rom_assert_eq(24, 0x2222222222222222ull, got);
    rom_assert_eq(25, 0, rom_exc.count);

    /*
     * WatchHi is part of the comparison: with bits 39:32 naming a
     * different
     * physical address, the same WatchLo matches nothing this access
     * touches.
     */
    rom_set_watchlo((uint32_t)WATCH_PA | WATCHLO_W);
    rom_set_watchhi(1);
    rom_exc_reset();
    *phys64(WATCH_PA) = 0x3333333333333333ull;
    rom_assert_eq(26, 0, rom_exc.count);
    rom_set_watchhi(0);

    /*
     * The comparison is physical: a store through a TLB mapping of the
     * watched page traps just the same, and just as precisely.
     */
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

    /*
     * Postponement. With EXL set the store is executed — the memory
     * changes — and no exception is taken until EXL is cleared again;
     * the delayed exception then arrives with the WATCH code and EPC
     * holding the next unexecuted instruction after the unmasking MTC0.
     */
    rom_exc_reset();
    DEFER_BLOCK(phys64(WATCH_PA), 0x5555555555555555ull,
                base | ROM_ST_EXL, base, after_addr);
    rom_assert_eq(31, 1, rom_exc.count);
    rom_assert_eq(32, ROM_EXC_WATCH, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(33, after_addr, rom_exc.epc);
    rom_assert_eq(34, 0x5555555555555555ull, *phys64(WATCH_PA));

    /*
     * An MTC0 to WatchLo clears a delayed Watch exception, the manual's
     * own escape hatch: defer a store the same way, write WatchLo while
     * still masked, and nothing may arrive when EXL clears.
     */
    rom_exc_reset();
    DEFER_CLEAR_BLOCK(phys64(WATCH_PA), 0x6666666666666666ull,
                      (uint32_t)WATCH_PA | WATCHLO_W,
                      base | ROM_ST_EXL, base);
    rom_assert_eq(35, 0, rom_exc.count);
    rom_assert_eq(36, 0x6666666666666666ull, *phys64(WATCH_PA));

    /*
     * Disarm and leave nothing behind.
     */
    rom_set_watchlo(0);
    rom_set_watchhi(0);
    rom_exc_reset();
    *phys64(WATCH_PA) = 0;
    rom_assert_eq(37, 0, rom_exc.count);
}
