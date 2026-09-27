/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The ECC register keeps its ten bits, and only its ten bits.
 *
 * Coprocessor 0 register 26 is not the same register on every MIPS
 * processor,
 * and that is the whole of this ROM's point. On the R10000 it is the
 * ECC
 * register: ten bits wide, read/write, holding the parity or
 * error-correcting
 * bits a cache operation loads from or stores to. Bit 9 is a secondary
 * cache
 * quadword's parity bit and bits 8:0 its nine ECC bits, with a primary
 * data
 * cache's four parity bits and a primary instruction cache's single
 * parity
 * bit
 * occupying the low end of that same nine. Bits 31:10 are reserved.
 * Oracle: MIPS R10000 Microprocessor User's Manual, version 2.0 of
 * October
 * 10,
 * 1996, section 14.21 "ECC Register (26)", page 273, and its Figure
 * 14-23 and
 * Table 14-23. The section's opening sentence calls it a 10-bit
 * read/write
 * register; the table's last row says of the reserved field "Must be
 * written
 * as zeroes, and returns zeroes when read."
 *
 * THE TWO ENDS ARE BOTH REAL CHECKS, AND THIS IS THE DIFFERENCE FROM
 * THE
 * TAGLO ROM BESIDE IT. TagLo is promised to round-trip all thirty-two
 * bits,
 * so
 * writing all ones and reading them back is the whole test there. Here
 * the
 * manual promises ten bits and says the other twenty-two read back as
 * zero,
 * so
 * a model can be wrong in two opposite directions: it can drop bits it
 * should
 * keep, or keep bits it should drop. Both are checked below, because a
 * test
 * for only the first would be passed by a model that never masks
 * anything.
 *
 * WHAT ELSE WOULD PASS THIS. The firmware whose failure prompted the
 * ROM
 * writes a single value with one bit set. A model masking this register
 * with
 * exactly that value would satisfy that firmware and still be wrong
 * about the
 * other nine bits, so the firmware's value appears here last and as a
 * regression anchor rather than as the evidence. The walking one below
 * covers
 * every one of the ten defined positions individually, so a mask that
 * drops
 * any single one of them is localised to that bit rather than merely
 * detected.
 *
 * This ROM is specific to the R10000 and is not listed against the
 * R4000,
 * whose register 26 is a different register with control fields in it.
 * The
 * harness-validating cross-check the shared ROMs get from a second
 * processor
 * is therefore unavailable here, as it is for the TagLo ROM; what
 * stands in
 * for it is that the defect was found by the vendor's own firmware
 * exercising
 * the part, before this ROM existed.
 *
 * No CACHE instruction appears below. The manual's read/write statement
 * is
 * about the register, and a cache operation is exactly the thing that
 * would
 * legitimately change its contents underneath a round-trip.
 */

#include "rom.h"

/*
 * The ten bits the manual defines, and the twenty-two it reserves.
 */
#define ECC_DEFINED  0x000003ffu
#define ECC_RESERVED 0xfffffc00u

/*
 * The value the vendor firmware writes when it tests the primary
 * instruction
 * cache data path: the single parity bit for one instruction word.
 * Recorded
 * as
 * the observed sample it is.
 */
#define FIRMWARE_ECC_VECTOR 0x00000001u

const char rom_name[] = "cacheecc";

void rom_main(void)
{
    unsigned int bit;

    rom_exc_reset();

    /*
     * Every defined bit at once, which no narrower mask survives.
     */
    rom_set_ecc(ECC_DEFINED);
    rom_assert_eq(1, ECC_DEFINED, rom_get_ecc());

    /*
     * And zero, so the check above cannot pass by reading something
     * stuck.
     */
    rom_set_ecc(0x00000000u);
    rom_assert_eq(2, 0x00000000u, rom_get_ecc());

    /*
     * Each defined bit on its own. Where a mask drops one of them this
     * names
     * which, rather than only reporting that the register did not
     * round-trip.
     */
    for (bit = 0; bit < 10; bit++) {
        uint32_t value = 1u << bit;

        rom_set_ecc(value);
        rom_assert_eq(100 + (int)bit, value, rom_get_ecc());
    }

    /*
     * The other direction, and the half a round-trip test on its own
     * would
     * miss. Writing all ones must read back as the defined ten alone: a
     * model
     * that keeps the reserved twenty-two passes every check above and
     * is
     * still
     * wrong, because the manual says those bits return zeroes when
     * read.
     */
    rom_set_ecc(0xffffffffu);
    rom_assert_eq(3, ECC_DEFINED, rom_get_ecc());

    /*
     * Each reserved bit on its own must read back as nothing at all.
     */
    for (bit = 10; bit < 32; bit++) {
        rom_set_ecc(1u << bit);
        rom_assert_eq(200 + (int)bit, 0x00000000u, rom_get_ecc());
    }

    /*
     * The observed failing case, last, as the regression anchor.
     */
    rom_set_ecc(FIRMWARE_ECC_VECTOR);
    rom_assert_eq(4, FIRMWARE_ECC_VECTOR, rom_get_ecc());

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
