/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check that CP0 register 26 preserves defined ECC bits and zeros reserved
 * bits.
 */

#include "rom.h"

/* Defined and reserved fields in CacheErr. */
#define ECC_DEFINED  0x000003ffu
#define ECC_RESERVED 0xfffffc00u

/* Firmware's observed primary instruction-cache parity test value. */
#define FIRMWARE_ECC_VECTOR 0x00000001u

const char rom_name[] = "cacheecc";

void rom_main(void)
{
    unsigned int bit;

    rom_exc_reset();

    /* Exercise all defined bits together. */
    rom_set_ecc(ECC_DEFINED);
    rom_assert_eq(1, ECC_DEFINED, rom_get_ecc());

    /* Ensure the register is not stuck at the previous value. */
    rom_set_ecc(0x00000000u);
    rom_assert_eq(2, 0x00000000u, rom_get_ecc());

    /* Isolate each defined bit to identify a missing mask bit. */
    for (bit = 0; bit < 10; bit++) {
        uint32_t value = 1u << bit;

        rom_set_ecc(value);
        rom_assert_eq(100 + (int)bit, value, rom_get_ecc());
    }

    /* Writing all ones catches reserved bits that incorrectly retain data. */
    rom_set_ecc(0xffffffffu);
    rom_assert_eq(3, ECC_DEFINED, rom_get_ecc());

    /* Isolate reserved bits that might leak through individually. */
    for (bit = 10; bit < 32; bit++) {
        rom_set_ecc(1u << bit);
        rom_assert_eq(200 + (int)bit, 0x00000000u, rom_get_ecc());
    }

    /* Preserve the observed failing-value regression case. */
    rom_set_ecc(FIRMWARE_ECC_VECTOR);
    rom_assert_eq(4, FIRMWARE_ECC_VECTOR, rom_get_ecc());

    /* Reject accesses that faulted and resumed despite matching readback. */
    rom_assert_eq(5, 0, rom_exc.count);
    rom_assert_eq(6, ~0ull, rom_exc.vector);
}
