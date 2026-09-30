/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check the R10000 TagLo register behavior. See the MIPS R10000
 * Microprocessor User's Manual version 2.0.
 */

#include "rom.h"

const char rom_name[] = "cachetags";

/*
 * The value the vendor firmware writes when it tests the primary
 * instruction
 * cache tag path: every field the manual defines for that cache set,
 * and
 * every
 * reserved bit clear. Recorded here as the observed sample it is, so
 * that the
 * case below is anchored to a real guest rather than to a value
 * invented to
 * match the model.
 */
#define FIRMWARE_ITAG_VECTOR 0xffffff4du

void rom_main(void)
{
    unsigned int bit;

    rom_exc_reset();

    /*
     * All ones. No mask survives this, which is the point: it fails for
     * any
     * model that drops any bit at all, without the test needing to know
     * which
     * bits a particular wrong mask would have dropped.
     */
    rom_set_taglo(0xffffffffu);
    rom_assert_eq(1, 0xffffffffu, rom_get_taglo());

    /*
     * And all zeroes, so that the case above cannot pass by reading a
     * stuck
     * value.
     */
    rom_set_taglo(0x00000000u);
    rom_assert_eq(2, 0x00000000u, rom_get_taglo());

    /*
     * One bit at a time through all thirty-two positions. Where a
     * partial
     * mask
     * is present this says which bit it drops rather than only that the
     * register did not round-trip, and the assertion identifier carries
     * the
     * bit position so a failure names it directly.
     */
    for (bit = 0; bit < 32; bit++) {
        uint32_t value = 1u << bit;

        rom_set_taglo(value);
        rom_assert_eq(100 + (int)bit, value, rom_get_taglo());
    }

    /*
     * The observed failing case, last and as a regression anchor. If
     * the two
     * checks above ever pass while this one fails, the register is
     * doing
     * something stranger than masking and the difference is worth
     * knowing.
     */
    rom_set_taglo(FIRMWARE_ITAG_VECTOR);
    rom_assert_eq(3, FIRMWARE_ITAG_VECTOR, rom_get_taglo());

    /*
     * TagHi is checked only for the four bits the manual defines for it
     * --
     * the
     * top four bits of the physical address a tag carries. The manual's
     * 32-bit read/write statement is made about TagLo specifically and
     * is not
     * repeated for TagHi, so asserting a full round-trip here would be
     * extending a guarantee past where it was actually given. This is
     * the
     * value the vendor firmware pairs with the vector above, and it is
     * the
     * half of that comparison which already succeeded.
     */
    rom_set_taghi(0x0000000fu);
    rom_assert_eq(4, 0x0000000fu, rom_get_taghi());

    /*
     * Nothing above should have raised anything. A model that took an
     * exception on one of these accesses and resumed would otherwise
     * leave
     * the
     * value checks looking clean.
     */
    rom_assert_eq(5, 0, rom_exc.count);
    rom_assert_eq(6, ~0ull, rom_exc.vector);
}
