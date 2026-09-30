/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check that all defined R10000 CACHE operation and cache combinations
 * execute.
 */

#include "rom.h"

const char rom_name[] = "cacheops";

/* Align scratch to the largest supported secondary-cache block. */
static volatile unsigned char scratch[256] __attribute__((aligned(128)));

/* Cached alias of the scratch physical address. */
#define KSEG0_VIEW(p) ((unsigned long)(p) & ~0x20000000ul)

/* Execute one encoded CACHE operation and check the exception record. */
#define CACHE_OK(id, op)                                                      \
    do {                                                                      \
        __asm__ __volatile__("cache " #op ", 0(%0)"                           \
                             : : "r"(addr) : "memory");                       \
        rom_assert_eq((id), 0, rom_exc.count);                                \
    } while (0)

void rom_main(void)
{
    unsigned long addr = KSEG0_VIEW(&scratch[0]);

    rom_exc_reset();

    /*
     * Primary instruction cache: index invalidate, index load tag,
     * index
     * store
     * tag, hit invalidate, cache barrier, index load data, index store
     * data.
     */
    CACHE_OK(1, 0x00);
    CACHE_OK(2, 0x04);
    CACHE_OK(3, 0x08);
    CACHE_OK(4, 0x10);
    CACHE_OK(5, 0x14);
    CACHE_OK(6, 0x18);
    CACHE_OK(7, 0x1c);

    /*
     * Primary data cache: index writeback invalidate, index load tag,
     * index
     * store tag, hit invalidate, hit writeback invalidate, index load
     * data,
     * index store data.
     */
    CACHE_OK(8, 0x01);
    CACHE_OK(9, 0x05);
    CACHE_OK(10, 0x09);
    CACHE_OK(11, 0x11);
    CACHE_OK(12, 0x15);
    CACHE_OK(13, 0x19);
    CACHE_OK(14, 0x1d);

    /*
     * Secondary cache: the same seven operations as the primary data
     * cache.
     * The secondary cache is indexed by physical address, which is
     * another
     * reason the address handed over is an unmapped one.
     */
    CACHE_OK(15, 0x03);
    CACHE_OK(16, 0x07);
    CACHE_OK(17, 0x0b);
    CACHE_OK(18, 0x13);
    CACHE_OK(19, 0x17);
    CACHE_OK(20, 0x1b);
    CACHE_OK(21, 0x1f);

    /*
     * The instruction forms its address the way a load does, from a
     * base
     * register and a signed offset, so an operation with a nonzero
     * offset
     * must
     * work as well as one at offset zero.
     */
    __asm__ __volatile__("cache 0x01, 128(%0)" : : "r"(addr) : "memory");
    rom_assert_eq(22, 0, rom_exc.count);

    /* Reject operations that faulted and resumed without changing results. */
    rom_assert_eq(23, ~0ull, rom_exc.vector);
}
