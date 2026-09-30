/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The host test must confirm its GDB stall as well as this ROM's PASS. */

#include "rom.h"

const char rom_name[] = "timerstall";

/* Status.IM7 masks the timer interrupt. */
#define ROM_ST_IM7 (1u << 15)

/* The host test stalls longer than this roughly 47 ms timer interval. */
#define STALL_TARGET 0x00400000u
#define POLL_LIMIT   50000000

void rom_main(void)
{
    int i;

    rom_exc_reset();
    rom_set_status(rom_get_status() & ~(ROM_ST_IE | ROM_ST_IM7));

    rom_set_compare(rom_get_count() + 0x40000000u);
    rom_assert_eq(1, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /* Cause polling distinguishes a match from full-wrap deferral. */
    rom_set_count(0);
    rom_set_compare(STALL_TARGET);
    for (i = 0; i < POLL_LIMIT && !(rom_get_cause() & ROM_CAUSE_IP7); i++) {
    }
    rom_assert_eq(2, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);

    rom_set_compare(rom_get_count() + 0x40000000u);
    rom_assert_eq(3, 0, rom_get_cause() & ROM_CAUSE_IP7);

    rom_assert_eq(4, 0, rom_exc.count);
}
