/* SPDX-License-Identifier: BSD-3-Clause */
/* Check R10000 Random, Cause, and LLAddr register behavior. */

#include "rom.h"

const char rom_name[] = "cp0misc";

/* Random's upper bound for a 64-entry TLB. */
#define RANDOM_UPPER 63

/*
 * Cause register fields beyond the ones the harness names.
 */
#define CAUSE_RESERVED_27_16 (0x0fffu << 16)
#define CAUSE_IP1_0          (3u << 8)

/* Load-linked operand and non-aliasing physical-address sentinel. */
#define LLADDR_SENTINEL 0x5aa55aa5u

/*
 * Volatile keeps the load-linked test operand in memory.
 */
static volatile uint32_t llword;

void rom_main(void)
{
    uint32_t i;

    rom_exc_reset();

    /*
     * Interrupts stay off: a Cause bit is only ever observed as a bit.
     */
    rom_set_status(rom_get_status() & ~ROM_ST_IE);

    /* Reset establishes the initial Random value before any TLB lookup. */
    rom_assert_eq(1, RANDOM_UPPER, rom_get_random());
    {
        /*
         * Reads advance Random; its cycle-derived step is not fixed.
         */
        uint32_t second = rom_get_random();

        rom_assert_eq(2, 1, second <= RANDOM_UPPER && second != RANDOM_UPPER);
    }

    /*
     * Writing the Wired register reloads the upper bound.
     */
    rom_set_wired(10);
    rom_assert_eq(3, RANDOM_UPPER, rom_get_random());

    /* With Wired at 60, Random must remain in the four-entry range. */
    rom_set_wired(60);
    for (i = 0; i < 8; i++) {
        uint32_t r = rom_get_random();

        rom_assert_eq(4 + i, 1, r >= 60 && r <= RANDOM_UPPER);
    }

    /*
     * Restoring the floor is itself a Wired write, so the bound
     * reloads.
     */
    rom_set_wired(0);
    rom_assert_eq(12, RANDOM_UPPER, rom_get_random());

    /*
     * Separate reads so delayed updates are visible without assuming a
     * fixed decrement. This catches the former unit-step approximation.
     */
    {
        uint32_t anchor, before, after;

        __asm__ volatile (
            ".set push\n\t"
            ".set noreorder\n\t"
            "mfc0 %0, $1\n\t"
            ".rept 10\n\t"
            "nop\n\t"
            ".endr\n\t"
            "mfc0 %1, $1\n\t"
            "mfc0 %2, $1\n\t"
            ".set pop"
            : "=r" (anchor), "=r" (before), "=r" (after));
        (void)anchor;
        rom_assert_eq(13, 1, ((before - after) & 63) != 1);
    }

    /*
     * Only software interrupt bits are writable; 23:22 and 27:16 are
     * reserved on R10000. Keep interrupts disabled so writes are observed
     * as bits only.
     */
    rom_set_cause(rom_get_cause() | CAUSE_RESERVED_27_16);
    rom_assert_eq(14, 0, rom_get_cause() & CAUSE_RESERVED_27_16);

    rom_set_cause(rom_get_cause() | CAUSE_IP1_0);
    rom_assert_eq(15, CAUSE_IP1_0, rom_get_cause() & CAUSE_IP1_0);
    rom_set_cause(rom_get_cause() & ~CAUSE_IP1_0);
    rom_assert_eq(16, 0, rom_get_cause() & CAUSE_IP1_0);

    /*
     * LLAddr is software-visible storage here. A load-linked instruction
     * and exception return must not overwrite its sentinel.
     */
    rom_set_lladdr(LLADDR_SENTINEL);
    rom_assert_eq(18, LLADDR_SENTINEL, rom_get_lladdr());

    __asm__ __volatile__("ll $0, 0(%0)" : : "r"(&llword) : "memory");
    rom_assert_eq(19, LLADDR_SENTINEL, rom_get_lladdr());

    rom_exc_reset();
    ROM_TRY("syscall");
    rom_assert_eq(20, 1, rom_exc.count);
    rom_assert_eq(21, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(22, LLADDR_SENTINEL, rom_get_lladdr());

    /*
     * Nothing ran off the expected path.
     */
    rom_assert_eq(23, 0, rom_exc.count - 1);
}
