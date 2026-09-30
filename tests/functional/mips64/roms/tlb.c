/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test R4000-family TLB operations, including entry readback,
 * replacement and exception behavior.
 */

#include "rom.h"

const char rom_name[] = "tlb";

/*
 * Physical pages nothing else in a ROM uses: the harness keeps its data
 * below
 * physical 0x200000 and its stack just under that.
 */
#define PHYS_EVEN  0x00300000ull
#define PHYS_ODD   0x00301000ull
#define PHYS_BIG   0x02000000ull

/*
 * Virtual pages in the user segment, which is translated from kernel
 * mode
 * too.
 */
#define VIRT_PAIR  0x10000000ull
#define VIRT_INVAL 0x12000000ull
#define VIRT_DIRTY 0x13000000ull
#define VIRT_ASID  0x14000000ull
#define VIRT_MISS  0x18000000ull
#define VIRT_BIG   0x20000000ull

#define ASID_MINE  7
#define ASID_OTHER 8

#define VEC_BOOTSTRAP_GENERAL (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)
#define VEC_BOOTSTRAP_REFILL  (ROM_VEC_BOOTSTRAP | ROM_VEC_TLB_REFILL)

#define MAP_RW (ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G)
#define MAP_RO (ROM_ENTRYLO_V | ROM_ENTRYLO_G)

/*
 * The uncached window onto a physical address, for planting a value
 * where a
 * translated access should later find it.
 */
static volatile uint32_t *phys(uint64_t paddr)
{
    /*
     * Volatile preserves accesses through the uncached physical-memory
     * mapping.
     */
    return (volatile uint32_t *)(unsigned long)(0xffffffffa0000000ull + paddr);
}

static uint32_t load32(uint64_t vaddr)
{
    uint32_t v;

    __asm__ __volatile__("lw %0, 0(%1)" : "=r"(v) : "r"(vaddr) : "memory");
    return v;
}

/*
 * An access that is expected to fault, with a return address armed so
 * the
 * harness resumes after it instead of reporting it.
 */
#define TRY_LOAD(vaddr)                                                       \
    do {                                                                      \
        uint64_t dead__;                                                      \
        __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                \
                             "lui $26, %%hi(9f)\n\t"                          \
                             "daddiu $26, $26, %%lo(9f)\n\t"                  \
                             "sd $26, 0(%[res])\n\t"                          \
                             "lw %[d], 0(%[a])\n"                             \
                             "9:\t.set pop"                                   \
                             : [d] "=&r"(dead__)                              \
                             : [res] "r"(&rom_exc.resume), [a] "r"(vaddr)     \
                             : "$26", "memory");                              \
        (void)dead__;                                                         \
    } while (0)

#define TRY_STORE(vaddr)                                                      \
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                    \
                         "lui $26, %%hi(9f)\n\t"                             \
                         "daddiu $26, $26, %%lo(9f)\n\t"                      \
                         "sd $26, 0(%[res])\n\t"                              \
                         "sw $0, 0(%[a])\n"                                   \
                         "9:\t.set pop"                                       \
                         :                                                    \
                         : [res] "r"(&rom_exc.resume), [a] "r"(vaddr)         \
                         : "$26", "memory")

/* Force 32-bit sign extension before testing word writes to EntryLo. */
static void mtc0_word_entrylo0(uint64_t v)
{
    __asm__ __volatile__("sll %0, %0, 0\n\tmtc0 %0, $2" : "+r"(v));
}

static void mtc0_word_entrylo1(uint64_t v)
{
    __asm__ __volatile__("sll %0, %0, 0\n\tmtc0 %0, $3" : "+r"(v));
}

static void tlb_write_index(int index, uint64_t entryhi, uint64_t lo0,
                            uint64_t lo1, uint32_t mask)
{
    rom_set_pagemask(mask);
    rom_set_entryhi(entryhi);
    rom_set_entrylo0(lo0);
    rom_set_entrylo1(lo1);
    rom_set_index(index);
    __asm__ __volatile__("tlbwi" : : : "memory");
}

static uint32_t tlb_probe(uint64_t entryhi)
{
    rom_set_entryhi(entryhi);
    __asm__ __volatile__("tlbp" : : : "memory");
    return rom_get_index();
}

/*
 * Stage an entry for the harness's refill handler to install with a
 * random
 * write when the next miss arrives.
 */
static void arm_refill(uint64_t lo0, uint64_t lo1, uint32_t mask)
{
    rom_exc.entrylo0 = lo0;
    rom_exc.entrylo1 = lo1;
    rom_exc.pagemask = mask;
    rom_exc.refill = 1;
}

static const struct {
    uint32_t mask;
    uint64_t size;
} sizes[] = {
    { ROM_PAGEMASK_4K,   0x00001000ull },
    { ROM_PAGEMASK_16K,  0x00004000ull },
    { ROM_PAGEMASK_64K,  0x00010000ull },
    { ROM_PAGEMASK_256K, 0x00040000ull },
    { ROM_PAGEMASK_1M,   0x00100000ull },
    { ROM_PAGEMASK_4M,   0x00400000ull },
    { ROM_PAGEMASK_16M,  0x01000000ull },
};

void rom_main(void)
{
    uint32_t index, probe;
    unsigned i;
    int id;

    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_wired(0);
    rom_set_entryhi(0);

    *phys(PHYS_EVEN) = 0xc0ffee00u;
    *phys(PHYS_ODD) = 0xc0ffee01u;

    /*
     * With nothing mapped, a load from the user segment misses and
     * arrives at
     * the refill entry point. The faulting virtual page number is left
     * in
     * EntryHi with its low bit cleared, which is what lets a refill
     * handler
     * complete the entry without recomputing it.
     */
    rom_exc_reset();
    TRY_LOAD(VIRT_PAIR);
    rom_assert_eq(1, 1, rom_exc.count);
    rom_assert_eq(2, VEC_BOOTSTRAP_REFILL, rom_exc.vector);
    rom_assert_eq(3, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(4, VIRT_PAIR, rom_exc.badvaddr);
    rom_assert_eq(5, VIRT_PAIR, rom_exc.entryhi);

    /*
     * The same load with an entry staged: the refill handler installs
     * it and
     * the access completes, returning what was planted at the physical
     * page.
     * Exactly one exception is taken, and a second access takes none at
     * all
     * because the entry is now in the buffer.
     */
    rom_exc_reset();
    arm_refill(rom_entrylo(PHYS_EVEN, MAP_RW, ROM_CACHE_NONCOHERENT),
               rom_entrylo(PHYS_ODD, MAP_RW, ROM_CACHE_NONCOHERENT),
               ROM_PAGEMASK_4K);
    rom_assert_eq(6, 0xc0ffee00u, load32(VIRT_PAIR));
    rom_assert_eq(7, 1, rom_exc.count);
    rom_assert_eq(8, 0xc0ffee00u, load32(VIRT_PAIR));
    rom_assert_eq(9, 1, rom_exc.count);

    /*
     * One entry maps a pair of pages, so the second page of the pair is
     * reachable through the same entry without another miss.
     */
    rom_assert_eq(10, 0xc0ffee01u, load32(VIRT_PAIR + 0x1000));
    rom_assert_eq(11, 1, rom_exc.count);
    rom_exc.refill = 0;

    /*
     * A store through the same entry reaches the physical page.
     */
    rom_exc_reset();
    __asm__ __volatile__("sw %1, 0(%0)"
                         : : "r"(VIRT_PAIR), "r"(0x5eed0000u) : "memory");
    rom_assert_eq(12, 0, rom_exc.count);
    rom_assert_eq(13, 0x5eed0000u, *phys(PHYS_EVEN));

    /*
     * A probe finds that entry and reports its index with the failure
     * bit
     * clear; a probe for a page nothing maps reports failure in the
     * high bit.
     */
    index = tlb_probe(VIRT_PAIR);
    rom_assert_eq(14, 0, index & 0x80000000u);
    rom_assert(15, (index & 0x3f) < ROM_TLB_ENTRIES);

    probe = tlb_probe(VIRT_MISS);
    rom_assert_eq(16, 0x80000000u, probe & 0x80000000u);

    /*
     * Reading the entry the probe found returns what was installed.
     */
    rom_set_index(index & 0x3f);
    __asm__ __volatile__("tlbr" : : : "memory");
    rom_assert_eq(17, rom_entrylo(PHYS_EVEN, MAP_RW, ROM_CACHE_NONCOHERENT),
                  rom_get_entrylo0());
    rom_assert_eq(18, rom_entrylo(PHYS_ODD, MAP_RW, ROM_CACHE_NONCOHERENT),
                  rom_get_entrylo1());
    rom_assert_eq(19, ROM_PAGEMASK_4K, rom_get_pagemask());
    rom_assert_eq(20, VIRT_PAIR, rom_get_entryhi());

    /*
     * An entry whose valid bit is clear is a match, not a miss, so the
     * fault
     * goes to the general entry point rather than a refill entry point.
     * This
     * is
     * the distinction that lets a kernel tell "no translation yet" from
     * "the
     * page is not resident".
     */
    tlb_write_index(1, VIRT_INVAL,
                    rom_entrylo(PHYS_EVEN, ROM_ENTRYLO_G,
                                ROM_CACHE_NONCOHERENT),
                    rom_entrylo(PHYS_ODD, ROM_ENTRYLO_G, ROM_CACHE_NONCOHERENT),
                    ROM_PAGEMASK_4K);
    rom_exc_reset();
    TRY_LOAD(VIRT_INVAL);
    rom_assert_eq(21, 1, rom_exc.count);
    rom_assert_eq(22, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(23, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(24, VIRT_INVAL, rom_exc.badvaddr);

    /*
     * An entry that is valid but not marked dirty is read-only: loads
     * work,
     * stores raise a modified exception at the general entry point.
     */
    *phys(PHYS_EVEN) = 0xdeadbe00u;
    tlb_write_index(2, VIRT_DIRTY,
                    rom_entrylo(PHYS_EVEN, MAP_RO, ROM_CACHE_NONCOHERENT),
                    rom_entrylo(PHYS_ODD, MAP_RO, ROM_CACHE_NONCOHERENT),
                    ROM_PAGEMASK_4K);
    rom_exc_reset();
    rom_assert_eq(25, 0xdeadbe00u, load32(VIRT_DIRTY));
    rom_assert_eq(26, 0, rom_exc.count);

    TRY_STORE(VIRT_DIRTY);
    rom_assert_eq(27, 1, rom_exc.count);
    rom_assert_eq(28, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(29, ROM_EXC_MOD, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(30, 0xdeadbe00u, *phys(PHYS_EVEN));

    /*
     * An entry that is not global matches only while the current
     * address
     * space
     * identifier equals the one stored with it.
     */
    *phys(PHYS_EVEN) = 0xa51d0000u;
    tlb_write_index(3, VIRT_ASID | ASID_MINE,
                    rom_entrylo(PHYS_EVEN, ROM_ENTRYLO_V | ROM_ENTRYLO_D,
                                ROM_CACHE_NONCOHERENT),
                    rom_entrylo(PHYS_ODD, ROM_ENTRYLO_V | ROM_ENTRYLO_D,
                                ROM_CACHE_NONCOHERENT),
                    ROM_PAGEMASK_4K);

    rom_exc_reset();
    rom_set_entryhi(VIRT_ASID | ASID_MINE);
    rom_assert_eq(31, 0xa51d0000u, load32(VIRT_ASID));
    rom_assert_eq(32, 0, rom_exc.count);

    rom_set_entryhi(VIRT_ASID | ASID_OTHER);
    TRY_LOAD(VIRT_ASID);
    rom_assert_eq(33, 1, rom_exc.count);
    rom_assert_eq(34, VEC_BOOTSTRAP_REFILL, rom_exc.vector);
    rom_set_entryhi(0);

    /* Test each supported page size and both halves of its pair. */
    id = 35;
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        uint64_t size = sizes[i].size;
        uint64_t top = size - 4;

        *phys(PHYS_BIG + top) = 0x512e0000u + i;
        *phys(PHYS_BIG + size + top) = 0x512e8000u + i;

        rom_tlb_park(ROM_TLB_ENTRIES);
        tlb_write_index(4, VIRT_BIG,
                        rom_entrylo(PHYS_BIG, MAP_RW, ROM_CACHE_NONCOHERENT),
                        rom_entrylo(PHYS_BIG + size, MAP_RW,
                                    ROM_CACHE_NONCOHERENT),
                        sizes[i].mask);
        rom_set_index(4);
        __asm__ __volatile__("tlbr" : : : "memory");
        rom_assert_eq(id++, sizes[i].mask, rom_get_pagemask());

        rom_exc_reset();
        rom_assert_eq(id++, 0x512e0000u + i, load32(VIRT_BIG + top));
        rom_assert_eq(id++, 0x512e8000u + i, load32(VIRT_BIG + size + top));
        rom_assert_eq(id++, 0, rom_exc.count);
    }

    /*
     * The wired count is a floor on which entries a random write may
     * take.
     * With
     * a floor set, a sentinel installed below it survives any number of
     * refills.
     */
    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_wired(0);
    tlb_write_index(0, VIRT_PAIR,
                    rom_entrylo(PHYS_EVEN, MAP_RW, ROM_CACHE_NONCOHERENT),
                    rom_entrylo(PHYS_ODD, MAP_RW, ROM_CACHE_NONCOHERENT),
                    ROM_PAGEMASK_4K);
    rom_set_wired(40);

    rom_exc_reset();
    arm_refill(rom_entrylo(PHYS_EVEN, MAP_RW, ROM_CACHE_NONCOHERENT),
               rom_entrylo(PHYS_ODD, MAP_RW, ROM_CACHE_NONCOHERENT),
               ROM_PAGEMASK_4K);
    for (i = 0; i < 8; i++) {
        load32(VIRT_MISS + (uint64_t)i * 0x2000);
    }
    rom_exc.refill = 0;
    rom_assert_eq(id++, 8, rom_exc.count);

    rom_set_index(0);
    __asm__ __volatile__("tlbr" : : : "memory");
    rom_assert_eq(id++, VIRT_PAIR, rom_get_entryhi());
    rom_assert_eq(id++, rom_entrylo(PHYS_EVEN, MAP_RW, ROM_CACHE_NONCOHERENT),
                  rom_get_entrylo0());

    /* Random replacement may evict earlier refills; check the last one. */
    probe = tlb_probe(VIRT_MISS + 7 * 0x2000);
    rom_assert_eq(id++, 0, probe & 0x80000000u);
    rom_assert(id++, (probe & 0x3f) >= 40);

    rom_set_wired(0);
    rom_set_entryhi(0);

    /* The following register assertions are specific to R10000. */
    if (((rom_get_prid() >> 8) & 0xff) != 0x09) {
        return;
    }

    /* PageMask bits 24:13 are defined (R10000 manual, section 14.3). */
    rom_set_pagemask(0xffffffffu);
    rom_assert_eq(id++, ROM_PAGEMASK_16M, rom_get_pagemask());

    /* Bits 26:25 are outside the PageMask field. */
    rom_set_pagemask(0x07ffe000u);
    rom_assert_eq(id++, ROM_PAGEMASK_16M, rom_get_pagemask());

    rom_set_pagemask(ROM_PAGEMASK_4K);
    rom_assert_eq(id++, ROM_PAGEMASK_4K, rom_get_pagemask());

    /* TLBR clears masked EntryHi bits (R10000 manual, section 14.35). */
    tlb_write_index(4, VIRT_BIG | 0x01ffe000ull, 0, 0, ROM_PAGEMASK_16M);
    rom_set_index(4);
    __asm__ __volatile__("tlbr" : : : "memory");
    rom_assert_eq(id++, VIRT_BIG, rom_get_entryhi());
    rom_assert_eq(id++, ROM_PAGEMASK_16M, rom_get_pagemask());

    /* Word MTC0 sign extends (manual, Table 14-26, section 14.3). */
    mtc0_word_entrylo0(0x89abc000u);
    rom_assert_eq(id++, 0xc000000389abc000ull, rom_get_entrylo0());

    rom_set_entrylo0(((uint64_t)ROM_UC_MEMORY << ROM_ENTRYLO_UC_SHIFT) |
                     (1ull << 33));
    mtc0_word_entrylo0(0x01234000u);
    rom_assert_eq(id++, 0x0000000001234000ull, rom_get_entrylo0());

    mtc0_word_entrylo1(0x89abc000u);
    rom_assert_eq(id++, 0xc000000389abc000ull, rom_get_entrylo1());
    mtc0_word_entrylo1(0x01234000u);
    rom_assert_eq(id++, 0x0000000001234000ull, rom_get_entrylo1());

    /*
     * R10000 manual v2.0, TLBWI/TLBWR: clear PFN bits below the page size.
     */
    tlb_write_index(4, VIRT_BIG,
                    rom_entrylo(PHYS_BIG | 0x00345000ull, 0,
                                ROM_CACHE_NONCOHERENT),
                    rom_entrylo((PHYS_BIG + 0x1000000) | 0x00abc000ull, 0,
                                ROM_CACHE_NONCOHERENT),
                    ROM_PAGEMASK_16M);
    rom_set_index(4);
    __asm__ __volatile__("tlbr" : : : "memory");
    rom_assert_eq(id++, rom_entrylo(PHYS_BIG, 0, ROM_CACHE_NONCOHERENT),
                  rom_get_entrylo0());
    rom_assert_eq(id++,
                  rom_entrylo(PHYS_BIG + 0x1000000, 0, ROM_CACHE_NONCOHERENT),
                  rom_get_entrylo1());

    rom_set_entrylo0(0);
    rom_set_entrylo1(0);
    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_pagemask(ROM_PAGEMASK_4K);
    rom_set_entryhi(0);
}
