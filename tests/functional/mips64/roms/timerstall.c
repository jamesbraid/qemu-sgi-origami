/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The timer-stall functional test runs this ROM under host GDB. It stops QEMU
 * between classifying a Compare write and arming its timer, long enough for
 * Count to pass the written value. The R10000 comparator must still pend IP7.
 *
 * An ordinary ROM run cannot force the host stall. Keep this payload outside
 * the 85-case ROM matrix and require the GDB breakpoint-hit marker alongside
 * its PASS result in test_timer_stall.py.
 */

#include "rom.h"

const char rom_name[] = "timerstall";

/*
 * Interrupt mask bit 7 of the Status register, the timer's mask.
 */
#define ROM_ST_IM7 (1u << 15)

/*
 * Close enough ahead that the harness's stall overshoots it, far enough
 * that an unstalled run cannot arm it behind the counter: ~47 ms at the
 * machine's counter rate against a stall of 200 ms.
 */
#define STALL_TARGET 0x00400000u
#define POLL_LIMIT   50000000

void rom_main(void)
{
    int i;

    rom_exc_reset();
    rom_set_status(rom_get_status() & ~(ROM_ST_IE | ROM_ST_IM7));

    /*
     * A Compare write clears any pending state left over from reset.
     */
    rom_set_compare(rom_get_count() + 0x40000000u);
    rom_assert_eq(1, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * The probe: one arm ahead of a zeroed counter. Stalled or not, the
     * counter meets the value and the pending bit must appear — the
     * only
     * question the harness asks is whether the model lets a stall
     * inside
     * the store turn this deadline into a full-wrap deferral.
     */
    rom_set_count(0);
    rom_set_compare(STALL_TARGET);
    for (i = 0; i < POLL_LIMIT && !(rom_get_cause() & ROM_CAUSE_IP7); i++) {
        /*
         * Poll Cause, not Count: only the timer may raise the bit.
         */
    }
    rom_assert_eq(2, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * The next write takes the pending bit away again.
     */
    rom_set_compare(rom_get_count() + 0x40000000u);
    rom_assert_eq(3, 0, rom_get_cause() & ROM_CAUSE_IP7);

    rom_assert_eq(4, 0, rom_exc.count);
}
