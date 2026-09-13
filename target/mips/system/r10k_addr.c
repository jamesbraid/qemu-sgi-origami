/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The R10000 family's xkphys uncached-attribute window, split out of the
 * shared address translation. get_physical_address() calls it for an
 * unmapped xkphys reference; it is inert on any processor whose definition
 * does not carry the feature.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "../internal.h"

/*
 * The two-bit uncached attribute goes out to the system interface alongside
 * the address, and an unmapped xkphys reference takes it from address bits
 * 58:57. Those two bits are not part of the physical address and, for an
 * uncached or uncached accelerated reference (cache algorithm 2 or 7), are
 * not checked for address errors; only bits 56:40 have to be zero. Any other
 * cache algorithm keeps the narrower rule that bits 58:40 must be zero, which
 * the caller enforces by leaving *pa unmasked.
 *
 * The SGI Origin address map is built on this: the four values of the
 * attribute are how a processor says whether an uncached reference is meant
 * for node memory, for the Hub's own registers, for a crossbar widget, or for
 * a special memory operation. The attribute is a separate signal, not
 * address, so it has nowhere to live in a physical address. Give it one: the
 * memory system sees the reference above the physical address, which is what
 * lets a machine tell an uncached reference to memory from one to a register
 * on the other side of the Hub. Attribute 3 is memory and is placed at window
 * zero, so a plain physical address still means memory and nothing that does
 * not use the attribute has to know about any of this.
 *
 * Returns the window offset to place above the physical address and clears
 * the attribute bits out of *pa; returns zero and leaves *pa untouched for a
 * cached reference or a processor without the feature.
 */
hwaddr mips_r10k_xkphys_uncached_window(CPUMIPSState *env, target_ulong address,
                                        unsigned int cca, uint64_t *pa)
{
    unsigned int attr;

    if (!env->cfg.uncached_attribute || (cca != 2 && cca != 7)) {
        return 0;
    }
    attr = (address >> 57) & 3;
    *pa &= ~(3ULL << 57);
    return (hwaddr)((attr + 1) & 3) << MIPS_UNCACHED_WINDOW_SHIFT;
}
