/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test the unmapped 64-bit physical window and its uncached attribute.
 * Uncached references use bits 58:57 as the attribute; bits 39:0 select
 * the physical address.
 *
 * Source: MIPS R10000 User's Manual, version 2.0, memory-management
 * chapter, 64-bit kernel-mode physical spaces and uncached-attribute
 * sections.
 */

#include "rom.h"

const char rom_name[] = "xkphys";

/*
 * Cache algorithm in bits 61:59 of an unmapped 64-bit address.
 */
#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))

#define CCA_UNCACHED     2
#define CCA_CACHEABLE    5   /* cacheable coherent exclusive on write */
#define CCA_UNCACHED_ACC 7

/*
 * Backed by memory on every machine this runs on.
 */
#define SCRATCH_PA 0x00400000ull

/*
 * Inside the forty bits the processor has, and far above the thirty-six
 * a
 * processor of the previous generation had, so nothing is likely to
 * answer.
 * Whether anything answers is not the point: an address error would
 * mean the
 * processor refused the address, which is the thing being measured.
 */
#define WIDE_PA 0x0000004000000000ull

/*
 * Bits 56:40, which no cache algorithm allows to be set.
 */
#define ILLEGAL_PA 0x0000010000000000ull

/*
 * Perform one load and report what the processor made of it.
 */
static uint64_t probe(uint64_t addr)
{
    uint64_t dead;

    rom_exc_reset();
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         "lui $26, %%hi(9f)\n\t"
                         "daddiu $26, $26, %%lo(9f)\n\t"
                         "sd $26, 0(%[res])\n\t"
                         "ld %[d], 0(%[a])\n"
                         "9:\t.set pop"
                         : [d] "=&r"(dead)
                         : [res] "r"(&rom_exc.resume), [a] "r"(addr)
                         : "$26", "memory");
    (void)dead;
    return rom_exc.count;
}

/*
 * Whether the processor refused the address, as opposed to the memory
 * system
 * having nothing there. Only the refusal is architectural.
 */
static int refused(uint64_t addr)
{
    return probe(addr) != 0 &&
           ROM_CAUSE_EXC(rom_exc.cause) == ROM_EXC_ADEL;
}

void rom_main(void)
{
    /*
     * Volatile preserves accesses through the uncached compatibility
     * mapping.
     */
    volatile uint64_t *compat;

    /*
     * The window exists only with 64-bit kernel addressing enabled.
     */
    rom_set_status(rom_get_status() | ROM_ST_KX);

    /*
     * A cached reference to backed memory, as the control.
     */
    rom_assert_eq(1, 0, probe(XKPHYS(CCA_CACHEABLE, 0, SCRATCH_PA)));

    /*
     * All four values of the uncached attribute are legal addresses.
     * Three of
     * them have bits 58:57 set, which under the narrower rule would be
     * an
     * address error, and that is exactly the rule the manual exempts an
     * uncached reference from.
     *
     * Only the refusal is asserted for three of the four, because what
     * a
     * machine puts behind an attribute is the machine's business: on
     * this one
     * the attribute that names memory reaches memory, and the other
     * three
     * name
     * the Hub, the crossbar and special memory operations, so whether
     * anything
     * answers depends on what has been built. An address error would
     * mean the
     * processor rejected the address, which is the part the manual
     * settles.
     */
    rom_assert(2, !refused(XKPHYS(CCA_UNCACHED, 0, SCRATCH_PA)));
    rom_assert(3, !refused(XKPHYS(CCA_UNCACHED, 1, SCRATCH_PA)));
    rom_assert(4, !refused(XKPHYS(CCA_UNCACHED, 2, SCRATCH_PA)));
    rom_assert_eq(5, 0, probe(XKPHYS(CCA_UNCACHED, 3, SCRATCH_PA)));

    /*
     * Uncached accelerated carries the attribute on the same terms.
     */
    rom_assert_eq(6, 0, probe(XKPHYS(CCA_UNCACHED_ACC, 3, SCRATCH_PA)));

    /*
     * A cache algorithm that is neither of those two keeps the narrower
     * rule,
     * so the same two bits become part of the address and take it out
     * of
     * range.
     */
    rom_assert(7, refused(XKPHYS(CCA_CACHEABLE, 1, SCRATCH_PA)));
    rom_assert(8, refused(XKPHYS(CCA_CACHEABLE, 3, SCRATCH_PA)));

    /*
     * Bits 56:40 are out of range under either rule.
     */
    rom_assert(9, refused(XKPHYS(CCA_UNCACHED, 3, ILLEGAL_PA)));
    rom_assert(10, refused(XKPHYS(CCA_UNCACHED_ACC, 0, ILLEGAL_PA)));
    rom_assert(11, refused(XKPHYS(CCA_CACHEABLE, 0, ILLEGAL_PA)));

    /*
     * An address inside the forty bits is not refused, whether or not
     * anything
     * answers it. This is what says the declared physical width is real
     * rather
     * than the narrower width of the previous generation.
     */
    rom_assert(12, !refused(XKPHYS(CCA_UNCACHED, 3, WIDE_PA)));
    rom_assert(13, !refused(XKPHYS(CCA_CACHEABLE, 0, WIDE_PA)));

    /*
     * The attribute is not part of the physical address. A store
     * through the
     * uncached window that names node memory reaches the same storage
     * as the
     * uncached compatibility segment, which addresses the same physical
     * bytes
     * without any attribute bits of its own.
     */
    compat = (volatile uint64_t *)(unsigned long)(0xffffffffa0000000ull +
                                                  SCRATCH_PA);
    *compat = 0;
    rom_exc_reset();
    __asm__ __volatile__("sd %1, 0(%0)"
                         :
                         : "r"(XKPHYS(CCA_UNCACHED, 3, SCRATCH_PA)),
                           "r"(0x0123456789abcdefull)
                         : "memory");
    rom_assert_eq(14, 0, rom_exc.count);
    rom_assert_eq(15, 0x0123456789abcdefull, *compat);
}
