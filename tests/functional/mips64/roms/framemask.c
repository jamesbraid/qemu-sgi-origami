/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check how the R10000 FrameMask register masks EntryLo address bits
 * during TLB writes. See the R10000 User's Manual version 2.0,
 * section 14.18.
 */

#include "rom.h"

const char rom_name[] = "framemask";

/*
 * Physical pages nothing else in a ROM uses: the harness keeps its data
 * below
 * physical 0x200000 and its stack just under that. The HIGH pair is the
 * LOW
 * pair with physical address bit 24 also set, still inside the
 * machine's
 * 64 Mbyte of node memory.
 */
#define PHYS_LOW      0x00300000ull
#define PHYS_LOW_ODD  0x00301000ull
#define PHYS_HIGH     0x01300000ull
#define PHYS_HIGH_ODD 0x01301000ull

/*
 * Virtual pages in the user segment, which is translated from kernel
 * mode.
 */
#define VIRT_WI   0x10000000ull
#define VIRT_CORR 0x12000000ull
#define VIRT_WR   0x18000000ull

#define MAP_RW (ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G)

/*
 * Where IRIX parks its software valid and modified bits: EntryLo 33:32,
 * physical address 39:38 if they were allowed through.
 */
#define SOFT_BITS (3ull << 32)

/*
 * FrameMask bits 15:14, masking EntryLo 33:32.
 */
#define FM_TOP 0xc000u
/*
 * FrameMask bit 0, masking EntryLo 18, physical address bit 24.
 */
#define FM_BIT18 0x0001u

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

    /*
     * The tests below lean on the reset value, so state it as a check.
     */
    rom_assert_eq(1, 0, rom_get_framemask());

    /*
     * With the mask at its reset value, every EntryLo bit passes to the
     * buffer, the parked software bits included: reading the entry back
     * returns them. This is what real hardware does before the mask is
     * programmed, and it is the half a model fails if it hardcodes the
     * masking instead of applying the register.
     */
    tlb_write_index(0, VIRT_WI, lo0_low | SOFT_BITS, lo1_low | SOFT_BITS);
    tlb_read_index(0);
    rom_assert_eq(2, lo0_low | SOFT_BITS, rom_get_entrylo0());
    rom_assert_eq(3, lo1_low | SOFT_BITS, rom_get_entrylo1());

    /*
     * With mask bits 15:14 set, the same indexed write stores the entry
     * with
     * EntryLo 33:32 forced to zero, and only those: reading it back
     * returns
     * the entry as if the software bits had never been written.
     *
     * The mask acts on the buffer write, not on the registers: right
     * after
     * the write the EntryLo registers still hold what was written,
     * software
     * bits included, and only the entry read back from the buffer has
     * them
     * stripped. A model that masked the register write instead would
     * pass
     * every other assertion in this ROM.
     */
    rom_set_framemask(FM_TOP);
    tlb_write_index(0, VIRT_WI, lo0_low | SOFT_BITS, lo1_low | SOFT_BITS);
    rom_assert_eq(4, lo0_low | SOFT_BITS, rom_get_entrylo0());
    rom_assert_eq(5, lo1_low | SOFT_BITS, rom_get_entrylo1());
    tlb_read_index(0);
    rom_assert_eq(6, lo0_low, rom_get_entrylo0());
    rom_assert_eq(7, lo1_low, rom_get_entrylo1());

    /*
     * The kernel's own pattern, end to end: a load through that mapping
     * must
     * reach the physical page the masked frame number names, with no
     * exception. Unmasked, the software bits become physical address
     * bits
     * 39:38 and the access leaves the node's memory entirely.
     */
    *phys(PHYS_LOW) = 0xf1a60000u;
    *phys(PHYS_LOW_ODD) = 0xf1a60001u;
    rom_exc_reset();
    rom_assert_eq(8, 0xf1a60000u, load32(VIRT_WI));
    rom_assert_eq(9, 0xf1a60001u, load32(VIRT_WI + 0x1000));
    rom_assert_eq(10, 0, rom_exc.count);

    /*
     * The other end of the mask field: bit 0 masks EntryLo 18, physical
     * address bit 24. An entry naming the HIGH page must store and
     * translate
     * as the LOW page — the load must return what was planted at the
     * masked
     * address, not at the address the write named.
     */
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

    /*
     * The manual names both write instructions, so the random write is
     * checked as well: a refill installed by the handler while the mask
     * is
     * programmed stores the masked entry, and the resumed load
     * completes
     * through it.
     */
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
