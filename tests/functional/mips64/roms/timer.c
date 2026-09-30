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

/*
 * Interrupt mask bit 7 of the Status register, the timer's mask.
 */
#define ROM_ST_IM7 (1u << 15)

/*
 * How far ahead of Count the Compare register is armed for the match
 * test,
 * and
 * how many times to read Count while waiting for anything at all. The
 * bound
 * exists so that a counter which never advances fails with its own
 * assertion
 * number rather than running until the runner's timeout, which would
 * say
 * nothing about which step went wrong. It is not a prediction of how
 * long the
 * wait takes: every wait below ends on the condition it is waiting for.
 */
#define TIMER_TARGET (1u << 22)
#define POLL_LIMIT   50000000

void rom_main(void)
{
    uint32_t first, second, cause;
    uint32_t status;
    int i;

    rom_exc_reset();

    /*
     * Interrupts off, so a pending timer interrupt is only ever
     * observed as a
     * bit in the Cause register.
     */
    rom_set_status(rom_get_status() & ~ROM_ST_IE);

    /*
     * Count is writable. Reading it straight back cannot give exactly
     * the
     * value
     * written, because the counter keeps advancing between the write
     * and the
     * read, so the check is a window: at least the value written, and
     * less
     * than
     * that value plus a margin wide enough to survive the host stalling
     * the
     * emulated processor between the write and the read, yet still
     * under a
     * hundredth of the counter's range. Writing a large value rather
     * than
     * zero
     * is what makes this prove anything — Count is naturally small this
     * early
     * in
     * a run, so a write of zero would appear to have landed even if it
     * were
     * discarded.
     */
    rom_set_count(0x40000000u);
    first = rom_get_count();
    rom_assert(1, first >= 0x40000000u);
    rom_assert(2, first < 0x41000000u);

    /*
     * Count advances on its own. Reading it repeatedly must eventually
     * give a
     * larger value than the first read. Both reads go through a
     * volatile move
     * from coprocessor 0, so neither can be hoisted out of the loop or
     * folded
     * into the other.
     */
    first = rom_get_count();
    second = first;
    for (i = 0; i < POLL_LIMIT && second == first; i++) {
        second = rom_get_count();
    }
    rom_assert(3, second > first);

    /*
     * Compare is writable and reads back exactly what was written;
     * unlike
     * Count, nothing changes it underneath.
     */
    rom_set_compare(0x12345678u);
    rom_assert_eq(4, 0x12345678u, rom_get_compare());

    /*
     * Writing Compare clears the pending timer interrupt. The baseline
     * is
     * measured after an arm ahead of Count, so it holds whether or not
     * the
     * model matches a Compare value the counter has already passed.
     */
    rom_set_compare(rom_get_count() + 0x40000000u);
    rom_assert_eq(5, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * Arm Compare ahead of Count and wait for Count to reach it. The
     * wait
     * watches for the crossing itself rather than for a fixed number of
     * turns,
     * so how fast this loop runs cannot decide whether the test passes:
     * Count
     * only moves one way, so the crossing is certain to arrive.
     *
     * The distance chosen is large for a reason. Reading Count and
     * writing
     * Compare are two separate instructions, and the emulated processor
     * can
     * be
     * held up between them for as long as the host takes to reschedule
     * it. A
     * distance smaller than that delay would let Count run past Compare
     * before
     * Compare was even written, which leaves the two to meet only after
     * Count
     * has wrapped the whole way round — minutes later. A distance of
     * four
     * million ticks is far beyond any scheduling delay and still under
     * a
     * tenth
     * of a second.
     */
    for (second = 0; second < 8; second++) {
        rom_set_count(0);
        rom_set_compare(TIMER_TARGET);
        first = rom_get_count();
        if (first < TIMER_TARGET) {
            /*
             * Keep the read that admitted this arm. A second read can
             * cross
             * the deadline after an otherwise valid arm.
             */
            break;
        }
        /*
         * The host stalled across the two arming writes; arm again.
         */
    }
    rom_assert(6, first < TIMER_TARGET);

    for (i = 0; i < POLL_LIMIT && rom_get_count() < TIMER_TARGET; i++) {
        /*
         * Reading Count is the whole body: it is what advances
         * observably.
         */
    }
    rom_assert(7, rom_get_count() >= TIMER_TARGET);

    cause = rom_get_cause();
    rom_assert_eq(8, ROM_CAUSE_IP7, cause & ROM_CAUSE_IP7);

    /*
     * Writing Compare again takes the pending interrupt away, whatever
     * value
     * is written.
     */
    rom_set_compare(rom_get_count() + 0x40000000u);
    rom_assert_eq(9, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * The mask bit that the manual names as the only way to turn the
     * timer
     * interrupt off has to be writable for that to be true.
     */
    status = rom_get_status();
    rom_set_status(status | ROM_ST_IM7);
    rom_assert_eq(10, ROM_ST_IM7, rom_get_status() & ROM_ST_IM7);
    rom_set_status(status & ~ROM_ST_IM7);
    rom_assert_eq(11, 0, rom_get_status() & ROM_ST_IM7);

    /*
     * Nothing above raised an exception.
     */
    rom_assert_eq(12, 0, rom_exc.count);

    /*
     * The comparator is continuous, so a write that leaves the two
     * registers
     * equal is a standing match: writing Count equal to the standing
     * Compare
     * must set the pending bit at once, even though Count then moves
     * past it.
     * The write anchors Count at exactly the compared value, which is
     * what
     * makes this the one equality a program can arrange
     * deterministically -
     * writing Compare equal to a running Count is a race no test can
     * win, so
     * that direction is implemented alongside this one but only this
     * one is
     * pinned.
     *
     * R10000 only: the fork leaves the other processor models' timer
     * with
     * QEMU's long-standing behaviour, where a write-time match waits a
     * full
     * wrap of the counter instead.
     */
    if (((rom_get_prid() >> 8) & 0xff) == 0x09) {
        /*
         * Park Count below the armed value so the baseline holds
         * whether or
         * not the model matches a Compare value the counter has passed.
         */
        rom_set_count(0);
        rom_set_compare(0x40000000u);
        rom_assert_eq(13, 0, rom_get_cause() & ROM_CAUSE_IP7);
        rom_set_count(0x40000000u);
        rom_assert_eq(14, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);

        /*
         * An ordinary Compare write still takes the pending bit away.
         */
        rom_set_compare(rom_get_count() + 0x40000000u);
        rom_assert_eq(15, 0, rom_get_cause() & ROM_CAUSE_IP7);

        rom_assert_eq(16, 0, rom_exc.count);

    }

    /*
     * A behind-Count Compare write leaves IP7 clear on each tested model.
     * For R14000 this is a QEMU compatibility choice, not an established
     * hardware fact. The former half-counter window asserted IP7 here and
     * on OpenBSD's Count - 1 disarm write (QEMU issue 626).
     */
    rom_set_count(0x00200000u);
    rom_set_compare(0x00100000u);
    rom_assert_eq(17, 0, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_compare(rom_get_count() + 0x40000000u);
    rom_assert_eq(18, 0, rom_get_cause() & ROM_CAUSE_IP7);
    rom_assert_eq(19, 0, rom_exc.count);

}
