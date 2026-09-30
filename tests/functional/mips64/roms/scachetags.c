/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * R10000 secondary-cache index tag operations preserve the external
 * tag array's state. Loads return defined tag fields. Invalidation
 * updates matching tags and related primary-cache entries; rewritten
 * tag ECC is not asserted because its encoding is unpublished.
 *
 * Source: MIPS R10000 Microprocessor User's Manual, version 2.0,
 * sections 10.4, 10.7, 10.10, 10.13, 10.16 and the CACHE chapter.
 */

#include "rom.h"

const char rom_name[] = "scachetags";

#define SCACHE_TAGLO_MASK 0xffffcdffu
#define SCACHE_TAGHI_MASK 0x8000000fu

/*
 * Volatile keeps the memory accesses visible to this cache test.
 */
static volatile unsigned char scratch[256] __attribute__((aligned(128)));

#define KSEG0_VIEW(p) ((unsigned long)(p) & ~0x20000000ul)

/*
 * The secondary tag fields a kseg0 view of an address produces:
 * PA[35:18]
 * into TagLo[31:14], a state at TagLo[11:10], the virtual index
 * VA[13:12]
 * at TagLo[8:7].  PA[39:36] of anything kseg0 reaches is zero.
 */
#define STAG_LO(addr) ((uint32_t)((((addr) & 0x1ffffffful) >> 18) << 14))
#define SST(s)        ((uint32_t)(s) << 10)
#define SVIDX_OF(addr) ((uint32_t)((((addr) >> 12) & 3ul)) << 7)
#define SECC_MASK     0x7fu

/*
 * Primary tag pieces, for the subset-invalidation check.
 */
#define PTAG_LO(addr) ((uint32_t)((((addr) & 0x1ffffffful) >> 12) << 8))
#define PT_TP    (1u << 0)
#define PT_IVAL  (1u << 6)
#define PT_DSTATE(s) ((uint32_t)(s) << 6)

#define ICACHE_STORE_TAG(addr) \
    __asm__ __volatile__("cache 0x08, 0(%0)" : : "r"(addr) : "memory")
#define ICACHE_LOAD_TAG(addr) \
    __asm__ __volatile__("cache 0x04, 0(%0)" : : "r"(addr) : "memory")
#define DCACHE_STORE_TAG(addr) \
    __asm__ __volatile__("cache 0x09, 0(%0)" : : "r"(addr) : "memory")
#define DCACHE_LOAD_TAG(addr) \
    __asm__ __volatile__("cache 0x05, 0(%0)" : : "r"(addr) : "memory")

static inline void scache_index_wbinv(unsigned long addr)
{
    __asm__ __volatile__("cache 0x03, 0(%0)" : : "r"(addr) : "memory");
}

static inline void scache_hit_inv(unsigned long addr)
{
    __asm__ __volatile__("cache 0x13, 0(%0)" : : "r"(addr) : "memory");
}

static inline void scache_hit_wbinv(unsigned long addr)
{
    __asm__ __volatile__("cache 0x17, 0(%0)" : : "r"(addr) : "memory");
}

static inline void scache_store_tag(unsigned long addr)
{
    __asm__ __volatile__("cache 0x0b, 0(%0)" : : "r"(addr) : "memory");
}

static inline void scache_load_tag(unsigned long addr)
{
    __asm__ __volatile__("cache 0x07, 0(%0)" : : "r"(addr) : "memory");
}

static void store_tag(unsigned long addr, uint32_t lo, uint32_t hi)
{
    rom_set_taglo(lo);
    rom_set_taghi(hi);
    scache_store_tag(addr);
}

static void assert_tag(int id, unsigned long addr, uint32_t lo, uint32_t hi)
{
    rom_set_taglo(0);
    rom_set_taghi(0);
    scache_load_tag(addr);
    rom_assert_eq(id, lo, rom_get_taglo());
    rom_assert_eq(id + 1, hi, rom_get_taghi());
}

void rom_main(void)
{
    unsigned long set0 = KSEG0_VIEW(&scratch[0]);
    unsigned long set1 = KSEG0_VIEW(&scratch[128]);

    rom_exc_reset();

    /*
     * Each way at one set keeps an independent tag.
     */
    store_tag(set0, 0x55555555u, 0x00000005u);
    store_tag(set0 | 1, 0xaaaaaaaa, 0x8000000au);
    assert_tag(1, set0, 0x55554555u, 0x80000005u);
    assert_tag(3, set0 | 1, 0xaaaa88aau, 0x8000000au);

    /*
     * The neighboring set must not alias the first one.
     */
    store_tag(set1, 0xffffffffu, 0xffffffffu);
    assert_tag(5, set1, SCACHE_TAGLO_MASK, SCACHE_TAGHI_MASK);
    assert_tag(7, set0, 0x55554555u, 0x80000005u);

    /*
     * Writing either way changes the MRU bit reported for the whole
     * set.
     */
    store_tag(set0 | 1, 0x00000000u, 0x0000000au);
    assert_tag(9, set0, 0x55554555u, 0x00000005u);
    assert_tag(11, set0 | 1, 0x00000000u, 0x0000000au);

    /*
     * A Hit operation that misses — set zero holds one valid tag naming
     * some other address and one invalid way — clears the CH bit and
     * leaves
     * every tag alone.
     */
    rom_set_status(rom_get_status() | ROM_ST_CH);
    scache_hit_inv(set0);
    rom_assert_eq(13, 0, rom_get_status() & ROM_ST_CH);
    assert_tag(14, set0, 0x55554555u, 0x00000005u);

    /*
     * A hit sets CH and rewrites the hit way's whole tag entry: state
     * to
     * invalid, the tag from the operation's physical address, the
     * virtual
     * index from its VA[13:12], and the MRU bit pointed away from the
     * invalidated way. The other way is untouched.
     */
    store_tag(set1 | 1, STAG_LO(set1) | SST(2) | 0x2au, 0x00000000u);
    rom_set_status(rom_get_status() & ~ROM_ST_CH);
    scache_hit_inv(set1);
    rom_assert_eq(16, ROM_ST_CH, rom_get_status() & ROM_ST_CH);
    rom_set_taglo(0xffffffffu);
    rom_set_taghi(0xffffffffu);
    scache_load_tag(set1 | 1);
    rom_assert_eq(17, STAG_LO(set1) | SVIDX_OF(set1),
                  rom_get_taglo() & ~SECC_MASK);
    rom_assert_eq(18, 0, rom_get_taghi());
    assert_tag(19, set1, SCACHE_TAGLO_MASK, 0x0000000fu);

    /*
     * The invalidated way's tag now names this very address, but its
     * state
     * is invalid, so a second Hit operation misses and CH clears again.
     */
    rom_set_status(rom_get_status() | ROM_ST_CH);
    scache_hit_wbinv(set1);
    rom_assert_eq(21, 0, rom_get_status() & ROM_ST_CH);

    /*
     * Hit WriteBack Invalidate (S) hits and rewrites the same way.
     */
    store_tag(set0, STAG_LO(set0) | SST(1), 0x80000000u);
    scache_hit_wbinv(set0);
    rom_assert_eq(22, ROM_ST_CH, rom_get_status() & ROM_ST_CH);
    rom_set_taglo(0xffffffffu);
    rom_set_taghi(0xffffffffu);
    scache_load_tag(set0);
    rom_assert_eq(23, STAG_LO(set0) | SVIDX_OF(set0),
                  rom_get_taglo() & ~SECC_MASK);
    rom_assert_eq(24, 0x80000000u, rom_get_taghi());

    /*
     * Index WriteBack Invalidate (S) takes the way from address bit
     * zero
     * without a tag compare, rewrites the entry the same way, and moves
     * the
     * MRU bit away from the invalidated way. CH belongs to the Hit
     * forms
     * alone and stays put.
     */
    store_tag(set0 | 1, (0x2222u << 14) | SST(3), 0x80000000u);
    scache_index_wbinv(set0 | 1);
    rom_assert_eq(25, ROM_ST_CH, rom_get_status() & ROM_ST_CH);
    rom_set_taglo(0xffffffffu);
    rom_set_taghi(0xffffffffu);
    scache_load_tag(set0 | 1);
    rom_assert_eq(26, STAG_LO(set0) | SVIDX_OF(set0),
                  rom_get_taglo() & ~SECC_MASK);
    rom_assert_eq(27, 0, rom_get_taghi());

    /*
     * The same operation on a block already invalid does nothing at
     * all:
     * no tag rewrite, and the MRU bit — set by the plant below — does
     * not
     * move either, where an invalidation would have pointed it away.
     */
    store_tag(set0, 0x00000000u, 0x80000000u);
    scache_index_wbinv(set0 | 1);
    rom_set_taglo(0xffffffffu);
    rom_set_taghi(0xffffffffu);
    scache_load_tag(set0 | 1);
    rom_assert_eq(28, STAG_LO(set0) | SVIDX_OF(set0),
                  rom_get_taglo() & ~SECC_MASK);
    rom_assert_eq(29, 0x80000000u, rom_get_taghi());

    /*
     * Invalidating a secondary block extends to the primary blocks that
     * are
     * its subsets. The secondary tag's virtual index bits say where in
     * the
     * virtually indexed primary caches to look — planted here as this
     * address's own VA[13:12], so the primary tags planted for the same
     * address are found and invalidated with it.
     */
    rom_set_taglo(PTAG_LO(set1) | PT_IVAL | PT_TP);
    rom_set_taghi(0);
    ICACHE_STORE_TAG(set1);
    rom_set_taglo(PTAG_LO(set1) | PT_DSTATE(2) | PT_TP);
    rom_set_taghi(0);
    DCACHE_STORE_TAG(set1);
    store_tag(set1, STAG_LO(set1) | SST(1) | SVIDX_OF(set1), 0x00000000u);
    scache_hit_inv(set1);
    rom_assert_eq(30, ROM_ST_CH, rom_get_status() & ROM_ST_CH);
    rom_set_taglo(0xffffffffu);
    rom_set_taghi(0xffffffffu);
    ICACHE_LOAD_TAG(set1);
    rom_assert_eq(31, PTAG_LO(set1) | PT_TP, rom_get_taglo());
    rom_set_taglo(0xffffffffu);
    rom_set_taghi(0xffffffffu);
    DCACHE_LOAD_TAG(set1);
    rom_assert_eq(32, PTAG_LO(set1) | PT_TP, rom_get_taglo());
    rom_assert_eq(33, 1u << 29, rom_get_taghi());

    /*
     * The MRU bit lives in the set's single way-prediction entry - the
     * manual's secondary-cache chapter gives the prediction table one
     * bit
     * per pair of blocks - so a store whose TagHi[31] was set must not
     * leave a private copy behind in the way it addressed. Point the
     * set's
     * bit back away through the other way: the first way then reads
     * back
     * the set's bit, not the bit its own store carried.
     */
    store_tag(set0, STAG_LO(set0) | SST(1), 0x80000000u);
    store_tag(set0 | 1, 0, 0x00000000u);
    assert_tag(34, set0, STAG_LO(set0) | SST(1), 0x00000000u);

    rom_assert_eq(36, 0, rom_exc.count);
    rom_assert_eq(37, ~0ull, rom_exc.vector);
}
