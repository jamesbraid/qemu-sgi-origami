/* SPDX-License-Identifier: BSD-3-Clause */
/* Repeated indexed replacement of both halves of a mapped TLB entry. */

#include "rom.h"

const char rom_name[] = "tlbmaint";

#define VIRT 0x10000000ull
#define PHYS_A0 0x02000000ull
#define PHYS_A1 0x03000000ull
#define PHYS_B0 0x04000000ull
#define PHYS_B1 0x05000000ull
#define MAP_RW (ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G)

static uint32_t *physical(uint64_t addr)
{
    return (uint32_t *)(unsigned long)(0xffffffffa0000000ull + addr);
}

static uint32_t mapped(uint64_t addr)
{
    uint32_t value;

    __asm__ __volatile__("lw %0, 0(%1)" : "=r"(value) : "r"(addr) : "memory");
    return value;
}

static void write_entry(uint64_t even, uint64_t odd)
{
    rom_set_entrylo0(rom_entrylo(even, MAP_RW, ROM_CACHE_NONCOHERENT));
    rom_set_entrylo1(rom_entrylo(odd, MAP_RW, ROM_CACHE_NONCOHERENT));
    __asm__ __volatile__("tlbwi" : : : "memory");
}

void rom_main(void)
{
    const uint64_t frames[] = { PHYS_A0, PHYS_A1, PHYS_B0, PHYS_B1 };
    const uint64_t offsets[] = { 0, PAGE_BYTES / 2, PAGE_BYTES - 4 };
    unsigned i, frame, offset;

    rom_exc_reset();
    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_wired(0);
    for (frame = 0; frame < 4; frame++) {
        for (offset = 0; offset < 3; offset++) {
            /* Keep each uncached pattern store ahead of the mapped loads. */
            *(volatile uint32_t *)physical(frames[frame] + offsets[offset]) =
                0x100 + frame * 16 + offset;
        }
    }
    rom_set_pagemask(PAGE_MASK);
    rom_set_entryhi(VIRT);
    rom_set_index(0);

    for (i = 0; i < ITERATIONS; i++) {
        unsigned pair = (i & 1) * 2;

        write_entry(frames[pair], frames[pair + 1]);
        for (frame = 0; frame < 2; frame++) {
            for (offset = 0; offset < 3; offset++) {
                rom_assert_eq(1 + frame * 3 + offset,
                              0x100 + (pair + frame) * 16 + offset,
                              mapped(VIRT + frame * PAGE_BYTES +
                                     offsets[offset]));
            }
        }
    }
    rom_assert_eq(7, 0, rom_exc.count);
}
