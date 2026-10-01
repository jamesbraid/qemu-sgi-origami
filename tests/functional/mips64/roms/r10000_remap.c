/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Indexed TLB replacement changes only the even half's uncached frame. */

#include "r10000-remap.h"

const char rom_name[] = "r10000_remap";

static void run_case(uint32_t mask, uint64_t odd_offset, int id)
{
    uint64_t old_lo = remap_lo(REMAP_OLD, ROM_CACHE_UNCACHED);
    uint64_t new_lo = remap_lo(REMAP_NEW, ROM_CACHE_UNCACHED);
    uint64_t odd_lo = remap_lo(REMAP_ODD, ROM_CACHE_UNCACHED);

    remap_park();
    *remap_direct(REMAP_OLD + REMAP_EVEN_OFFSET) = 0x11112222;
    *remap_direct(REMAP_NEW + REMAP_EVEN_OFFSET) = 0x33334444;
    *remap_direct(REMAP_ODD + REMAP_EVEN_OFFSET) = 0x55556666;

    remap_write(32, mask, REMAP_VA, old_lo, odd_lo);
    rom_assert_eq(id, mask, rom_get_pagemask());
    rom_assert_eq(id + 1, REMAP_VA, rom_get_entryhi());
    rom_assert_eq(id + 2, old_lo, rom_get_entrylo0());
    rom_assert_eq(id + 3, odd_lo, rom_get_entrylo1());
    rom_assert_eq(id + 4, 0x11112222,
                  *remap_mapped(REMAP_VA + REMAP_EVEN_OFFSET));
    rom_assert_eq(id + 5, 0x55556666,
                  *remap_mapped(REMAP_VA + odd_offset));

    remap_write(32, mask, REMAP_VA, new_lo, odd_lo);
    rom_assert_eq(id + 6, new_lo, rom_get_entrylo0());
    rom_assert_eq(id + 7, 0x33334444,
                  *remap_mapped(REMAP_VA + REMAP_EVEN_OFFSET));
    rom_assert_eq(id + 8, 0x55556666,
                  *remap_mapped(REMAP_VA + odd_offset));
    *remap_mapped(REMAP_VA + REMAP_EVEN_OFFSET) = 0x77778888;
    rom_assert_eq(id + 9, 0x77778888,
                  *remap_direct(REMAP_NEW + REMAP_EVEN_OFFSET));
    rom_assert_eq(id + 10, 0x11112222,
                  *remap_direct(REMAP_OLD + REMAP_EVEN_OFFSET));
    rom_assert_eq(id + 11, 0, rom_exc.count);
}

void rom_main(void)
{
    remap_setup();
    run_case(ROM_PAGEMASK_4K, 0x1200, 5);
    run_case(ROM_PAGEMASK_16K, 0x4200, 17);
}
