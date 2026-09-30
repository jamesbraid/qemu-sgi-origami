/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * R10000 primary-cache index data operations
 * (User's Manual 2.0, 10.17-10.21, 14.21, 14.23).
 */

#include "rom.h"

const char rom_name[] = "pcachedata";

static volatile unsigned char scratch[256] __attribute__((aligned(128)));

#define KSEG0_VIEW(p) ((unsigned long)(p) & ~0x20000000ul)

#define ICACHE_STORE_DATA(addr) \
    __asm__ __volatile__("cache 0x1c, 0(%0)" : : "r"(addr) : "memory")
#define ICACHE_LOAD_DATA(addr) \
    __asm__ __volatile__("cache 0x18, 0(%0)" : : "r"(addr) : "memory")
#define DCACHE_STORE_DATA(addr) \
    __asm__ __volatile__("cache 0x1d, 0(%0)" : : "r"(addr) : "memory")
#define DCACHE_LOAD_DATA(addr) \
    __asm__ __volatile__("cache 0x19, 0(%0)" : : "r"(addr) : "memory")

static void stage(uint32_t lo, uint32_t hi, uint32_t ecc)
{
    rom_set_taglo(lo);
    rom_set_taghi(hi);
    rom_set_ecc(ecc);
}

void rom_main(void)
{
    unsigned long word0 = KSEG0_VIEW(&scratch[0]);
    unsigned long word1 = KSEG0_VIEW(&scratch[4]);

    rom_exc_reset();

    /* I-cache data preserves the 36-bit instruction and parity. */
    stage(0x01234567u, 0xffffffffu, 0x3ffu);
    ICACHE_STORE_DATA(word0);
    stage(0xdeadbeefu, 0xdeadbeefu, 0x2aau);
    ICACHE_LOAD_DATA(word0);
    rom_assert_eq(1, 0x01234567u, rom_get_taglo());
    rom_assert_eq(2, 0x0000000fu, rom_get_taghi());
    rom_assert_eq(3, 0x1u, rom_get_ecc());

    stage(0x89abcdefu, 0x5u, 0x3feu);
    ICACHE_STORE_DATA(word0 | 1);
    stage(0xdeadbeefu, 0xdeadbeefu, 0x3ffu);
    ICACHE_LOAD_DATA(word0 | 1);
    rom_assert_eq(4, 0x89abcdefu, rom_get_taglo());
    rom_assert_eq(5, 0x5u, rom_get_taghi());
    rom_assert_eq(6, 0, rom_get_ecc());

    /* Adjacent words occupy independent entries. */
    stage(0x76543210u, 0xau, 0x1u);
    ICACHE_STORE_DATA(word1);
    stage(0, 0, 0);
    ICACHE_LOAD_DATA(word1);
    rom_assert_eq(7, 0x76543210u, rom_get_taglo());
    stage(0, 0, 0);
    ICACHE_LOAD_DATA(word0);
    rom_assert_eq(8, 0x01234567u, rom_get_taglo());
    rom_assert_eq(9, 0x0000000fu, rom_get_taghi());

    /* D-cache data and byte parity round-trip independently. */
    stage(0xcafebabeu, 0, 0x3f5u);
    DCACHE_STORE_DATA(word0);
    stage(0xdeadbeefu, 0, 0x3ffu);
    DCACHE_LOAD_DATA(word0);
    rom_assert_eq(10, 0xcafebabeu, rom_get_taglo());
    rom_assert_eq(11, 0x5u, rom_get_ecc());

    stage(0x00000000u, 0, 0x00au);
    DCACHE_STORE_DATA(word0 | 1);
    stage(0xdeadbeefu, 0, 0x3ffu);
    DCACHE_LOAD_DATA(word0 | 1);
    rom_assert_eq(12, 0, rom_get_taglo());
    rom_assert_eq(13, 0xau, rom_get_ecc());

    stage(0x0badf00du, 0, 0);
    DCACHE_STORE_DATA(word1);
    stage(0, 0, 0x3ffu);
    DCACHE_LOAD_DATA(word1);
    rom_assert_eq(14, 0x0badf00du, rom_get_taglo());
    rom_assert_eq(15, 0, rom_get_ecc());

    /* D-cache operations leave the I-cache entry untouched. */
    stage(0, 0, 0);
    ICACHE_LOAD_DATA(word0);
    rom_assert_eq(16, 0x01234567u, rom_get_taglo());

    rom_assert_eq(17, 0, rom_exc.count);
    rom_assert_eq(18, ~0ull, rom_exc.vector);
}
