/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test TLB refill vector selection by faulting address space and its
 * UX, SX or KX enable, including exceptions taken with EXL set.
 * The supervisor-space row remains untested.
 * QEMU R4000 selects by faulting address, unlike real R4400. The two
 * UX-set user-address assertions check ROM mechanics on R4000, not R4400 behavior.
 *
 * Source: R10000 User's Manual, version 2.0, TLB refill vector selection
 * section and refill-vector and exception-vector tables.
 */

#include "rom.h"

const char rom_name[] = "xtlbvector";

/*
 * Free memory well clear of the ROM's own data and stack.
 */
#define USER_TARGET_PHYS   0x00300000ull
#define KERNEL_TARGET_PHYS 0x00310000ull

/*
 * One address in the user address space, and one in the third kernel
 * compatibility segment, both mapped through the TLB.
 */
#define USER_VADDR   0x0000000010000000ull
#define KERNEL_VADDR 0xffffffffe0100000ull

#define BOOTSTRAP_TLB  (ROM_VEC_BOOTSTRAP | ROM_VEC_TLB_REFILL)
#define BOOTSTRAP_XTLB (ROM_VEC_BOOTSTRAP | ROM_VEC_XTLB_REFILL)
#define BOOTSTRAP_GEN  (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)

/* The global entry lets the harness repair the miss without an ASID check. */
static void arm_refill(uint64_t paddr)
{
    const uint64_t flags = ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G;

    rom_exc_reset();
    rom_exc.entrylo0 = rom_entrylo(paddr, flags, ROM_CACHE_NONCOHERENT);
    rom_exc.entrylo1 = rom_entrylo(paddr + 0x1000, flags,
                                   ROM_CACHE_NONCOHERENT);
    rom_exc.pagemask = ROM_PAGEMASK_4K;
    rom_exc.refill = 1;
}

/*
 * Provoke one miss on an address and report which entry point handled
 * it.
 */
static uint64_t probe(uint64_t vaddr, uint64_t paddr)
{
    rom_tlb_park(ROM_TLB_ENTRIES);
    arm_refill(paddr);
    __asm__ __volatile__("sw %1, 0(%0)"
                         :
                         : "r"(vaddr), "r"(0x5a5a5a5aull)
                         : "memory");
    return rom_exc.vector;
}

void rom_main(void)
{
    uint32_t status = rom_get_status();
    uint64_t seen;

    rom_assert_eq(1, ROM_ST_BEV, status);

    /*
     * A user-space address with the user extended addressing bit clear
     * takes
     * the 32-bit entry point, even though the processor is in kernel
     * mode.
     */
    rom_set_status(ROM_ST_BEV);
    seen = probe(USER_VADDR, USER_TARGET_PHYS);
    rom_assert_eq(2, BOOTSTRAP_TLB, seen);
    rom_assert_eq(3, 1, rom_exc.count);
    rom_assert_eq(4, USER_VADDR, rom_exc.badvaddr);

    rom_assert_eq(5, 0x5a5a5a5aull,
                  *(volatile uint32_t *)(0xffffffffa0000000ull +
                                         USER_TARGET_PHYS));

    /*
     * The same address with that bit set takes the extended entry
     * point.
     */
    rom_set_status(ROM_ST_BEV | ROM_ST_UX);
    seen = probe(USER_VADDR, USER_TARGET_PHYS);
    rom_assert_eq(6, BOOTSTRAP_XTLB, seen);
    rom_assert_eq(7, 1, rom_exc.count);

    /*
     * A kernel-space address follows the kernel extended addressing
     * bit, and
     * is
     * unaffected by the user one.
     */
    rom_set_status(ROM_ST_BEV | ROM_ST_UX);
    seen = probe(KERNEL_VADDR, KERNEL_TARGET_PHYS);
    rom_assert_eq(8, BOOTSTRAP_TLB, seen);

    rom_set_status(ROM_ST_BEV | ROM_ST_KX);
    seen = probe(KERNEL_VADDR, KERNEL_TARGET_PHYS);
    rom_assert_eq(9, BOOTSTRAP_XTLB, seen);

    /*
     * A miss taken with the exception level bit already set goes to the
     * entry
     * point used for everything else. The harness cannot repair that
     * miss,
     * because repairing one is only sensible at a refill entry point,
     * so the
     * access is armed to be stepped over instead and never completes.
     */
    rom_set_status(ROM_ST_BEV);
    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_exc_reset();
    rom_set_status(ROM_ST_BEV | ROM_ST_EXL);
    ROM_TRY("sw %[v], 0(%[p])",
            [p] "r"(USER_VADDR), [v] "r"(0x5a5a5a5aull));
    rom_assert_eq(10, BOOTSTRAP_GEN, rom_exc.vector);
    rom_assert_eq(11, 1, rom_exc.count);
    rom_assert_eq(12, ROM_EXC_TLBS, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * Returning from that exception cleared the exception level bit, so
     * the
     * same miss now reaches a refill entry point again. With the
     * boot-exception-vector bit clear it reaches the copies the startup
     * code
     * placed in low memory, which the harness records without the bit
     * that
     * marks the copies in ROM.
     */
    rom_set_status(0);
    seen = probe(USER_VADDR, USER_TARGET_PHYS);
    rom_assert_eq(13, ROM_VEC_TLB_REFILL, seen);

    rom_set_status(ROM_ST_UX);
    seen = probe(USER_VADDR, USER_TARGET_PHYS);
    rom_assert_eq(14, ROM_VEC_XTLB_REFILL, seen);

    /*
     * Leave the processor as the startup code had it.
     */
    rom_set_status(ROM_ST_BEV);
    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_exc_reset();
}
