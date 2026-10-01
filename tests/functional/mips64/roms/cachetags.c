/* SPDX-License-Identifier: BSD-3-Clause */
/* Check R10000 TagLo and TagHi read/write behavior. */

#include "rom.h"

const char rom_name[] = "cachetags";

/* Observed firmware value for the primary instruction-cache tag path. */
#define FIRMWARE_ITAG_VECTOR 0xffffff4du

void rom_main(void)
{
    unsigned int bit;

    rom_exc_reset();

    /* All ones expose any dropped writable bit. */
    rom_set_taglo(0xffffffffu);
    rom_assert_eq(1, 0xffffffffu, rom_get_taglo());

    /* Ensure readback is not stuck at the previous value. */
    rom_set_taglo(0x00000000u);
    rom_assert_eq(2, 0x00000000u, rom_get_taglo());

    /* Isolate each bit to identify a partial write mask. */
    for (bit = 0; bit < 32; bit++) {
        uint32_t value = 1u << bit;

        rom_set_taglo(value);
        rom_assert_eq(100 + (int)bit, value, rom_get_taglo());
    }

    /* Retain the observed failing value as a regression case. */
    rom_set_taglo(FIRMWARE_ITAG_VECTOR);
    rom_assert_eq(3, FIRMWARE_ITAG_VECTOR, rom_get_taglo());

    /*
     * Check only TagHi's documented top four physical-address bits; the
     * full-width TagLo read/write guarantee does not apply to TagHi.
     */
    rom_set_taghi(0x0000000fu);
    rom_assert_eq(4, 0x0000000fu, rom_get_taghi());

    /* Reject accesses that faulted and resumed despite matching readback. */
    rom_assert_eq(5, 0, rom_exc.count);
    rom_assert_eq(6, ~0ull, rom_exc.vector);
}
