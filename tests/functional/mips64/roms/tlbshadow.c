/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test displaced TLB entries: a live translation takes precedence,
 * and rewriting an entry removes its displaced copies.
 */

#include "rom.h"

const char rom_name[] = "tlbshadow";

/*
 * Physical frames nothing else in a ROM uses: the harness keeps its
 * data
 * below physical 0x200000 and its stack just under that.
 */
#define PHYS_A 0x00320000ull
#define PHYS_B 0x00324000ull
#define PHYS_C 0x00328000ull
#define PHYS_D 0x0032c000ull
/*
 * A 64 Kbyte-aligned frame, for the differing-page-size leg.
 */
#define PHYS_E 0x00400000ull

#define PAT_A 0xa1
#define PAT_B 0xb2
#define PAT_C 0xc3
#define PAT_D 0xd4
#define PAT_E 0xe5

/*
 * Two 8 Kbyte-aligned user-segment addresses, translated from kernel
 * mode.
 */
#define VIRT  0x11000000ull
#define VIRT2 0x11002000ull

/*
 * The differing-page-size leg. A 64 Kbyte pair covers 128 Kbytes from
 * its
 * base; the small mapping sits inside it but away from that base, which
 * is where a coverage test that masks at only one of the two page sizes
 * gets the wrong answer -- the two page numbers agree only once both
 * masks are taken into account.
 */
#define LARGE_VBASE 0x11800000ull
#define SMALL_IN    (LARGE_VBASE + 0x8000ull)

#define MAP_RW (ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G)

/*
 * How many entries this processor's TLB has. The suite's constant is
 * the
 * largest any model here carries, and a smaller TLB keeps Wired modulo
 * its own size, so a readback below what was written names the size.
 */
static int tlb_entries(void)
{
    unsigned int probe = ROM_TLB_ENTRIES - 1;
    unsigned int back;

    rom_set_wired(probe);
    back = rom_get_wired();
    rom_set_wired(0);
    return back == probe ? ROM_TLB_ENTRIES : (int)(probe - back);
}

/*
 * The ROM runs with the boot-exception-vector bit set.
 */
#define VEC_BOOTSTRAP_REFILL (ROM_VEC_BOOTSTRAP | ROM_VEC_TLB_REFILL)

/*
 * Volatile preserves accesses through the uncached physical-memory
 * mapping.
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

/*
 * Stage an even-page mapping; the odd half stays invalid throughout.
 */
static void stage(uint64_t vaddr, uint64_t frame)
{
    rom_set_pagemask(ROM_PAGEMASK_4K);
    rom_set_entryhi(vaddr);
    rom_set_entrylo0(rom_entrylo(frame, MAP_RW, ROM_CACHE_NONCOHERENT));
    rom_set_entrylo1(0);
}

/*
 * Stage a 64 Kbyte even-page mapping; the odd half stays invalid.
 */
static void stage_large(uint64_t vaddr, uint64_t frame)
{
    rom_set_pagemask(ROM_PAGEMASK_64K);
    rom_set_entryhi(vaddr);
    rom_set_entrylo0(rom_entrylo(frame, MAP_RW, ROM_CACHE_NONCOHERENT));
    rom_set_entrylo1(0);
}

static void write_indexed(uint64_t vaddr, uint64_t frame, int index)
{
    stage(vaddr, frame);
    rom_set_index(index);
    __asm__ __volatile__("tlbwi" : : : "memory");
}

static void write_random(uint64_t vaddr, uint64_t frame)
{
    stage(vaddr, frame);
    __asm__ __volatile__("tlbwr" : : : "memory");
}

void rom_main(void)
{
    int last;

    rom_exc_reset();
    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_entryhi(0);

    *phys(PHYS_A) = PAT_A;
    *phys(PHYS_B) = PAT_B;
    *phys(PHYS_C) = PAT_C;
    *phys(PHYS_D) = PAT_D;

    /*
     * Pin the victim. With every entry below the last one wired, the
     * random register has a single value left to give.
     */
    last = tlb_entries() - 1;
    rom_set_wired(last);
    rom_assert_eq(1, last, rom_get_random());

    /*
     * Map the page at the victim index and cache its translation.
     */
    write_indexed(VIRT, PHYS_A, last);
    rom_assert_eq(2, PAT_A, load32(VIRT));

    /*
     * Displace it with an unrelated page.
     */
    write_random(VIRT2, PHYS_B);
    rom_assert_eq(3, PAT_B, load32(VIRT2));

    /*
     * Map the original page again, at the same index, onto a new frame.
     * The architectural entry must answer; the displaced copy of the
     * old
     * mapping must not.
     */
    write_random(VIRT, PHYS_C);
    rom_assert_eq(4, PAT_C, load32(VIRT));

    /*
     * Now rewrite that entry to a different page. Nothing maps the
     * original page any more, so a read of it must fault -- neither the
     * entry, which now names another page, nor any displaced copy of
     * the
     * earlier mapping may answer.
     */
    write_indexed(VIRT2, PHYS_D, last);
    rom_assert_eq(5, PAT_D, load32(VIRT2));

    rom_exc_reset();
    ROM_TRY("lw $27, 0(%[addr])", [addr] "r"(VIRT));
    rom_assert_eq(6, 1, rom_exc.count);
    /*
     * A miss with nothing matching arrives at a refill entry point. An
     * entry that matched but was unusable would arrive at the general
     * one instead, so this is what distinguishes "no mapping at all"
     * from "mapping present but invalid" -- and only the former is what
     * the guest asked for here.
     */
    rom_assert_eq(7, VEC_BOOTSTRAP_REFILL, rom_exc.vector);
    rom_assert_eq(8, VIRT, rom_exc.badvaddr);

    /*
     * And the page the entry does name still reads through it.
     */
    rom_exc_reset();
    rom_assert_eq(9, PAT_D, load32(VIRT2));
    rom_assert_eq(10, 0, rom_exc.count);

    /*
     * The same contract when the two mappings have different page
     * sizes.
     * Displace a small mapping into a shadow, then write a large
     * mapping
     * that covers it -- with the small page away from the large one's
     * base, so a coverage test masking at the small page's granularity
     * alone sees two different page numbers and misses it. The large
     * mapping must answer.
     */
    *phys(PHYS_E + 0x8000) = PAT_E;

    write_indexed(SMALL_IN, PHYS_A, last);
    rom_assert_eq(11, PAT_A, load32(SMALL_IN));

    write_random(VIRT2, PHYS_B);
    stage_large(LARGE_VBASE, PHYS_E);
    __asm__ __volatile__("tlbwr" : : : "memory");

    rom_assert_eq(12, PAT_E, load32(SMALL_IN));
    rom_assert_eq(13, 0, rom_exc.count);
}
