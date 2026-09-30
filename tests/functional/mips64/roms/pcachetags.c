/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * R10000 primary-cache index tag operations preserve each cache's tag
 * array. Loads return defined TagLo and TagHi fields; invalidation
 * changes state fields while retaining the tag address.
 *
 * Source: MIPS R10000 Microprocessor User's Manual, version 2.0,
 * sections 10.2, 10.3, 10.5, 10.6, 10.8, 10.9, 10.11, 10.12, 10.15
 * and 14.23.
 */

#include "rom.h"

const char rom_name[] = "pcachetags";

#define ICACHE_TAGLO_MASK 0xffffff4du
#define ICACHE_TAGHI_MASK 0x0000000fu
#define DCACHE_TAGLO_MASK 0xffffffcfu
#define DCACHE_TAGHI_MASK 0xe000000fu

/*
 * Volatile keeps the memory accesses visible to this cache test.
 */
static volatile unsigned char scratch[256] __attribute__((aligned(128)));

#define KSEG0_VIEW(p) ((unsigned long)(p) & ~0x20000000ul)

#define ICACHE_STORE_TAG(addr) \
    __asm__ __volatile__("cache 0x08, 0(%0)" : : "r"(addr) : "memory")
#define ICACHE_LOAD_TAG(addr) \
    __asm__ __volatile__("cache 0x04, 0(%0)" : : "r"(addr) : "memory")
#define DCACHE_STORE_TAG(addr) \
    __asm__ __volatile__("cache 0x09, 0(%0)" : : "r"(addr) : "memory")
#define DCACHE_LOAD_TAG(addr) \
    __asm__ __volatile__("cache 0x05, 0(%0)" : : "r"(addr) : "memory")
#define ICACHE_INDEX_INV(addr) \
    __asm__ __volatile__("cache 0x00, 0(%0)" : : "r"(addr) : "memory")
#define ICACHE_HIT_INV(addr) \
    __asm__ __volatile__("cache 0x10, 0(%0)" : : "r"(addr) : "memory")
#define DCACHE_INDEX_WBINV(addr) \
    __asm__ __volatile__("cache 0x01, 0(%0)" : : "r"(addr) : "memory")
#define DCACHE_HIT_INV(addr) \
    __asm__ __volatile__("cache 0x11, 0(%0)" : : "r"(addr) : "memory")
#define DCACHE_HIT_WBINV(addr) \
    __asm__ __volatile__("cache 0x15, 0(%0)" : : "r"(addr) : "memory")

/*
 * The tag-address field a primary tag carries for a kseg0 view:
 * PA[35:12]
 * into TagLo[31:8].  PA[39:36] of anything kseg0 can reach is zero.
 */
#define PTAG_LO(addr) ((uint32_t)((((addr) & 0x1ffffffful) >> 12) << 8))

/*
 * Primary tag field bits shared by both caches' TagLo formats.
 */
#define PT_TP    (1u << 0)
#define PT_SP    (1u << 2)
#define PT_LRU   (1u << 3)
#define PT_IVAL  (1u << 6)  /* instruction cache state: valid */
#define PT_DSTATE(s) ((uint32_t)(s) << 6)
#define PT_SCWAY (1u << 1)
#define PT_STMOD(m) ((uint32_t)(m) << 29)

static void set_tags(uint32_t lo, uint32_t hi)
{
    rom_set_taglo(lo);
    rom_set_taghi(hi);
}

/*
 * Poison both staging registers so a load-back that skips a bit shows
 * it.
 */
static void poison_tags(void)
{
    set_tags(0xdeadbeefu, 0xdeadbeefu);
}

void rom_main(void)
{
    unsigned long iset0 = KSEG0_VIEW(&scratch[0]);
    unsigned long iset1 = KSEG0_VIEW(&scratch[64]);
    unsigned long dset0 = KSEG0_VIEW(&scratch[0]);
    unsigned long dset1 = KSEG0_VIEW(&scratch[32]);

    rom_exc_reset();

    /*
     * Instruction cache: each way of one set keeps its own tag, every
     * defined bit round-trips, and every reserved bit reads back zero
     * however hard it was written.
     */
    set_tags(0xffffffffu, 0xffffffffu);
    ICACHE_STORE_TAG(iset0);
    set_tags(0x00000000u, 0x00000000u);
    ICACHE_STORE_TAG(iset0 | 1);
    poison_tags();
    ICACHE_LOAD_TAG(iset0);
    /*
     * Way 1's store cleared the set's shared least-recently-used bit.
     */
    rom_assert_eq(1, ICACHE_TAGLO_MASK & ~(1u << 3), rom_get_taglo());
    rom_assert_eq(2, ICACHE_TAGHI_MASK, rom_get_taghi());
    poison_tags();
    ICACHE_LOAD_TAG(iset0 | 1);
    rom_assert_eq(3, 0, rom_get_taglo());
    rom_assert_eq(4, 0, rom_get_taghi());

    /*
     * Writing either way moves the set-wide least-recently-used bit.
     */
    set_tags(1u << 3, 0);
    ICACHE_STORE_TAG(iset0 | 1);
    poison_tags();
    ICACHE_LOAD_TAG(iset0);
    rom_assert_eq(5, (ICACHE_TAGLO_MASK & ~(1u << 3)) | (1u << 3),
                  rom_get_taglo());

    /*
     * The neighboring set holds its own tag and does not alias set
     * zero.
     */
    set_tags(0x55555555u, 0x55555555u);
    ICACHE_STORE_TAG(iset1);
    poison_tags();
    ICACHE_LOAD_TAG(iset1);
    rom_assert_eq(6, 0x55555555u & ICACHE_TAGLO_MASK, rom_get_taglo());
    rom_assert_eq(7, 0x55555555u & ICACHE_TAGHI_MASK, rom_get_taghi());
    poison_tags();
    ICACHE_LOAD_TAG(iset0);
    rom_assert_eq(8, ICACHE_TAGLO_MASK, rom_get_taglo());

    /*
     * Data cache: same shape, its own field set, and an array
     * independent
     * of the instruction cache's — the address both caches were given
     * is
     * the same one.
     */
    set_tags(0xffffffffu, 0xffffffffu);
    DCACHE_STORE_TAG(dset0);
    set_tags(0x00000000u, 0x00000000u);
    DCACHE_STORE_TAG(dset0 | 1);
    poison_tags();
    DCACHE_LOAD_TAG(dset0);
    rom_assert_eq(9, DCACHE_TAGLO_MASK & ~(1u << 3), rom_get_taglo());
    rom_assert_eq(10, DCACHE_TAGHI_MASK, rom_get_taghi());
    poison_tags();
    DCACHE_LOAD_TAG(dset0 | 1);
    rom_assert_eq(11, 0, rom_get_taglo());
    rom_assert_eq(12, 0, rom_get_taghi());

    set_tags(1u << 3, 0);
    DCACHE_STORE_TAG(dset0 | 1);
    poison_tags();
    DCACHE_LOAD_TAG(dset0);
    rom_assert_eq(13, (DCACHE_TAGLO_MASK & ~(1u << 3)) | (1u << 3),
                  rom_get_taglo());

    set_tags(0xaaaaaaaau, 0xaaaaaaaau);
    DCACHE_STORE_TAG(dset1);
    poison_tags();
    DCACHE_LOAD_TAG(dset1);
    rom_assert_eq(14, 0xaaaaaaaau & DCACHE_TAGLO_MASK, rom_get_taglo());
    rom_assert_eq(15, 0xaaaaaaaau & DCACHE_TAGHI_MASK, rom_get_taghi());

    /*
     * The instruction cache set at the shared address kept its own tag.
     */
    poison_tags();
    ICACHE_LOAD_TAG(iset0);
    rom_assert_eq(16, ICACHE_TAGLO_MASK, rom_get_taglo());

    /*
     * Tag operations move tag parity inside TagLo — "Tag ECC and parity
     * are
     * loaded to and stored from the TagLo register" — so the ECC
     * register
     * stays out of them entirely.
     */
    rom_set_ecc(0x2aau);
    set_tags(0, 0);
    ICACHE_STORE_TAG(iset0);
    poison_tags();
    ICACHE_LOAD_TAG(iset0);
    rom_assert_eq(17, 0x2aau, rom_get_ecc());

    /*
     * Index Invalidate (I) clears the addressed way's state and state
     * parity, and nothing else: the tag address and tag parity survive,
     * the
     * set's least-recently-used bit does not move, and the other way is
     * not
     * touched.
     */
    set_tags(PTAG_LO(iset0) | PT_IVAL | PT_SP | PT_LRU | PT_TP, 0);
    ICACHE_STORE_TAG(iset0);
    set_tags(0x00004500u | PT_IVAL | PT_LRU, 0);
    ICACHE_STORE_TAG(iset0 | 1);
    ICACHE_INDEX_INV(iset0);
    poison_tags();
    ICACHE_LOAD_TAG(iset0);
    rom_assert_eq(18, PTAG_LO(iset0) | PT_LRU | PT_TP, rom_get_taglo());
    poison_tags();
    ICACHE_LOAD_TAG(iset0 | 1);
    rom_assert_eq(19, 0x00004500u | PT_IVAL | PT_LRU, rom_get_taglo());

    /*
     * Hit Invalidate (I) compares the physical address against both
     * ways: a
     * valid way whose tag matches is invalidated, a way holding a
     * different
     * address is left alone. Way one holds a tag one page away, so only
     * way
     * zero can hit.
     */
    set_tags(PTAG_LO(iset0) | PT_IVAL | PT_SP | PT_TP, 0);
    ICACHE_STORE_TAG(iset0);
    set_tags(PTAG_LO(iset0 + 0x1000) | PT_IVAL | PT_TP, 0);
    ICACHE_STORE_TAG(iset0 | 1);
    ICACHE_HIT_INV(iset0);
    poison_tags();
    ICACHE_LOAD_TAG(iset0);
    rom_assert_eq(20, PTAG_LO(iset0) | PT_TP, rom_get_taglo());
    poison_tags();
    ICACHE_LOAD_TAG(iset0 | 1);
    rom_assert_eq(21, PTAG_LO(iset0 + 0x1000) | PT_IVAL | PT_TP,
                  rom_get_taglo());

    /*
     * A second hit operation misses everywhere and changes nothing.
     */
    ICACHE_HIT_INV(iset0);
    poison_tags();
    ICACHE_LOAD_TAG(iset0 | 1);
    rom_assert_eq(22, PTAG_LO(iset0 + 0x1000) | PT_IVAL | PT_TP,
                  rom_get_taglo());

    /*
     * Index WriteBack Invalidate (D) rewrites the state side of the tag
     * —
     * state to invalid, secondary-way bit to zero, state modifier back
     * to
     * normal, state parity to zero — and keeps the tag address, the tag
     * parity and the least-recently-used bit.
     */
    set_tags(PTAG_LO(dset1) | PT_DSTATE(3) | PT_SP | PT_SCWAY | PT_TP,
             PT_STMOD(2) | 0x5u);
    DCACHE_STORE_TAG(dset1);
    DCACHE_INDEX_WBINV(dset1);
    poison_tags();
    DCACHE_LOAD_TAG(dset1);
    rom_assert_eq(23, PTAG_LO(dset1) | PT_TP, rom_get_taglo());
    rom_assert_eq(24, PT_STMOD(1) | 0x5u, rom_get_taghi());

    /*
     * Hit Invalidate (D) and Hit WriteBack Invalidate (D) leave the
     * same
     * state behind on a hit, and a mismatched way keeps everything.
     */
    set_tags(PTAG_LO(dset0) | PT_DSTATE(2) | PT_SP | PT_SCWAY | PT_TP,
             PT_STMOD(2));
    DCACHE_STORE_TAG(dset0);
    set_tags(PTAG_LO(dset0 + 0x1000) | PT_DSTATE(3) | PT_TP, PT_STMOD(1));
    DCACHE_STORE_TAG(dset0 | 1);
    DCACHE_HIT_INV(dset0);
    poison_tags();
    DCACHE_LOAD_TAG(dset0);
    rom_assert_eq(25, PTAG_LO(dset0) | PT_TP, rom_get_taglo());
    rom_assert_eq(26, PT_STMOD(1), rom_get_taghi());
    poison_tags();
    DCACHE_LOAD_TAG(dset0 | 1);
    rom_assert_eq(27, PTAG_LO(dset0 + 0x1000) | PT_DSTATE(3) | PT_TP,
                  rom_get_taglo());

    /*
     * The tag's upper-nibble address bits must be zero to match kseg0.
     */
    set_tags(PTAG_LO(dset1) | PT_DSTATE(3) | PT_SP | PT_SCWAY | PT_TP,
             PT_STMOD(2));
    DCACHE_STORE_TAG(dset1);
    DCACHE_HIT_WBINV(dset1);
    poison_tags();
    DCACHE_LOAD_TAG(dset1);
    rom_assert_eq(28, PTAG_LO(dset1) | PT_TP, rom_get_taglo());
    rom_assert_eq(29, PT_STMOD(1), rom_get_taghi());

    rom_assert_eq(30, 0, rom_exc.count);
    rom_assert_eq(31, ~0ull, rom_exc.vector);
}
