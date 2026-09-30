/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test Context and XContext field packing for 44-bit virtual addresses.
 * R10000 XContext uses a wider page-number field than the R4400, and
 * its base field is independent of Context.
 *
 * Source: MIPS R10000 User's Manual, version 2.0, sections 14.4 and
 * 14.17, and the memory-management chapter.
 */

#include "rom.h"

const char rom_name[] = "xcontext";

/* Context and XContext field positions from the manual. */
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

/* Distinct base values expose field-boundary errors. */
#define XCTX_BASE 0x0123456ull                  /* 27 bits */
#define CTX_BASE  0x00000123456789ull           /* 41 bits */

/* Addresses exercise high VPN, region, and even/odd page fields. */
#define VA_USER_WIDE    0x0000000100002000ull
#define VA_USER_HIGH    0x00000a0000002000ull
#define VA_KERNEL_HIGH  0xc00000a000002000ull
#define VA_PAIR_EVEN    0x0000000200004000ull
#define VA_PAIR_ODD     (VA_PAIR_EVEN | 0x1000ull)

/* Outside the harness's first 2 MiB. */
#define PA_SCRATCH 0x400000ull

/* Stage refill so unsupported address widths fail through an assertion. */
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

    /* Enable 64-bit addressing for user and kernel segments. */
    rom_set_status(rom_get_status() | ROM_ST_KX | ROM_ST_UX);
    rom_tlb_park(ROM_TLB_ENTRIES);

    rom_set_xcontext(XCTX_BASE << XCTX_PTEBASE_SHIFT);
    rom_set_context(CTX_BASE << CTX_PTEBASE_SHIFT);

    /* Both processors accept this address, establishing known VPN fields. */
    fault_at(VA_USER_WIDE, PA_SCRATCH);
    rom_assert_eq(1, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(2, ROM_VEC_XTLB_REFILL,
                  rom_exc.vector & ~(uint64_t)ROM_VEC_BOOTSTRAP);

    /* Context truncates the VPN at bit 31; XContext retains the wider VPN. */
    wide_vpn2 = xctx_vpn2(VA_USER_WIDE);
    rom_assert_eq(3, ctx_vpn2(VA_USER_WIDE),
                  rom_exc.context & ~CTX_PTEBASE_MASK);
    rom_assert_eq(4, wide_vpn2, rom_exc.xcontext & ~XCTX_PTEBASE_MASK);

    /* Writing ones changes the base fields but preserves the VPN fields. */
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

    /* This address is valid only in the R10000's wider user segment. */
    rom_tlb_park(ROM_TLB_ENTRIES);
    fault_at(VA_USER_HIGH, PA_SCRATCH + 0x10000);
    high_vpn2 = xctx_vpn2(VA_USER_HIGH);
    rom_assert_eq(9, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(10, high_vpn2, rom_exc.xcontext & ~XCTX_PTEBASE_MASK);
    rom_assert_eq(11, 0, (rom_exc.xcontext >> XCTX_R_SHIFT) & 3);
    rom_assert_eq(12, XCTX_BASE, rom_exc.xcontext >> XCTX_PTEBASE_SHIFT);

    /* Context still truncates the wider address at bit 31. */
    rom_assert_eq(13, ctx_vpn2(VA_USER_HIGH),
                  rom_exc.context & ~CTX_PTEBASE_MASK);
    rom_assert_eq(14, CTX_BASE, rom_exc.context >> CTX_PTEBASE_SHIFT);

    /* The kernel address sets the XContext region field to 3. */
    rom_tlb_park(ROM_TLB_ENTRIES);
    fault_at(VA_KERNEL_HIGH, PA_SCRATCH + 0x20000);
    rom_assert_eq(15, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(16, 3, (rom_exc.xcontext >> XCTX_R_SHIFT) & 3);
    rom_assert_eq(17, xctx_region(VA_KERNEL_HIGH) | xctx_vpn2(VA_KERNEL_HIGH),
                  rom_exc.xcontext & ~XCTX_PTEBASE_MASK);

    /* Bit 12 selects the paired page; park the TLB so both addresses miss. */
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
