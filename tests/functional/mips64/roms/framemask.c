/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * R10000 FrameMask masking of EntryLo during TLB writes
 * (User's Manual 2.0, 14.18).
 */

#include "rom.h"

const char rom_name[] = "framemask";

/* Test pages are above ROM data and stack. HIGH differs at PA bit 24. */
#define PHYS_LOW      0x00300000ull
#define PHYS_LOW_ODD  0x00301000ull
#define PHYS_HIGH     0x01300000ull
#define PHYS_HIGH_ODD 0x01301000ull

/* Kernel-mode addresses in the user segment exercise TLB translation. */
#define VIRT_WI   0x10000000ull
#define VIRT_CORR 0x12000000ull
#define VIRT_WR   0x18000000ull

#define MAP_RW (ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G)

/* IRIX software valid/modified bits occupy EntryLo[33:32]. */
#define SOFT_BITS (3ull << 32)

/* FrameMask[15:14] masks EntryLo[33:32]. */
#define FM_TOP 0xc000u
/* FrameMask[0] masks EntryLo[18], physical address bit 24. */
#define FM_BIT18 0x0001u

/* Access physical memory through the uncached segment. */
static volatile uint32_t *phys(uint64_t paddr)
{
    return (volatile uint32_t *)(unsigned long)(0xffffffffa0000000ull + paddr);
}

static uint32_t load32(uint64_t vaddr)
{
    uint32_t v;

    __asm__ __volatile__("lw %0, 0(%1)" : "=r"(v) : "r"(vaddr) : "memory");
    return v;
}

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

static void tlb_read_index(int index)
{
    rom_set_index(index);
    __asm__ __volatile__("tlbr" : : : "memory");
}

void rom_main(void)
{
    uint64_t lo0_low = rom_entrylo(PHYS_LOW, MAP_RW, ROM_CACHE_NONCOHERENT);
    uint64_t lo1_low = rom_entrylo(PHYS_LOW_ODD, MAP_RW, ROM_CACHE_NONCOHERENT);
    uint64_t lo0_high = rom_entrylo(PHYS_HIGH, MAP_RW, ROM_CACHE_NONCOHERENT);
    uint64_t lo1_high = rom_entrylo(PHYS_HIGH_ODD, MAP_RW,
                                    ROM_CACHE_NONCOHERENT);
    uint32_t probe;

    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_wired(0);
    rom_set_entryhi(0);

    /* Confirm the reset mask before testing writes. */
    rom_assert_eq(1, 0, rom_get_framemask());

    /* At reset, EntryLo software bits must survive a TLB write/read. */
    tlb_write_index(0, VIRT_WI, lo0_low | SOFT_BITS, lo1_low | SOFT_BITS);
    tlb_read_index(0);
    rom_assert_eq(2, lo0_low | SOFT_BITS, rom_get_entrylo0());
    rom_assert_eq(3, lo1_low | SOFT_BITS, rom_get_entrylo1());

    /* Masking occurs on TLB write. EntryLo registers retain the input. */
    rom_set_framemask(FM_TOP);
    tlb_write_index(0, VIRT_WI, lo0_low | SOFT_BITS, lo1_low | SOFT_BITS);
    rom_assert_eq(4, lo0_low | SOFT_BITS, rom_get_entrylo0());
    rom_assert_eq(5, lo1_low | SOFT_BITS, rom_get_entrylo1());
    tlb_read_index(0);
    rom_assert_eq(6, lo0_low, rom_get_entrylo0());
    rom_assert_eq(7, lo1_low, rom_get_entrylo1());

    /* Loads must reach the masked physical pages without an exception. */
    *phys(PHYS_LOW) = 0xf1a60000u;
    *phys(PHYS_LOW_ODD) = 0xf1a60001u;
    rom_exc_reset();
    rom_assert_eq(8, 0xf1a60000u, load32(VIRT_WI));
    rom_assert_eq(9, 0xf1a60001u, load32(VIRT_WI + 0x1000));
    rom_assert_eq(10, 0, rom_exc.count);

    /* FrameMask[0] redirects HIGH to LOW in the entry and translation. */
    *phys(PHYS_LOW) = 0x900d0000u;
    *phys(PHYS_HIGH) = 0xbad00000u;
    rom_set_framemask(FM_BIT18);
    tlb_write_index(1, VIRT_CORR, lo0_high, lo1_high);
    tlb_read_index(1);
    rom_assert_eq(11, lo0_low, rom_get_entrylo0());
    rom_assert_eq(12, lo1_low, rom_get_entrylo1());
    rom_exc_reset();
    rom_assert_eq(13, 0x900d0000u, load32(VIRT_CORR));
    rom_assert_eq(14, 0, rom_exc.count);

    /* A refill handler's TLBWR must apply the same mask as TLBWI. */
    *phys(PHYS_LOW) = 0x0dd10000u;
    rom_set_framemask(FM_TOP);
    rom_exc_reset();
    rom_exc.entrylo0 = lo0_low | SOFT_BITS;
    rom_exc.entrylo1 = lo1_low | SOFT_BITS;
    rom_exc.pagemask = ROM_PAGEMASK_4K;
    rom_exc.refill = 1;
    rom_assert_eq(15, 0x0dd10000u, load32(VIRT_WR));
    rom_assert_eq(16, 1, rom_exc.count);
    rom_exc.refill = 0;

    rom_set_entryhi(VIRT_WR);
    __asm__ __volatile__("tlbp" : : : "memory");
    probe = rom_get_index();
    rom_assert_eq(17, 0, probe & 0x80000000u);
    tlb_read_index(probe & 0x3f);
    rom_assert_eq(18, lo0_low, rom_get_entrylo0());

    rom_set_framemask(0);
    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_entryhi(0);
}
