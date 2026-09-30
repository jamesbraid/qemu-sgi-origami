/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check interrupt delivery after Status writes and ERET unmask a pending
 * level interrupt. See the R10000 User's Manual interrupt, Count/Compare,
 * and Status register sections.
 */

#include "rom.h"

const char rom_name[] = "intdeliver";

/*
 * Interrupt mask bit 7 of the Status register, the timer's mask.
 */
#define ROM_ST_IM7 (1u << 15)

/*
 * The same arming distance and poll bound the timer ROM uses, for the
 * same
 * reason: the waits end on their condition; the bound only converts a
 * counter that never advances into a failed assertion.
 */
#define TIMER_TARGET (1u << 22)
#define POLL_LIMIT   50000000

#define VEC_BOOTSTRAP_GENERAL (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)

/*
 * Unmask by writing the Status register, then wait, bounded, for the
 * delivery. The resume address is armed at the label past the wait, so
 * the
 * handler returns there; if nothing is delivered the counter runs out
 * and
 * control falls to the same label with the exception count unchanged.
 * The
 * counter lives in a compiler-chosen register, never $26 or $27, which
 * the
 * handler owns.
 */
#define UNMASK_AND_WAIT(unmasked)                                             \
    do {                                                                      \
        uint64_t bound__ = 1000000;                                           \
        uint32_t st__ = (unmasked);                                           \
        __asm__ __volatile__(                                                 \
            ".set push\n\t.set noreorder\n\t"                                 \
            "lui $26, %%hi(9f)\n\t"                                           \
            "daddiu $26, $26, %%lo(9f)\n\t"                                   \
            "sd $26, 0(%[res])\n\t"                                           \
            "mtc0 %[st], $12\n"                                               \
            "1:\tbeqz %[n], 9f\n\t"                                           \
            " daddiu %[n], %[n], -1\n\t"                                      \
            "b 1b\n\t"                                                        \
            " nop\n"                                                          \
            "9:\t.set pop"                                                    \
            : [n] "+r"(bound__)                                               \
            : [res] "r"(&rom_exc.resume), [st] "r"(st__)                      \
            : "$26", "memory");                                               \
    } while (0)

/*
 * Unmask by ERET: point EPC at the wait loop inside the block and
 * return
 * to it. The processor leaves the exception level at the ERET, so a
 * pending enabled interrupt is deliverable from the loop's first
 * instruction boundary on.
 */
#define ERET_AND_WAIT()                                                       \
    do {                                                                      \
        uint64_t bound__ = 1000000;                                           \
        __asm__ __volatile__(                                                 \
            ".set push\n\t.set noreorder\n\t"                                 \
            "lui $26, %%hi(9f)\n\t"                                           \
            "daddiu $26, $26, %%lo(9f)\n\t"                                   \
            "sd $26, 0(%[res])\n\t"                                           \
            "lui $26, %%hi(1f)\n\t"                                           \
            "daddiu $26, $26, %%lo(1f)\n\t"                                   \
            "dmtc0 $26, $14\n\t"                                              \
            "eret\n"                                                          \
            "1:\tbeqz %[n], 9f\n\t"                                           \
            " daddiu %[n], %[n], -1\n\t"                                      \
            "b 1b\n\t"                                                        \
            " nop\n"                                                          \
            "9:\t.set pop"                                                    \
            : [n] "+r"(bound__)                                               \
            : [res] "r"(&rom_exc.resume)                                      \
            : "$26", "memory");                                               \
    } while (0)

/*
 * Arm the timer and wait for the line, so IP7 is a pending level. The
 * wait
 * is on the line itself, not on the counter: the two arming writes are
 * separate instructions, and a host that stalls the emulated processor
 * between them for longer than the arming distance lets Count cross
 * Compare before Compare is written — the match is then a whole counter
 * wrap away, and a wait on the counter returns with no line up, failing
 * the caller for the host's sins. A lost arming window is detectable
 * (Count is already past the target right after the writes) and is
 * simply
 * re-armed; a line that fails to rise from a cleanly armed crossing is
 * not forgiven and falls through to the caller's assertion.
 */
static void raise_timer_line(void)
{
    int i, tries;

    for (tries = 0; tries < 8; tries++) {
        rom_set_count(0);
        rom_set_compare(TIMER_TARGET);
        if (rom_get_count() >= TIMER_TARGET) {
            /*
             * Stalled across the arming window. The Compare write above
             * also took down any stale line, so re-arm from scratch.
             */
            continue;
        }
        for (i = 0; i < POLL_LIMIT && !(rom_get_cause() & ROM_CAUSE_IP7);
             i++) {
        }
        return;
    }
}

/*
 * Park Compare far above Count so nothing crosses during a scenario,
 * and
 * take down a line already up. A stall between reading Count and
 * writing
 * Compare can let the parked value be crossed before it lands, which
 * leaves the line up — re-park until it is not. Bounded like every wait
 * here: a line that will not go down becomes the caller's assertion
 * failure, not a hang.
 */
static void park_timer(void)
{
    int tries;

    for (tries = 0; tries < 8; tries++) {
        rom_set_compare(rom_get_count() + 0x40000000u);
        if (!(rom_get_cause() & ROM_CAUSE_IP7)) {
            return;
        }
    }
}

void rom_main(void)
{
    uint32_t base;

    rom_exc_reset();

    /*
     * The baseline the scenarios build on: kernel mode with the
     * exception
     * level clear and interrupts off, the state the harness hands over.
     */
    base = rom_get_status() & ~(ROM_ST_IE | ROM_ST_IM7);
    rom_set_status(base);
    rom_assert_eq(1, 0, rom_get_status() &
                  (ROM_ST_IE | ROM_ST_EXL | ROM_ST_ERL));
    park_timer();
    rom_assert_eq(2, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * The line rises while IE is off: pending is observable, delivery
     * is
     * not. This is the state every scenario below starts from.
     */
    raise_timer_line();
    rom_assert_eq(3, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_assert_eq(4, 0, rom_exc.count);

    /*
     * Opening IE with the line already pending delivers it: exactly one
     * interrupt exception, at the general entry point, carrying code
     * zero
     * and the pending bit. The staged Status takes interrupts back off
     * on
     * the way out, so one delivery is all there can be.
     */
    rom_exc.status_set = base;
    UNMASK_AND_WAIT(base | ROM_ST_IE | ROM_ST_IM7);
    rom_assert_eq(5, 1, rom_exc.count);
    rom_assert_eq(6, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(7, ROM_EXC_INT, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(8, ROM_CAUSE_IP7, rom_exc.cause & ROM_CAUSE_IP7);

    /*
     * Writing Compare takes the line down again.
     */
    park_timer();
    rom_assert_eq(9, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * The same crossing with IE on but the IM bit closed: pending, not
     * delivered. Opening the IM bit alone is the unmask that delivers.
     */
    rom_set_status(base | ROM_ST_IE);
    raise_timer_line();
    rom_assert_eq(10, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_assert_eq(11, 1, rom_exc.count);

    rom_exc.status_set = base;
    UNMASK_AND_WAIT(base | ROM_ST_IE | ROM_ST_IM7);
    rom_assert_eq(12, 2, rom_exc.count);
    rom_assert_eq(13, ROM_EXC_INT, ROM_CAUSE_EXC(rom_exc.cause));

    park_timer();

    /*
     * The exception level masks even with IE and the IM bit open, and
     * ERET is the unmask: the pending line is taken at the loop the
     * ERET
     * returns to, through the same handler-and-resume path as above.
     */
    rom_set_status(base | ROM_ST_IE | ROM_ST_IM7 | ROM_ST_EXL);
    raise_timer_line();
    rom_assert_eq(14, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_assert_eq(15, 2, rom_exc.count);

    rom_exc.status_set = base;
    ERET_AND_WAIT();
    rom_assert_eq(16, 3, rom_exc.count);
    rom_assert_eq(17, ROM_EXC_INT, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * Leave nothing pending and nothing open behind.
     */
    rom_set_status(base);
    park_timer();
    rom_assert_eq(18, 0, rom_get_cause() & ROM_CAUSE_IP7);
}
