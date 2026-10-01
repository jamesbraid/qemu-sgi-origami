/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Replacing a large-page TLB entry must invalidate cached translations
 * across its full span. Each smallest-page quarter is read before and
 * after the replacement to expose stale mappings.
 */

#include "rom.h"

const char rom_name[] = "tlbspan";

/*
 * Physical pages nothing else in a ROM uses: the harness keeps its data
 * below physical 0x200000 and its stack just under that. Two 16 Kbyte
 * frames per mapping half, before and after the rewrite.
 */
#define PHYS_A0 0x00300000ull
#define PHYS_A1 0x00304000ull
#define PHYS_B0 0x00310000ull
#define PHYS_B1 0x00314000ull

/*
 * A 32 Kbyte-aligned user-segment address, translated from kernel mode.
 */
#define VIRT 0x10000000ull

#define MAP_RW (ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G)

#define QUARTER 0x1000
#define QUARTERS_PER_HALF 4

/*
 * The uncached window onto a physical address, for planting the
 * patterns.
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

static void write_entry(uint64_t lo0_frame, uint64_t lo1_frame)
{
    rom_set_pagemask(ROM_PAGEMASK_16K);
    rom_set_entryhi(VIRT);
    rom_set_entrylo0(rom_entrylo(lo0_frame, MAP_RW, ROM_CACHE_NONCOHERENT));
    rom_set_entrylo1(rom_entrylo(lo1_frame, MAP_RW, ROM_CACHE_NONCOHERENT));
    rom_set_index(0);
    __asm__ __volatile__("tlbwi" : : : "memory");
}

void rom_main(void)
{
    uint64_t frames[4] = { PHYS_A0, PHYS_A1, PHYS_B0, PHYS_B1 };
    int f, q;

    rom_exc_reset();
    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_wired(0);
    rom_set_entryhi(0);

    /*
     * Each quarter of each frame carries its frame and quarter number.
     */
    for (f = 0; f < 4; f++) {
        for (q = 0; q < QUARTERS_PER_HALF; q++) {
            *phys(frames[f] + q * QUARTER) = 0xf0 + f * 0x10 + q;
        }
    }

    /*
     * Map the pair onto the first frames and touch every quarter, so
     * the
     * emulator caches all eight smallest-page translations.
     */
    write_entry(PHYS_A0, PHYS_A1);
    for (q = 0; q < 2 * QUARTERS_PER_HALF; q++) {
        rom_assert_eq(1 + q, 0xf0 + (q / 4) * 0x10 + (q % 4),
                      load32(VIRT + q * QUARTER));
    }

    /*
     * Replace the entry: same virtual pair, new frames. Every cached
     * translation of the old span must go with it.
     */
    write_entry(PHYS_B0, PHYS_B1);
    for (q = 0; q < 2 * QUARTERS_PER_HALF; q++) {
        rom_assert_eq(9 + q, 0xf0 + (2 + q / 4) * 0x10 + (q % 4),
                      load32(VIRT + q * QUARTER));
    }

    rom_assert_eq(17, 0, rom_exc.count);
}
