/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * R10000 secondary-cache storage across a TLB PFN replacement.
 *
 * This control is built by the native sgi-roms target. The current
 * diagnostic cache arrays do not retain dirty data or write it back, so this
 * ROM is omitted from suite until the cache model implements that contract.
 * Expected results follow the R10000 User's Manual v2.0, pp. 187-200, 324.
 */

#include "r10000-remap.h"

const char rom_name[] = "r10000_cache_storage";

#define OLD_INDEX 0x9800000001000200ull
#define ODD_INDEX 0x9800000001004200ull
#define NEW_INDEX 0x9800000001800200ull
#define OLD_TAG 0x00100000u
#define NEW_TAG 0x00180000u
#define TAG_PA_MASK 0xffffc000u
#define TAG_STATE 0x00000c00u
#define TAG_PINDEX 0x00000180u

static void index_wbinv(uint64_t addr)
{
    __asm__ __volatile__("cache 0x03, 0(%0)" : : "r"(addr) : "memory");
}

static void hit_wbinv(uint64_t addr)
{
    __asm__ __volatile__("cache 0x17, 0(%0)" : : "r"(addr) : "memory");
}

static uint32_t index_tag(uint64_t addr)
{
    __asm__ __volatile__("cache 0x07, 0(%0)" : : "r"(addr) : "memory");
    return rom_get_taglo();
}

static void require_invalid(uint64_t addr, int id)
{
    rom_assert_eq(id, 0, index_tag(addr) & TAG_STATE);
    rom_assert_eq(id + 1, 0, index_tag(addr | 1) & TAG_STATE);
}

static int valid_tag(uint32_t tag, uint32_t pa_tag)
{
    return (tag & TAG_STATE) != 0 &&
           (tag & TAG_PA_MASK) == pa_tag &&
           (tag & TAG_PINDEX) == 0;
}

static void require_tags(int id, int old, int new)
{
    uint32_t way0 = index_tag(OLD_INDEX);
    uint32_t way1 = index_tag(OLD_INDEX | 1);
    int old_count = valid_tag(way0, OLD_TAG) + valid_tag(way1, OLD_TAG);
    int new_count = valid_tag(way0, NEW_TAG) + valid_tag(way1, NEW_TAG);

    rom_assert_eq(id, old, old_count);
    rom_assert_eq(id + 1, new, new_count);
}

static void run_case(uint32_t mask, uint64_t odd_offset, int id)
{
    uint64_t old_lo = remap_lo(REMAP_OLD, ROM_CACHE_NONCOHERENT);
    uint64_t new_lo = remap_lo(REMAP_NEW, ROM_CACHE_NONCOHERENT);
    uint64_t odd_lo = remap_lo(REMAP_ODD, ROM_CACHE_NONCOHERENT);

    remap_park();
    index_wbinv(OLD_INDEX);
    index_wbinv(OLD_INDEX | 1);
    index_wbinv(ODD_INDEX);
    index_wbinv(ODD_INDEX | 1);
    require_invalid(OLD_INDEX, id);
    require_invalid(ODD_INDEX, id + 2);

    *remap_direct(REMAP_OLD + REMAP_EVEN_OFFSET) = 0x11112222;
    *remap_direct(REMAP_NEW + REMAP_EVEN_OFFSET) = 0x33334444;
    *remap_direct(REMAP_ODD + REMAP_EVEN_OFFSET) = 0x55556666;

    remap_write(32, mask, REMAP_VA, old_lo, odd_lo);
    rom_assert_eq(id + 4, mask, rom_get_pagemask());
    rom_assert_eq(id + 5, old_lo, rom_get_entrylo0());
    rom_assert_eq(id + 6, 0x11112222,
                  *remap_mapped(REMAP_VA + REMAP_EVEN_OFFSET));
    rom_assert_eq(id + 7, 0x55556666,
                  *remap_mapped(REMAP_VA + odd_offset));
    *remap_mapped(REMAP_VA + REMAP_EVEN_OFFSET) = 0xa1b2c3d4;
    rom_assert_eq(id + 8, 0xa1b2c3d4,
                  *remap_mapped(REMAP_VA + REMAP_EVEN_OFFSET));
    require_tags(id + 9, 1, 0);

    remap_write(32, mask, REMAP_VA, new_lo, odd_lo);
    rom_assert_eq(id + 11, 0x33334444,
                  *remap_mapped(REMAP_VA + REMAP_EVEN_OFFSET));
    *remap_mapped(REMAP_VA + REMAP_EVEN_OFFSET) = 0xb1c2d3e4;
    rom_assert_eq(id + 12, 0xb1c2d3e4,
                  *remap_mapped(REMAP_VA + REMAP_EVEN_OFFSET));
    require_tags(id + 13, 1, 1);
    rom_assert_eq(id + 15, 0x55556666,
                  *remap_mapped(REMAP_VA + odd_offset));

    hit_wbinv(OLD_INDEX);
    hit_wbinv(NEW_INDEX);
    rom_assert_eq(id + 16, 0xa1b2c3d4,
                  *remap_direct(REMAP_OLD + REMAP_EVEN_OFFSET));
    rom_assert_eq(id + 17, 0xb1c2d3e4,
                  *remap_direct(REMAP_NEW + REMAP_EVEN_OFFSET));
    require_invalid(OLD_INDEX, id + 18);
    rom_assert_eq(id + 20, 0, rom_exc.count);
}

void rom_main(void)
{
    remap_setup();
    run_case(ROM_PAGEMASK_4K, 0x1200, 5);
    run_case(ROM_PAGEMASK_16K, 0x4200, 26);
}
