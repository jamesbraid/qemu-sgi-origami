/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check that CP0 register 26 preserves the ten defined ECC bits and
 * reads reserved bits as zero. See the MIPS R10000 Microprocessor User's
 * Manual version 2.0, section 14.21.
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
