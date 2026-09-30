/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check that timer interrupts arrive while a branch loop runs under
 * instruction counting, including branch delay-slot boundaries. Run
 * under -icount; otherwise the instruction-budget case is inactive.
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
