/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Every cache operation the R10000 defines must be accepted rather than
 * faulted.
 *
 * This is the quietest way an early boot can die. A guest kernel
 * initialises
 * its caches by walking every index of every cache with the CACHE
 * instruction,
 * hundreds of executions deep, and it does so before it has a console.
 * A
 * model
 * that raises a reserved-instruction or coprocessor-unusable exception
 * on one
 * of those operations produces a machine that hangs with nothing
 * printed and
 * no clue as to why.
 *
 * Oracle: the R10000 manual's CACHE instruction op field encoding table
 * (Table 10-1), which lists twenty-one operation and target-cache
 * combinations. The operation is instruction bits 20:16, the same field
 * an
 * ordinary load would use for its destination register. Its low two
 * bits name
 * the target cache — 0 the primary instruction cache, 1 the primary
 * data
 * cache, 3 the secondary cache — and its high three bits name the
 * operation.
 * The manual's notes on cache instruction operations are explicit that
 * "the
 * operation of any operation/cache combination not listed below is
 * undefined",
 * so this ROM exercises exactly the twenty-one listed combinations and
 * none
 * of
 * the ten the manual leaves undefined: an emulator is free to do
 * anything at
 * all with those, and a test that pinned them down would be inventing a
 * contract rather than checking one.
 *
 * Two constraints from the same notes shape the address every operation
 * is
 * given. First, "the operation of this instruction on uncached
 * addresses is
 * also undefined", so the address must not be an uncached one. Second,
 * TLB
 * refill and TLB invalid exceptions can occur on any cache operation,
 * and the
 * notes recommend unmapped addresses to avoid them. The address used
 * below is
 * therefore the unmapped cached view of the ROM's own scratch buffer:
 * the
 * same
 * physical memory the buffer occupies, reached through the
 * compatibility
 * segment that is unmapped and cached rather than the one that is
 * unmapped
 * and
 * uncached. The two views differ by one address bit.
 *
 * The manual's note on coprocessor 0 not being usable says a cache
 * operation
 * raises a coprocessor-unusable exception unless the processor is in
 * kernel
 * mode or the CU0 bit of the Status register is set. This ROM runs in
 * kernel
 * mode throughout, which is the case that must work; the user-mode side
 * of
 * that rule belongs with the other user-mode privilege checks and is
 * not
 * repeated here.
 *
 * Nothing in this ROM is specific to the R10000 — QEMU treats the whole
 * instruction as a no-operation for every processor of this generation
 * — so
 * it
 * runs against the R4000 model too, and passing there is what says the
 * ROM is
 * sound rather than the model being lenient.
 */

#include "rom.h"

const char rom_name[] = "cacheops";

/*
 * A block of scratch memory to point the cache operations at. It is
 * aligned
 * to
 * the largest block size the R10000 can be configured with, a 32-word
 * secondary cache block, so that an index operation addresses one whole
 * block
 * rather than straddling two.
 */
static volatile unsigned char scratch[256] __attribute__((aligned(128)));

/*
 * The unmapped cached view of an address in the unmapped uncached one.
 */
#define KSEG0_VIEW(p) ((unsigned long)(p) & ~0x20000000ul)

/*
 * Run one cache operation and check that nothing was raised by it. The
 * operation field has to be spelled out at each use because it is an
 * instruction field rather than a register, so there is no loop to
 * write.
 */
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

    /*
     * No exception entry point ran at any point above. The record still
     * holds
     * the value rom_exc_reset() writes to mean that nothing has been
     * recorded,
     * so no vector fired and was then quietly resumed from.
     */
    rom_assert_eq(23, ~0ull, rom_exc.vector);
}
