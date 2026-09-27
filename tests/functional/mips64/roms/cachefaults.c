/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The exceptions a CACHE instruction takes on the R10000, and the ones
 * it
 * must not.
 *
 * The manual's notes on CACHE instruction operations say TLB Refill and
 * TLB
 * Invalid exceptions "can occur on any operation" — index operations
 * included, which is why the same notes advise unmapped addresses for
 * them
 * — and that "the operation never causes TLB Modified exceptions"; the
 * CACHE instruction's own section adds that CacheOps "may induce
 * Address
 * Error or TLBL exceptions (Refill or Invalid) during address
 * translation,
 * but never take a TLBS or Mod exception". For address errors the
 * low-bit
 * rules are: bit 0 of an Index operation is the way indicator and never
 * checked, bit 1 of an Index operation generates an Address Error if
 * set,
 * and a Hit operation's low bits are don't-care — the notes' one
 * sentence
 * demanding them zero is contradicted two sentences later by "a CacheOp
 * is
 * never checked for alignment Address Error exceptions, only for
 * privilege-type", and by the shipped kernels that set bit 0.
 *
 * The addresses in the exception record are asserted too: an address
 * error
 * or a translation fault must leave the operation's own virtual address
 * in
 * BadVAddr, exactly as a load would.
 *
 * This ROM is R10000-only: QEMU's R4000 model treats the whole
 * instruction
 * as a no-operation, so there is nothing there to take these
 * exceptions.
 *
 * Oracle: MIPS R10000 Microprocessor User's Manual, version 2.0 of
 * October
 * 10, 1996: the notes on CACHE instruction operations in the CACHE
 * chapter,
 * and the CACHE instruction section of the coprocessor 0 chapter.
 */

#include "rom.h"

const char rom_name[] = "cachefaults";

/*
 * A physical page nothing else in a ROM uses, and user-segment virtual
 * pages translated from kernel mode too.
 */
#define PHYS_PAGE  0x00300000ull
#define VIRT_VALID 0x10000000ull
#define VIRT_INVAL 0x12000000ull
#define VIRT_RO    0x13000000ull
#define VIRT_MISS  0x18000000ull

#define VEC_BOOTSTRAP_GENERAL (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)
#define VEC_BOOTSTRAP_REFILL  (ROM_VEC_BOOTSTRAP | ROM_VEC_TLB_REFILL)

#define MAP_RW (ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G)
#define MAP_RO (ROM_ENTRYLO_V | ROM_ENTRYLO_G)

/*
 * Volatile keeps the memory accesses visible to this cache test.
 */
static volatile unsigned char scratch[256] __attribute__((aligned(128)));

#define KSEG0_VIEW(p) ((unsigned long)(p) & ~0x20000000ul)

/*
 * A cache operation that may fault, with the resume address armed.
 */
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
     * A Hit operation's low address bits are don't-care, never an
     * alignment fault: the operation tag-searches both ways, the
     * manual's
     * address-error section limits CacheOps to privilege-type address
     * errors, and the secondary flush every shipped kernel runs sets
     * bit 0
     * as a vestigial way-select it expects ignored. Only an Index
     * operation's bit 1 faults.
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

    /*
     * A Hit operation through an unmapped page takes a TLB Refill, like
     * the
     * load its translation is.
     */
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

    /*
     * An index operation is "any operation" too: unmapped addresses
     * avoid
     * the exception only because they never miss, not because index
     * operations are exempt.
     */
    rom_exc_reset();
    TRY_CACHE(0x00, VIRT_MISS);
    rom_assert_eq(18, 1, rom_exc.count);
    rom_assert_eq(19, VEC_BOOTSTRAP_REFILL, rom_exc.vector);
    rom_assert_eq(20, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * Never a TLB Modified exception: a writeback-invalidate through a
     * mapping whose dirty bit is clear is still a load-type
     * translation.
     */
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
