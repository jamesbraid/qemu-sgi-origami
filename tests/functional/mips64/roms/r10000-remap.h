/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef R10000_REMAP_H
#define R10000_REMAP_H

#include "rom.h"

#define REMAP_VA 0x40000000ull
#define REMAP_OLD 0x01000000ull
#define REMAP_NEW 0x01800000ull
#define REMAP_ODD 0x01004000ull
#define REMAP_EVEN_OFFSET 0x200ull
#define REMAP_FLAGS (ROM_ENTRYLO_G | ROM_ENTRYLO_V | ROM_ENTRYLO_D)

/* Volatile forces real loads and stores across translation changes. */
static inline volatile uint32_t *remap_direct(uint64_t pa)
{
    /* The uncached control access must reach memory every time. */
    return (volatile uint32_t *)(unsigned long)(0xffffffffa0000000ull + pa);
}

/* Each mapped probe must use the current TLB translation. */
static inline volatile uint32_t *remap_mapped(uint64_t va)
{
    /* Repeated virtual addresses can name different physical words. */
    return (volatile uint32_t *)(unsigned long)va;
}

/*
 * R10000 User's Manual v2.0 p. 291 forbids memory and TLB operations
 * immediately beside DMTC0 when changing translation registers.
 */
static void remap_write(uint32_t index, uint32_t mask, uint64_t hi,
                        uint64_t lo0, uint64_t lo1)
{
    __asm__ __volatile__(
        ".set push\n\t.set noreorder\n\t"
        "mtc0 %[index], $0\n\t"
        "mtc0 %[mask], $5\n\t"
        "nop\n\t"
        "dmtc0 %[hi], $10\n\t"
        "nop\n\t"
        "nop\n\t"
        "dmtc0 %[lo0], $2\n\t"
        "nop\n\t"
        "nop\n\t"
        "dmtc0 %[lo1], $3\n\t"
        "nop\n\t"
        "dmfc0 $0, $10\n\t"
        "tlbwi\n\t"
        "dla $8, 1f\n\t"
        "jr $8\n\t"
        "nop\n\t"
        "1:\n\t.set pop"
        :
        : [index] "r"(index), [mask] "r"(mask), [hi] "r"(hi),
          [lo0] "r"(lo0), [lo1] "r"(lo1)
        : "$8", "memory");
}

static void remap_park(void)
{
    int i;

    for (i = 0; i < ROM_TLB_ENTRIES; i++) {
        remap_write(i, ROM_PAGEMASK_4K,
                    ROM_TLB_PARK_BASE + (uint64_t)i * 0x2000, 0, 0);
    }
}

static void remap_setup(void)
{
    rom_exc_reset();
    rom_set_status(ROM_ST_BEV | ROM_ST_KX);
    rom_set_framemask(0);
    rom_set_wired(0);
    remap_park();
    rom_assert_eq(1, ROM_ST_BEV | ROM_ST_KX, rom_get_status());
    rom_assert_eq(2, 0, rom_get_framemask());
    rom_assert_eq(3, 0, rom_get_pagemask());
    rom_assert_eq(4, 0, rom_exc.count);
}

static uint64_t remap_lo(uint64_t pa, int cache)
{
    return rom_entrylo_in(pa, REMAP_FLAGS, cache,
                          cache == ROM_CACHE_UNCACHED ? ROM_UC_MEMORY : 0);
}

#endif
