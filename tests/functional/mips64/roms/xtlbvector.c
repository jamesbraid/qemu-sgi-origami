/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A TLB miss must arrive at the right one of the two refill entry
 * points.
 *
 * The processor has a separate entry point for misses on addresses that
 * need
 * 64-bit handling, and a 64-bit kernel puts a completely different
 * refill
 * handler there — one that reads a different page table through a
 * different
 * page-table-pointer register. Send a miss to the wrong entry point and
 * the
 * kernel walks the wrong table with the wrong shift, so the first page
 * fault
 * either installs a garbage translation or faults again forever. There
 * is no
 * console output at that point, so this has to be settled before a
 * kernel is
 * ever loaded.
 *
 * The R10000 changed how the choice is made. Oracle: the R10000 User's
 * Manual
 * version 2.0. Its TLB refill vector selection section states that "The
 * TLB
 * refill vector selection is based on the address space of the address
 * (user,
 * supervisor, or kernel) that caused the TLB miss, and the value of the
 * corresponding extended addressing bit in the Status register (UX, SX,
 * or
 * KX)", and adds that "The current operating mode of the processor is
 * not
 * important except that it plays a part in specifying in which address
 * space
 * an
 * address resides". The manual contrasts this with the R4400, which
 * chooses
 * from the mode the processor is currently running in. That is why
 * every
 * probe
 * below runs in kernel mode throughout and still expects the entry
 * point the
 * faulting address calls for.
 *
 * The manual's TLB refill vectors table gives the ranges. Two of its
 * rows are
 * used here: an address from 0x0000000000000000 to 0x000000007fffffff
 * belongs
 * to the user address space and takes the 32-bit entry point when the
 * user
 * extended addressing bit is clear and the extended entry point when it
 * is
 * set;
 * an address from 0xffffffffc0000000 to 0xffffffffffffffff belongs to
 * the
 * kernel address space and follows the kernel extended addressing bit
 * the
 * same
 * way.
 *
 * The manual's exception vector address table gives the addresses of
 * both
 * entry points in each of the two placements, and qualifies both refill
 * rows
 * with the exception level bit being clear. A miss taken while that bit
 * is
 * already set therefore goes to the entry point used for everything
 * else,
 * which
 * is what lets a refill handler fault on its own page table without
 * looping.
 *
 * One row of the refill vector table is deliberately left untested: the
 * supervisor 64-bit region reached from 0x4000000000000000, which the
 * manual
 * says follows the supervisor extended addressing bit. QEMU sends
 * misses
 * there
 * to the extended entry point whatever that bit holds. No operating
 * system we
 * intend to boot on this machine uses the supervisor address space at
 * all, so
 * the gap is recorded here rather than turned into a failing test.
 *
 * What the R4000 run of this ROM does and does not establish. Most of
 * what is
 * checked below is common ground: a miss on a kernel address follows
 * the
 * kernel
 * extended addressing bit on either processor, and so does a miss on a
 * user
 * address while the user bit is clear, because in kernel mode with both
 * bits
 * clear the two rules give the same answer. Two assertions are the
 * exception,
 * the two that touch a user-space address while the user extended
 * addressing
 * bit
 * is set and the kernel one is clear: a real R4400 would send those to
 * the
 * 32-bit entry point, following the mode it is running in, and the
 * R10000
 * sends
 * them to the extended one, following the address. QEMU's R4000 model
 * does
 * not
 * implement the R4400 rule — it selects from the faulting address for
 * every
 * 64-bit processor it models — so those two assertions passing there
 * says
 * only
 * that the ROM drives the machinery correctly, not that a known-good
 * R4400
 * agrees with them. The manual quoted above is the whole of their
 * authority.
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

/*
 * Stage a translation for the page pair containing an address, so the
 * miss
 * the
 * probe provokes is repaired by the harness and the access completes.
 * The
 * entries are global, which keeps the address space identifier out of
 * the
 * question entirely.
 */
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

    /*
     * Nothing here should disturb the state the startup code
     * established.
     */
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

    /*
     * The repaired translation really did reach the intended memory.
     */
    rom_assert_eq(5, 0x5a5a5a5aull,
                  /*
                   * Volatile preserves the marker access through the
                   * uncached
                   * KSEG1 alias.
                   */
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
