/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * R10000 secondary-cache index data operations preserve diagnostic
 * data.
 *
 * Index Store Data (S) writes one doubleword from CP0 TagHi and TagLo
 * into a
 * secondary-cache quadword, pads the other doubleword with zeroes, and
 * stores
 * all ten bits of CP0 ECC.  Physical address bit three selects the
 * populated
 * doubleword and bit zero selects the cache way.  Index Load Data (S)
 * returns
 * the selected doubleword and the quadword's check bits.
 *
 * Oracle: MIPS R10000 Microprocessor User's Manual, version 2.0 of
 * January
 * 29, 1997, sections 10.19 and 10.22.
 */

#include "rom.h"

const char rom_name[] = "scachedata";

/*
 * Volatile keeps the memory accesses visible to this cache test.
 */
static volatile unsigned char scratch[256] __attribute__((aligned(128)));

#define KSEG0_VIEW(p) ((unsigned long)(p) & ~0x20000000ul)

static inline void scache_store_data(unsigned long addr)
{
    __asm__ __volatile__("cache 0x1f, 0(%0)" : : "r"(addr) : "memory");
}

static inline void scache_load_data(unsigned long addr)
{
    __asm__ __volatile__("cache 0x1b, 0(%0)" : : "r"(addr) : "memory");
}

static void store_data(unsigned long addr, uint32_t lo, uint32_t hi,
                       uint32_t ecc)
{
    rom_set_taglo(lo);
    rom_set_taghi(hi);
    rom_set_ecc(ecc);
    scache_store_data(addr);
}

static void assert_data(int id, unsigned long addr, uint32_t lo, uint32_t hi,
                        uint32_t ecc)
{
    rom_set_taglo(0xffffffffu);
    rom_set_taghi(0xffffffffu);
    rom_set_ecc(0x3ffu);
    scache_load_data(addr);
    rom_assert_eq(id, lo, rom_get_taglo());
    rom_assert_eq(id + 1, hi, rom_get_taghi());
    rom_assert_eq(id + 2, ecc, rom_get_ecc());
}

void rom_main(void)
{
    unsigned long set0 = KSEG0_VIEW(&scratch[0]);
    unsigned long set1 = KSEG0_VIEW(&scratch[128]);

    rom_exc_reset();

    /*
     * A lower-doubleword write pads the upper half of its quadword.
     */
    store_data(set0, 0x01234567u, 0x89abcdefu, 0x155u);
    assert_data(1, set0, 0x01234567u, 0x89abcdefu, 0x155u);
    assert_data(4, set0 + 8, 0, 0, 0x155u);

    /*
     * An upper-doubleword write pads the lower half instead.
     */
    store_data(set0 + 8, 0xfedcba98u, 0x76543210u, 0x2aau);
    assert_data(7, set0 + 8, 0xfedcba98u, 0x76543210u, 0x2aau);
    assert_data(10, set0, 0, 0, 0x2aau);

    /*
     * The other way and a neighboring set have independent data arrays.
     */
    store_data(set0 | 1, 0xaaaaaaaau, 0x55555555u, 0x3ffu);
    store_data(set1, 0x33333333u, 0xccccccccu, 0x001u);
    assert_data(13, set0 | 1, 0xaaaaaaaau, 0x55555555u, 0x3ffu);
    assert_data(16, set1, 0x33333333u, 0xccccccccu, 0x001u);
    assert_data(19, set0, 0, 0, 0x2aau);

    rom_assert_eq(22, 0, rom_exc.count);
    rom_assert_eq(23, ~0ull, rom_exc.vector);
}
