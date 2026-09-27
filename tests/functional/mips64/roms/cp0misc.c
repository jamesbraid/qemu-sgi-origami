/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Coprocessor 0 register semantics the R10000 defines away from the
 * R4000
 * lineage: the Random register's reload-and-decrement behaviour, the
 * Cause
 * register's reserved field, and the load-linked address register that
 * no
 * longer records load-linked addresses.
 *
 * None of these is exotic. The Random register is what the manual
 * offers a
 * diagnostic for checking the processor is alive; the Cause register is
 * read
 * and written by every kernel's interrupt path; and a register that
 * changes
 * underneath software that treats it as scratch is exactly the kind of
 * divergence that corrupts a context switch nobody can reproduce.
 *
 * Oracle: the MIPS R10000 User's Manual version 2.0.
 *  - Its Random register section (section 14.2) says the register
 * decrements
 *    when an instruction graduates, ranges between the Wired register's
 * value
 *    and the number of entries minus one, is set to that upper bound by
 * reset
 *    and by every write to the Wired register, and is readable "to
 * verify
 *    proper operation of the processor".
 *  - Its Cause register section (section 14.11, Figure 14-13, Table
 * 14-12)
 *    makes every bit except the two software interrupt bits read-only,
 * and
 *    reserves bits 27:16 outright: "Must be written as zeroes, and
 * returns
 *    zeroes when read". The R10000 has none of the later architecture's
 *    fields there.
 *  - Its LLAddr register section (section 14.15): "Physical addresses
 * for
 *    Load Link instructions are no longer written into this register.
 * LLAddr
 *    is implemented as a read/write scratch register used for NT
 *    compatibility."
 *
 * The model approximates instruction graduation with elapsed
 * virtual-clock
 * nanoseconds. This is a deliberate approximation, protected by a
 * regression
 * check. It is not a hardware requirement that every observed
 * decrement exceed one: wrapping can produce a unit decrement after any
 * delay.
 * The suite runs this ROM with deterministic instruction counting so
 * its
 * elapsed-time check controls the interval and avoids host scheduling
 * noise.
 *
 * The R10000 is the only processor this ROM runs against: the fork
 * leaves the
 * other models' Cause writability and load-linked bookkeeping as QEMU
 * has
 * always had them, and the Random assertions encode this processor's
 * 64-entry
 * buffer.
 */

#include "rom.h"

const char rom_name[] = "cp0misc";

/*
 * The upper bound of the Random register: the entry count minus one.
 */
#define RANDOM_UPPER 63

/*
 * Cause register fields beyond the ones the harness names.
 */
#define CAUSE_RESERVED_27_16 (0x0fffu << 16)
#define CAUSE_IP1_0          (3u << 8)

/*
 * A word for the load-linked probe to link against, and a sentinel that
 * no
 * physical address in the machine's first terabyte can alias.
 */
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

    /*
     * Reset left the Random register at its upper bound, and nothing
     * between
     * reset and here consults it — the startup code takes no refills —
     * so the
     * first read returns the bound.
     */
    rom_assert_eq(1, RANDOM_UPPER, rom_get_random());
    {
        /*
         * A consultation moves the register; the step size is
         * cycle-derived
         * and not pinned, only the bounds are.
         */
        uint32_t second = rom_get_random();

        rom_assert_eq(2, 1, second <= RANDOM_UPPER && second != RANDOM_UPPER);
    }

    /*
     * Writing the Wired register reloads the upper bound.
     */
    rom_set_wired(10);
    rom_assert_eq(3, RANDOM_UPPER, rom_get_random());

    /*
     * The register never leaves the range the two bounds pin: with the
     * floor at 60 every read sits in the four-value window however the
     * cycle-derived step lands.
     */
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
     * The icount configuration advances eight virtual nanoseconds per
     * instruction.
     * Ten nops separate two Random reads, so the delayed update cannot
     * wrap
     * to a unit decrement. Reads return the pre-update value: the third
     * read
     * observes the update calculated by the second. The old unit-step
     * approximation fails this deliberate approximation regression
     * check.
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
     * The Cause register: only the two software interrupt bits take a
     * write,
     * and the reserved field 27:16 returns zeroes however it is
     * written. The
     * bits at 23 and 22 are where a model built from the later
     * architecture
     * would give itself away, because that architecture defines
     * vectoring and
     * watch-deferral controls there; on the R10000 they are reserved
     * zeroes
     * like the rest of the field.
     *
     * The software interrupt bits are exercised with the interrupt
     * enable off
     * and both interrupts masked, so posting them is observable only as
     * bits.
     */
    rom_set_cause(rom_get_cause() | CAUSE_RESERVED_27_16);
    rom_assert_eq(14, 0, rom_get_cause() & CAUSE_RESERVED_27_16);

    rom_set_cause(rom_get_cause() | CAUSE_IP1_0);
    rom_assert_eq(15, CAUSE_IP1_0, rom_get_cause() & CAUSE_IP1_0);
    rom_set_cause(rom_get_cause() & ~CAUSE_IP1_0);
    rom_assert_eq(16, 0, rom_get_cause() & CAUSE_IP1_0);

    /*
     * The load-linked address register holds what software last put
     * there and
     * nothing else: the manual says physical addresses for load-linked
     * instructions "are no longer written into this register", leaving
     * a
     * plain
     * read/write scratch word. So a sentinel must survive both of the
     * writers
     * the R4000 lineage had - a load-linked instruction, and the
     * exception
     * return that every handled exception ends with. The link itself
     * lives
     * elsewhere; breaking it does not touch this register.
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
