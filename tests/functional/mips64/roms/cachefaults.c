/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Check address and TLB exceptions raised by R10000 CACHE operations. */

#include "rom.h"

const char rom_name[] = "cachefaults";

/* Scratch page and virtual aliases used by the translation probes. */
#define PHYS_PAGE  0x00300000ull
#define VIRT_VALID 0x10000000ull
#define VIRT_INVAL 0x12000000ull
#define VIRT_RO    0x13000000ull
#define VIRT_MISS  0x18000000ull

#define VEC_BOOTSTRAP_GENERAL (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)
#define VEC_BOOTSTRAP_REFILL  (ROM_VEC_BOOTSTRAP | ROM_VEC_TLB_REFILL)

#define MAP_RW (ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G)
#define MAP_RO (ROM_ENTRYLO_V | ROM_ENTRYLO_G)

/* Keep the scratch object materialized for cache operations. */
static volatile unsigned char scratch[256] __attribute__((aligned(128)));

#define KSEG0_VIEW(p) ((unsigned long)(p) & ~0x20000000ul)

/* Arm exception recovery before a potentially faulting operation. */
#define TRY_CACHE(op, vaddr) \
    ROM_TRY("cache " #op ", 0(%[a])", [a] "r"(vaddr))

static void tlb_write_index(int index, uint64_t entryhi, uint64_t lo0,
                            uint64_t lo1)
{
    rom_set_pagemask(ROM_PAGEMASK_4K);
    rom_set_entryhi(entryhi);
    rom_set_entrylo0(lo0);
    rom_set_entrylo1(lo1);
    rom_set_index(index);
    __asm__ __volatile__("tlbwi" : : : "memory");
}

void rom_main(void)
{
    unsigned long kaddr = KSEG0_VIEW(&scratch[0]);

    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_wired(0);
    rom_set_entryhi(0);

    /*
     * A valid mapping, an invalid one, and a read-only one.
     */
    tlb_write_index(0, VIRT_VALID,
                    rom_entrylo(PHYS_PAGE, MAP_RW, ROM_CACHE_NONCOHERENT),
                    rom_entrylo(PHYS_PAGE + 0x1000, MAP_RW,
                                ROM_CACHE_NONCOHERENT));
    tlb_write_index(1, VIRT_INVAL, ROM_ENTRYLO_G, ROM_ENTRYLO_G);
    tlb_write_index(2, VIRT_RO,
                    rom_entrylo(PHYS_PAGE, MAP_RO, ROM_CACHE_NONCOHERENT),
                    rom_entrylo(PHYS_PAGE + 0x1000, MAP_RO,
                                ROM_CACHE_NONCOHERENT));

    /*
     * Bit 1 of an Index operation's address generates an Address Error,
     * with the operation's virtual address in BadVAddr.
     */
    rom_exc_reset();
    TRY_CACHE(0x07, kaddr | 2);
    rom_assert_eq(1, 1, rom_exc.count);
    rom_assert_eq(2, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(3, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(4, kaddr | 2, rom_exc.badvaddr);

    /*
     * Bit 0 of an Index operation is the way indicator, never an error.
     */
    rom_exc_reset();
    TRY_CACHE(0x07, kaddr | 1);
    rom_assert_eq(5, 0, rom_exc.count);

    /*
     * Hit operations ignore low address bits. Only Index bit 1 causes an
     * alignment error; rejecting other low bits breaks normal way selects.
     */
    rom_exc_reset();
    TRY_CACHE(0x11, kaddr | 1);
    rom_assert_eq(6, 0, rom_exc.count);
    rom_exc_reset();
    TRY_CACHE(0x11, kaddr | 3);
    rom_assert_eq(7, 0, rom_exc.count);
    rom_exc_reset();
    TRY_CACHE(0x17, kaddr | 1);
    rom_assert_eq(8, 0, rom_exc.count);
    rom_exc_reset();
    TRY_CACHE(0x17, kaddr | 2);
    rom_assert_eq(9, 0, rom_exc.count);

    /* Hit operations still require translation and raise TLB Refill. */
    rom_exc_reset();
    TRY_CACHE(0x11, VIRT_MISS);
    rom_assert_eq(10, 1, rom_exc.count);
    rom_assert_eq(11, VEC_BOOTSTRAP_REFILL, rom_exc.vector);
    rom_assert_eq(12, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(13, VIRT_MISS, rom_exc.badvaddr);

    /*
     * Through a mapping with the valid bit clear: TLB Invalid.
     */
    rom_exc_reset();
    TRY_CACHE(0x11, VIRT_INVAL);
    rom_assert_eq(14, 1, rom_exc.count);
    rom_assert_eq(15, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(16, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(17, VIRT_INVAL, rom_exc.badvaddr);

    /* Index operations also translate and are not exempt from TLB misses. */
    rom_exc_reset();
    TRY_CACHE(0x00, VIRT_MISS);
    rom_assert_eq(18, 1, rom_exc.count);
    rom_assert_eq(19, VEC_BOOTSTRAP_REFILL, rom_exc.vector);
    rom_assert_eq(20, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));

    /* Writeback-invalidate through a clean mapping must not raise Modified. */
    rom_exc_reset();
    TRY_CACHE(0x15, VIRT_RO);
    rom_assert_eq(21, 0, rom_exc.count);

    /*
     * A translated operation through a valid mapping simply works.
     */
    rom_exc_reset();
    TRY_CACHE(0x07, VIRT_VALID);
    rom_assert_eq(22, 0, rom_exc.count);
    rom_assert_eq(23, ~0ull, rom_exc.vector);
}
