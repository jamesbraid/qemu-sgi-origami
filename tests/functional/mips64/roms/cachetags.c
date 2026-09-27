/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TagLo holds every bit written to it, and hands them all back.
 *
 * The R10000's TagLo is not merely a cache tag staging register with
 * some
 * fields defined and the rest dropped. The manual says twice -- in the
 * body
 * of
 * its TagLo and TagHi section and again in that section's own footnote
 * --
 * that
 * the register is a 32-bit read/write register, and that a value
 * written by
 * MTC0 can be retrieved by MFC0 unless a CACHE instruction has modified
 * the
 * content in between. It gives the reason: an operating system of the
 * day
 * used
 * it as scratch storage, so it had to keep bits no cache tag field
 * claims.
 * Oracle: MIPS R10000 Microprocessor User's Manual, version 2.0 of
 * October
 * 10,
 * 1996, section 14.23 "TagLo (28) and TagHi (29) Registers", pages
 * 278-279.
 *
 * Why this is worth a ROM rather than a line in a datasheet. A guest
 * that
 * initialises its caches writes a tag into TagLo, stores it into the
 * tag
 * array
 * with an index store-tag operation, loads it back with an index
 * load-tag
 * operation, and compares. Every bit the register quietly drops turns
 * that
 * comparison into a mismatch, and the guest concludes its cache is
 * broken.
 * The
 * vendor firmware this project boots does exactly that, and reported an
 * instruction-cache self-test failure on this model for exactly this
 * reason:
 * the bits it dropped were the tag parity bit, the least-recently-used
 * bit,
 * and two bits of the physical address the tag carries.
 *
 * WHAT ELSE WOULD PASS THIS, AND WHY THE CASES BELOW ARE THE ONES THEY
 * ARE.
 * The obvious test is the value the vendor firmware happens to use.
 * That test
 * is satisfied by a model masking TagLo with exactly that value, which
 * would
 * be a mask fitted to one sample and wrong about the register -- it
 * would
 * pass
 * while still dropping every bit the sample leaves clear. So the first
 * case
 * here is all ones, which no mask of any kind survives, and the second
 * walks
 * a
 * single one through all thirty-two bit positions, which localises
 * whichever
 * bit a partial mask drops instead of merely reporting that something
 * was
 * lost. The firmware's own value is checked last, as the regression
 * anchor
 * for
 * the case that was actually observed failing, not as the evidence.
 *
 * This ROM is specific to the R10000. A processor whose TagLo genuinely
 * does
 * reserve bits would rightly fail it, which is why it is not listed
 * against
 * the R4000 -- and that means the harness-validating cross-check the
 * other
 * ROMs get from a second model is not available here. What stands in
 * for it
 * is
 * that the failure this ROM detects was first found by the vendor's own
 * firmware rather than by the ROM, so the behaviour under test was
 * established
 * independently of the test written for it.
 *
 * No CACHE instruction appears anywhere below. The manual's guarantee
 * is
 * explicitly conditioned on none intervening, so a ROM that issued one
 * would
 * be testing something the manual does not promise.
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
