/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * R14000 cache choices that distinguish the selected 2 MiB secondary
 * cache
 * from the existing 1 MiB R10000 model.
 *
 * Oracle: docs/datasheets/r14000.md at Origami commit 14dbec407f.  The
 * cache
 * has two ways and 128-byte lines by reviewed R12000A policy.  A 2 MiB
 * cache
 * therefore has twice the index span of the R10000 implementation.  The
 * same
 * contract also clears Linux's read-modify-write of Diagnostic bit 26.
 */

#include "rom.h"

const char rom_name[] = "r14000_cache";

#define KSEG0_VIEW(p) ((unsigned long)(p) & ~0x20000000ul)
#define HALF_MIB (512ul * 1024)
#define DIAG_BIT26 (1ull << 26)

/*
 * Volatile keeps the memory accesses visible to this cache test.
 */
static volatile unsigned char scratch[128] __attribute__((aligned(128)));

static void scache_store_tag(unsigned long addr)
{
    __asm__ __volatile__("cache 0x0b, 0(%0)" : : "r"(addr) : "memory");
}

static void scache_load_tag(unsigned long addr)
{
    __asm__ __volatile__("cache 0x07, 0(%0)" : : "r"(addr) : "memory");
}

static void plant(unsigned long addr, uint32_t value)
{
    rom_set_taglo(value);
    rom_set_taghi(0);
    scache_store_tag(addr);
}

static uint32_t read_tag(unsigned long addr)
{
    rom_set_taglo(0);
    rom_set_taghi(0);
    scache_load_tag(addr);
    return rom_get_taglo();
}

void rom_main(void)
{
    unsigned long first = KSEG0_VIEW(&scratch[0]);
    unsigned long second = first + HALF_MIB;
    uint64_t diagnostic;

    rom_exc_reset();

    /*
     * A 1 MiB, two-way cache aliases these indices; a 2 MiB cache does
     * not.
     */
    plant(first, 0x11110111u);
    plant(second, 0x22220022u);
    rom_assert_eq(1, 0x11110111u, read_tag(first));
    rom_assert_eq(2, 0x22220022u, read_tag(second));

    diagnostic = rom_get_brdiag();
    rom_set_brdiag(diagnostic | DIAG_BIT26);
    rom_assert_eq(3, DIAG_BIT26, rom_get_brdiag() & DIAG_BIT26);
    rom_set_brdiag(diagnostic);
    rom_assert_eq(4, diagnostic & DIAG_BIT26,
                  rom_get_brdiag() & DIAG_BIT26);

    rom_assert_eq(5, 0, rom_exc.count);
}
