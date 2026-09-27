/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A timer interrupt must reach a processor spinning in a branch loop.
 *
 * On hardware this is trivially true: a processor that is executing
 * instructions has a clock that moves, so an armed timer deadline
 * arrives
 * however tight the loop it interrupts. The ROM pins that property
 * under the
 * emulator's deterministic execution mode (-icount), where instruction
 * budgets bound each translation block against the next timer deadline
 * and a
 * branch is never split from its delay slot. Those two rules meet
 * exactly
 * when the budget boundary lands on a branch, and a block extended for
 * its
 * delay slot must still retire — a model that refuses it stops
 * executing and
 * stops virtual time with it, forever, at the first loop whose
 * alignment
 * collides.
 *
 * Each phase arms the CP0 timer a short distance ahead, opens the
 * interrupt,
 * and spins in a bare branch loop until the handler fires. The phases
 * repeat
 * the loop at different instruction alignments and loop lengths:
 * budgets
 * count down in whole loop bodies, so which alignment lands the
 * boundary on
 * the branch depends on parities the guest cannot know, and covering
 * both
 * parities of the two-instruction loop guarantees one phase collides.
 * The
 * three-instruction phases cover the residues of the longer body the
 * same
 * way.
 *
 * Nothing here is specific to the R10000 — every MIPS branch carries a
 * delay
 * slot — so the ROM runs against QEMU's R4000 model as well. The suite
 * runs
 * this ROM under -icount; without that flag the budget machinery is
 * idle and
 * every phase passes vacuously.
 */

#include "rom.h"

const char rom_name[] = "icountslot";

/*
 * Interrupt mask bit 7 of the Status register, the timer's mask.
 */
#define ROM_ST_IM7 (1u << 15)

/*
 * How far ahead of Count each phase arms Compare. Far enough that the
 * loop
 * reaches its steady translation block before the deadline's budget
 * shrinks,
 * near enough that five phases stay quick.
 */
#define SLOT_TARGET 0x4000u

/*
 * One spin phase: arm the deadline, open the interrupt, spin until the
 * handler resumes past the loop, then assert exactly one interrupt
 * arrived.
 * The loop body is given whole, because its instruction alignment is
 * the
 * thing under test and a shared body would share one alignment.
 */
#define SLOT_PHASE(n, loop)                                                   \
    do {                                                                      \
        uint32_t on;                                                          \
                                                                              \
        rom_set_count(0);                                                     \
        rom_set_compare(SLOT_TARGET);                                         \
        rom_exc_reset();                                                      \
        rom_exc.status_set = rom_get_status() & ~(ROM_ST_IE | ROM_ST_IM7);    \
        on = rom_get_status() | ROM_ST_IE | ROM_ST_IM7;                       \
        ROM_TRY("mtc0 %[on], $12\n\t" loop, [on] "r"(on));                    \
        rom_set_status(rom_get_status() & ~(ROM_ST_IE | ROM_ST_IM7));         \
        rom_assert_eq(n, 1, rom_exc.count);                                   \
        rom_assert_eq(n + 1, ROM_EXC_INT, ROM_CAUSE_EXC(rom_exc.cause));      \
        rom_set_compare(rom_get_count() + 0x40000000u);                       \
    } while (0)

void rom_main(void)
{
    rom_exc_reset();
    rom_set_status(rom_get_status() & ~ROM_ST_IE);

    /*
     * The two-instruction loop, at both entry alignments.
     */
    SLOT_PHASE(1, "1:\tb 1b\n\tnop");
    SLOT_PHASE(3, "nop\n"
                  "1:\tb 1b\n\tnop");

    /*
     * The three-instruction loop, at all three entry alignments.
     */
    SLOT_PHASE(5, "1:\tnop\n\tb 1b\n\tnop");
    SLOT_PHASE(7, "nop\n"
                  "1:\tnop\n\tb 1b\n\tnop");
    SLOT_PHASE(9, "nop\n\tnop\n"
                  "1:\tnop\n\tb 1b\n\tnop");
}
