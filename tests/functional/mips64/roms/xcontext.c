/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The two page-table-pointer registers are packed for 44-bit addresses.
 *
 * A refill handler does almost no work: it reads the page-table pointer
 * the
 * hardware just built for it, loads a pair of entries from there and
 * returns.
 * If the fields in that pointer are one bit out of place, every fault
 * lands
 * on
 * the wrong page table entry, and the failure looks like random memory
 * corruption rather than a broken register. The R10000 moved those
 * fields
 * relative to the R4400 because its virtual addresses are four bits
 * wider, so
 * a model that inherits the R4400 packing is wrong in exactly that way.
 *
 * Oracle: the MIPS R10000 User's Manual version 2.0.
 *
 * Its memory management chapter establishes the width: the processor
 * uses
 * virtual address bits 43 down to 0 with region bits 63 down to 59,
 * bits 58
 * down to 44 are sign extensions of bit 43, the 64-bit user segment is
 * 2 to
 * the 44 bytes, and a user address whose bits 63 down to 44 are not
 * zero
 * raises an address error. The R4400's segments are 2 to the 40 bytes.
 *
 * Its Context register section (section 14.4, Figure 14-4 and Table
 * 14-5)
 * gives that register as a 41-bit operating-system-owned page-table
 * base in
 * bits 63 down to 23, a 19-bit bad-virtual-page-number field in bits 22
 * down
 * to 4 holding virtual address bits 31 down to 13, and zeroes in bits 3
 * down
 * to 0. Hardware writes the page-number field on a miss; the base is
 * read/write. This layout is the same on both processors, because it
 * only
 * describes 32-bit addresses.
 *
 * Its XContext register section (section 14.17, Figure 14-19 and Table
 * 14-17)
 * gives the wide register as a 27-bit page-table base in bits 63 down
 * to 37,
 * a
 * two-bit region field in bits 36 and 35 holding virtual address bits
 * 63 and
 * 62 with 00 meaning user and 11 meaning kernel, a 31-bit
 * bad-virtual-page-number field in bits 34 down to 4 holding virtual
 * address
 * bits 43 down to 13, and zeroes in bits 3 down to 0. On the R4400 that
 * page-number field is 27 bits and the region field sits at bits 32 and
 * 31,
 * so
 * the base boundary is the discriminator between the two processors.
 * The same
 * section also records that the R10000 stopped sharing the base field
 * between
 * the two registers, which the R4400 did, and the refill vector
 * discussion
 * adds that every buffer exception loads the page-number field of both.
 *
 * Bit 12 of the faulting address never appears in either page-number
 * field,
 * because one buffer entry maps an even and an odd page and that bit
 * chooses
 * between them.
 *
 * The first few assertions describe behaviour both processors share, so
 * they
 * are checks on this ROM rather than on the model. The ROM then reaches
 * the
 * base-field boundary, which is where a processor that is not an R10000
 * stops.
 */

#include "rom.h"

const char rom_name[] = "xcontext";

/*
 * The field positions, written as arithmetic rather than as finished
 * constants
 * so that each expected value below shows which manual field it came
 * from.
 */
#define XCTX_PTEBASE_SHIFT  37
#define XCTX_PTEBASE_BITS   27
#define XCTX_R_SHIFT        35
#define XCTX_VPN2_SHIFT     4
#define XCTX_VPN2_BITS      31

#define CTX_PTEBASE_SHIFT   23
#define CTX_PTEBASE_BITS    41
#define CTX_VPN2_SHIFT      4
#define CTX_VPN2_BITS       19

#define MASK(bits)          ((1ull << (bits)) - 1ull)
#define XCTX_PTEBASE_MASK   (MASK(XCTX_PTEBASE_BITS) << XCTX_PTEBASE_SHIFT)
#define CTX_PTEBASE_MASK    (MASK(CTX_PTEBASE_BITS) << CTX_PTEBASE_SHIFT)

/*
 * What the hardware must place in each page-number field for an
 * address.
 */
static uint64_t xctx_vpn2(uint64_t va)
{
    return ((va >> 13) & MASK(XCTX_VPN2_BITS)) << XCTX_VPN2_SHIFT;
}

static uint64_t xctx_region(uint64_t va)
{
    return ((va >> 62) & 3ull) << XCTX_R_SHIFT;
}

static uint64_t ctx_vpn2(uint64_t va)
{
    return ((va >> 13) & MASK(CTX_VPN2_BITS)) << CTX_VPN2_SHIFT;
}

/*
 * Bases distinctive enough that a misplaced field boundary is obvious.
 */
#define XCTX_BASE 0x0123456ull                  /* 27 bits */
#define CTX_BASE  0x00000123456789ull           /* 41 bits */

/*
 * Addresses chosen so each one exercises a different part of the
 * packing.
 *
 * The first sits above four gigabytes but inside the 2-to-the-40
 * segment both
 * processors have, so it faults the same way on either and its page
 * number
 * fills bits the 19-bit field cannot hold.
 *
 * The second has bits 43 and 41 set, which is inside the R10000's
 * 2-to-the-44
 * user segment and outside the R4400's, so it distinguishes the two.
 *
 * The third is the same offset in the 64-bit kernel region, whose
 * region bits
 * are 11.
 *
 * The last pair differ only in bit 12, the bit that chooses between the
 * two
 * pages one buffer entry maps.
 */
#define VA_USER_WIDE    0x0000000100002000ull
#define VA_USER_HIGH    0x00000a0000002000ull
#define VA_KERNEL_HIGH  0xc00000a000002000ull
#define VA_PAIR_EVEN    0x0000000200004000ull
#define VA_PAIR_ODD     (VA_PAIR_EVEN | 0x1000ull)

/*
 * Spare physical memory: the harness uses only the first two megabytes.
 */
#define PA_SCRATCH 0x400000ull

/*
 * Touch an address that has no translation, with a replacement entry
 * staged
 * so
 * that the handler repairs the miss and the access completes. An
 * address
 * error
 * instead of a miss goes to the general entry point, where the armed
 * return
 * address the block carries takes over, so a processor with narrower
 * segments
 * reports a wrong exception cause rather than stopping the ROM dead.
 */
static void fault_at(uint64_t vaddr, uint64_t paddr)
{
    rom_exc_reset();
    rom_exc.entrylo0 = rom_entrylo(paddr, ROM_ENTRYLO_V | ROM_ENTRYLO_G,
                                   ROM_CACHE_NONCOHERENT);
    rom_exc.entrylo1 = rom_entrylo(paddr + 0x1000,
                                   ROM_ENTRYLO_V | ROM_ENTRYLO_G,
                                   ROM_CACHE_NONCOHERENT);
    rom_exc.pagemask = ROM_PAGEMASK_4K;
    rom_exc.refill = 1;

    ROM_TRY("ld $27, 0(%[a])", [a] "r"(vaddr));
}

void rom_main(void)
{
    uint64_t xctx, ctx, wide_vpn2, high_vpn2;

    /*
     * Extended addressing on for both the user and the kernel segments:
     * the
     * wide addresses below are unreachable without it, and which of the
     * two
     * refill entry points runs is beside the point here.
     */
    rom_set_status(rom_get_status() | ROM_ST_KX | ROM_ST_UX);
    rom_tlb_park(ROM_TLB_ENTRIES);

    rom_set_xcontext(XCTX_BASE << XCTX_PTEBASE_SHIFT);
    rom_set_context(CTX_BASE << CTX_PTEBASE_SHIFT);

    /*
     * Establish known page-number fields with a miss both processors
     * take.
     */
    fault_at(VA_USER_WIDE, PA_SCRATCH);
    rom_assert_eq(1, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(2, ROM_VEC_XTLB_REFILL,
                  rom_exc.vector & ~(uint64_t)ROM_VEC_BOOTSTRAP);

    /*
     * The narrow register truncates at address bit 31; the wide one
     * does not.
     */
    wide_vpn2 = xctx_vpn2(VA_USER_WIDE);
    rom_assert_eq(3, ctx_vpn2(VA_USER_WIDE),
                  rom_exc.context & ~CTX_PTEBASE_MASK);
    rom_assert_eq(4, wide_vpn2, rom_exc.xcontext & ~XCTX_PTEBASE_MASK);

    /*
     * A write reaches only the base field. Since the page-number fields
     * now
     * hold known values, writing every bit shows exactly where the base
     * ends:
     * everything below the boundary must survive the write untouched.
     */
    rom_set_context(~0ull);
    ctx = rom_get_context();
    rom_assert_eq(5, CTX_PTEBASE_MASK | ctx_vpn2(VA_USER_WIDE), ctx);

    rom_set_xcontext(~0ull);
    xctx = rom_get_xcontext();
    rom_assert_eq(6, XCTX_PTEBASE_MASK | wide_vpn2, xctx);

    /*
     * The two bases are separate registers, not one shared field.
     */
    rom_set_context(CTX_BASE << CTX_PTEBASE_SHIFT);
    rom_set_xcontext(XCTX_BASE << XCTX_PTEBASE_SHIFT);
    rom_assert_eq(7, CTX_BASE, rom_get_context() >> CTX_PTEBASE_SHIFT);
    rom_assert_eq(8, XCTX_BASE, rom_get_xcontext() >> XCTX_PTEBASE_SHIFT);

    /*
     * An address with bits 43 and 41 set: inside the R10000's user
     * segment,
     * beyond the R4400's, and wide enough to fill the high end of the
     * 31-bit
     * page-number field.
     */
    rom_tlb_park(ROM_TLB_ENTRIES);
    fault_at(VA_USER_HIGH, PA_SCRATCH + 0x10000);
    high_vpn2 = xctx_vpn2(VA_USER_HIGH);
    rom_assert_eq(9, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(10, high_vpn2, rom_exc.xcontext & ~XCTX_PTEBASE_MASK);
    rom_assert_eq(11, 0, (rom_exc.xcontext >> XCTX_R_SHIFT) & 3);
    rom_assert_eq(12, XCTX_BASE, rom_exc.xcontext >> XCTX_PTEBASE_SHIFT);

    /*
     * The narrow register saw the same fault, truncated to address bit
     * 31.
     */
    rom_assert_eq(13, ctx_vpn2(VA_USER_HIGH),
                  rom_exc.context & ~CTX_PTEBASE_MASK);
    rom_assert_eq(14, CTX_BASE, rom_exc.context >> CTX_PTEBASE_SHIFT);

    /*
     * The same offset in the 64-bit kernel region reports region bits
     * 11.
     */
    rom_tlb_park(ROM_TLB_ENTRIES);
    fault_at(VA_KERNEL_HIGH, PA_SCRATCH + 0x20000);
    rom_assert_eq(15, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(16, 3, (rom_exc.xcontext >> XCTX_R_SHIFT) & 3);
    rom_assert_eq(17, xctx_region(VA_KERNEL_HIGH) | xctx_vpn2(VA_KERNEL_HIGH),
                  rom_exc.xcontext & ~XCTX_PTEBASE_MASK);

    /*
     * Two addresses differing only in bit 12 pack identically, because
     * that
     * bit selects between the even and odd page of one buffer entry
     * rather
     * than taking part in the page number. The buffer is parked between
     * the
     * two so the second address misses instead of hitting the entry the
     * first
     * miss installed for the pair.
     */
    rom_tlb_park(ROM_TLB_ENTRIES);
    fault_at(VA_PAIR_EVEN, PA_SCRATCH + 0x30000);
    rom_assert_eq(18, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    xctx = rom_exc.xcontext;
    ctx = rom_exc.context;

    rom_tlb_park(ROM_TLB_ENTRIES);
    fault_at(VA_PAIR_ODD, PA_SCRATCH + 0x30000);
    rom_assert_eq(19, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(20, xctx, rom_exc.xcontext);
    rom_assert_eq(21, ctx, rom_exc.context);
}
