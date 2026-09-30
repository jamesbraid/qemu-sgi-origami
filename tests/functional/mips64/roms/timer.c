/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test the Count and Compare registers and their shared timer
 * interrupt. Interrupts remain masked so the pending bit is observed
 * directly. The ROM checks matching and clearing, not timer rate.
 *
 * Source: R10000 User's Manual, version 2.0, section 14.8.
 */

#include "rom.h"

const char rom_name[] = "timer";

/* Status.IM7 masks the timer interrupt. */
#define ROM_ST_IM7 (1u << 15)

/* The target exceeds scheduling delays; the bound catches a stopped Count. */
#define TIMER_TARGET (1u << 22)
#define POLL_LIMIT   50000000

void rom_main(void)
{
    uint32_t first, second, cause;
    uint32_t status;
    int i;

    rom_exc_reset();

    /* Observe pending IP[7] in Cause without taking the interrupt. */
    rom_set_status(rom_get_status() & ~ROM_ST_IE);

    /* Check a wide readback window because Count advances after the write. */
    rom_set_count(0x40000000u);
    first = rom_get_count();
    rom_assert(1, first >= 0x40000000u);
    rom_assert(2, first < 0x41000000u);

    /* Repeated CP0 reads must observe Count advancing. */
    first = rom_get_count();
    second = first;
    for (i = 0; i < POLL_LIMIT && second == first; i++) {
        second = rom_get_count();
    }
    rom_assert(3, second > first);

    rom_set_compare(0x12345678u);
    rom_assert_eq(4, 0x12345678u, rom_get_compare());

    /* Compare writes clear IP[7]. */
    rom_set_compare(rom_get_count() + 0x40000000u);
    rom_assert_eq(5, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /* Arm well ahead of Count so host scheduling cannot miss the crossing. */
    for (second = 0; second < 8; second++) {
        rom_set_count(0);
        rom_set_compare(TIMER_TARGET);
        first = rom_get_count();
        if (first < TIMER_TARGET) {
            /* Keep the read that proved Count had not passed the target. */
            break;
        }
    }
    rom_assert(6, first < TIMER_TARGET);

    for (i = 0; i < POLL_LIMIT && rom_get_count() < TIMER_TARGET; i++) {
    }
    rom_assert(7, rom_get_count() >= TIMER_TARGET);

    cause = rom_get_cause();
    rom_assert_eq(8, ROM_CAUSE_IP7, cause & ROM_CAUSE_IP7);

    rom_set_compare(rom_get_count() + 0x40000000u);
    rom_assert_eq(9, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /* Status.IM7 must be writable. */
    status = rom_get_status();
    rom_set_status(status | ROM_ST_IM7);
    rom_assert_eq(10, ROM_ST_IM7, rom_get_status() & ROM_ST_IM7);
    rom_set_status(status & ~ROM_ST_IM7);
    rom_assert_eq(11, 0, rom_get_status() & ROM_ST_IM7);

    rom_assert_eq(12, 0, rom_exc.count);

    /* R10000 equality writes set IP[7]; other models retain QEMU behavior. */
    if (((rom_get_prid() >> 8) & 0xff) == 0x09) {
        /* Establish a nonmatching baseline before the equality write. */
        rom_set_count(0);
        rom_set_compare(0x40000000u);
        rom_assert_eq(13, 0, rom_get_cause() & ROM_CAUSE_IP7);
        rom_set_count(0x40000000u);
        rom_assert_eq(14, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);

        rom_set_compare(rom_get_count() + 0x40000000u);
        rom_assert_eq(15, 0, rom_get_cause() & ROM_CAUSE_IP7);

        rom_assert_eq(16, 0, rom_exc.count);

    }

    /* Behind-Count writes leave IP[7] clear; R14000 behavior is unknown. */
    rom_set_count(0x00200000u);
    rom_set_compare(0x00100000u);
    rom_assert_eq(17, 0, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_compare(rom_get_count() + 0x40000000u);
    rom_assert_eq(18, 0, rom_get_cause() & ROM_CAUSE_IP7);
    rom_assert_eq(19, 0, rom_exc.count);

}
