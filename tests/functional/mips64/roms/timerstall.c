/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * One timer arm, watched for its pending bit — the ROM half of the
 * stalled-store reproducer.
 *
 * On its own this ROM is unremarkable: arm Compare a short distance
 * ahead
 * of Count and poll the Cause register until IP[7] pends, interrupts
 * held
 * off throughout so the bit is observed rather than taken. What it
 * exists
 * for is the harness around it, tools/timer-stall-repro, which runs the
 * emulator under a debugger that halts the process inside every Compare
 * store long enough for virtual time to pass the value being written.
 * The
 * R10000's comparator is continuous, so a write the counter has already
 * passed must still pend — a model that reads the clock once to
 * classify
 * the write and again to arm the deadline instead defers it a full
 * counter
 * wrap, and the poll below runs dry. The stall is the debugger's,
 * applied
 * to the emulator process, which is why no unstalled run — and no
 * icount
 * run, where helpers take no virtual time — can exercise this seam.
 *
 * Deliberately not in the suite manifest: without the harness the stall
 * never happens and the ROM passes without testing anything beyond what
 * the timer ROM already pins.
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
