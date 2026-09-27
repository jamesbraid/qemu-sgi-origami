/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Hub's interrupt path, from a pending bit to a processor line.
 *
 * This exists because the register tests cannot see the thing that
 * matters. A
 * qtest drives the machine's memory with no processor executing, so it
 * can
 * read
 * a pending bank back and can never observe whether a line was raised —
 * an
 * access that faulted and an access answered with zero look identical
 * from
 * there, and a line that was asserted looks like nothing at all. The
 * half of
 * this unit's contract that decides whether a guest ever gets an
 * interrupt is
 * invisible to every test we had.
 *
 * A ROM has a processor. So the line is checked the way the processor
 * sees
 * it:
 * as a bit in the Cause register.
 *
 * The method is the timer ROM's, for the same reason it uses it.
 * Interrupts
 * are
 * left globally disabled, so a raised line is observed as a pending
 * Cause bit
 * rather than by taking an exception. That removes the handler, the
 * arming
 * and
 * the question of where control resumes, and leaves one thing under
 * test.
 *
 * What each assertion is for:
 *
 *   - A pending bit whose mask is clear raises nothing. Pending is
 * node-wide
 *     state and masking is per processor, so the two are separate and a
 * model
 *     that raised on the pending write alone would pass every register
 * test.
 *   - Setting the mask afterwards raises the line with no further write
 * to
 * the
 *     pending state. This is the assertion that says the output is
 * recomputed
 *     rather than raised at the moment a bit is written, and it is the
 * one no
 *     qtest can make.
 *   - Clearing the last pending bit drops the line. The lines are
 *     level-triggered; a model that raised and never recomputed would
 * leave
 *     this asserted, which a guest experiences as an interrupt storm
 * rather
 *     than as a lost interrupt, and which is much harder to read from a
 *     transcript than a missing interrupt is.
 *   - The two banks reach different processor lines and do not disturb
 * each
 *     other, because an interrupt delivered to the wrong bank's handler
 * is a
 *     plausible-looking failure rather than an obvious one.
 *
 * Register layout is from the Hub processor interface datasheet: the
 * interrupt-modify register at offset 0x90 takes a seven-bit interrupt
 * number
 * and a bit saying whether to set the named pending bit or clear it,
 * with
 * numbers 0 through 63 the first pending bank and 64 through 127 the
 * second.
 * The first bank raises the processor's hardware interrupt 0, which is
 * Cause
 * bit 10, and the second raises hardware interrupt 1, Cause bit 11.
 *
 * The interrupt numbers used here are above the thirteen the hardware
 * assigns,
 * so nothing else in the machine drives them.
 *
 * **This runs against the R10000 alone, and the reason is not the
 * Hub.** The
 * Hub is the machine's and its registers behave the same whatever is
 * fetching
 * from them — but reaching them at all goes through the 64-bit unmapped
 * space
 * with a non-zero uncached attribute, and that attribute is an
 * R10000-family
 * extension: the bits it occupies are not attribute bits on an R4000,
 * which
 * takes an address error on the same address. So a machine-level test
 * of a
 * machine-level unit is still processor-specific, because the only
 * route to
 * the
 * unit is.
 *
 * That was found by listing it for both and watching the R4000 refuse
 * the
 * first
 * store, rather than by reasoning about it — the harness reported the
 * address,
 * which named the cause.
 */

#include "rom.h"

const char rom_name[] = "hubint";

/*
 * The Hub register window, through the crosstalk space's uncached view:
 * the
 * space that means crosstalk I/O, node zero, and widget 1's small
 * window,
 * which
 * is the address a processor reaches its own Hub at without first
 * learning
 * which node it is on.
 */
#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))

#define CCA_UNCACHED    2
#define ATTR_XTALK      1               /* the attribute that means crosstalk */
#define HUB_WINDOW      0x01000000ull   /* widget 1's small window, node zero */

#define HUB_REG(off)    XKPHYS(CCA_UNCACHED, ATTR_XTALK, HUB_WINDOW + (off))

#define PI_INT_MODIFY   0x000090
#define PI_INT_PEND0    0x000098
#define PI_INT_PEND1    0x0000a0
#define PI_INT_MASK0_A  0x0000a8
#define PI_INT_MASK1_A  0x0000b0

#define INT_MODIFY_SET  0x100u
#define INT_BANK_BITS   64

/*
 * Cause register bits 10 and 11: the two lines this unit drives.
 */
#define CAUSE_BANK0     (1u << 10)
#define CAUSE_BANK1     (1u << 11)

/*
 * Two numbers above every bit the hardware assigns for itself.
 */
#define NUMBER_BANK0    12
#define NUMBER_BANK1    70

static void hub_write(unsigned int off, uint64_t val)
{
    /*
     * Volatile preserves each required MMIO access.
     */
    *(volatile uint64_t *)(unsigned long)HUB_REG(off) = val;
}

static uint64_t hub_read(unsigned int off)
{
    /*
     * Volatile preserves each required MMIO access.
     */
    return *(volatile uint64_t *)(unsigned long)HUB_REG(off);
}

static void int_set(unsigned int number)
{
    hub_write(PI_INT_MODIFY, INT_MODIFY_SET | number);
}

static void int_clear(unsigned int number)
{
    hub_write(PI_INT_MODIFY, number);
}

void rom_main(void)
{
    uint64_t bank0 = 1ull << NUMBER_BANK0;
    uint64_t bank1 = 1ull << (NUMBER_BANK1 - INT_BANK_BITS);

    /*
     * Interrupts off for the whole ROM, so every line this unit raises
     * is
     * observed as a pending Cause bit and never as an exception.
     * Nothing here
     * wants control transferred; the question is only whether the line
     * is up.
     */
    rom_set_status(rom_get_status() & ~ROM_ST_IE);

    /*
     * And 64-bit kernel addressing on, because the Hub's window is
     * reached
     * through the 64-bit unmapped space and nothing below it. Without
     * this
     * every access here takes an address error rather than reaching the
     * Hub —
     * which is what the harness reported on the first attempt, at the
     * first
     * store, naming the address.
     */
    rom_set_status(rom_get_status() | ROM_ST_KX);

    /*
     * Start from nothing masked and nothing pending, whatever reset
     * left.
     */
    hub_write(PI_INT_MASK0_A, 0);
    hub_write(PI_INT_MASK1_A, 0);
    int_clear(NUMBER_BANK0);
    int_clear(NUMBER_BANK1);

    rom_assert_eq(1, 0, hub_read(PI_INT_PEND0));
    rom_assert_eq(2, 0, hub_read(PI_INT_PEND1));
    rom_assert_eq(3, 0, rom_get_cause() & (CAUSE_BANK0 | CAUSE_BANK1));

    /*
     * A pending bit with its mask clear: pending, and invisible to the
     * processor. A model that raised the line on the pending write
     * would fail
     * here, and would pass every test that only reads the pending bank
     * back.
     */
    int_set(NUMBER_BANK0);
    rom_assert_eq(4, bank0, hub_read(PI_INT_PEND0));
    rom_assert_eq(5, 0, rom_get_cause() & CAUSE_BANK0);

    /*
     * Now the mask, and nothing else. The line has to come up without
     * any
     * further write to the pending state — the output follows the
     * pending
     * bits
     * and the masks together, so it is recomputed on a mask change and
     * not
     * only
     * on a pending one. No qtest can make this assertion.
     */
    hub_write(PI_INT_MASK0_A, bank0);
    rom_assert_eq(6, bank0, hub_read(PI_INT_PEND0));
    rom_assert_eq(7, CAUSE_BANK0, rom_get_cause() & CAUSE_BANK0);

    /*
     * And the other bank's line is not disturbed by the first bank's.
     */
    rom_assert_eq(8, 0, rom_get_cause() & CAUSE_BANK1);

    /*
     * The second bank reaches the second line. Interrupt number 70 is
     * bit 6
     * of
     * the second bank: a model masking the number to six bits would put
     * it in
     * the first bank instead, where it would raise a line that is
     * already up
     * and go unnoticed here — so the first bank's pending state is
     * asserted
     * unchanged as well as the second's line coming up.
     */
    hub_write(PI_INT_MASK1_A, bank1);
    int_set(NUMBER_BANK1);
    rom_assert_eq(9, bank1, hub_read(PI_INT_PEND1));
    rom_assert_eq(10, bank0, hub_read(PI_INT_PEND0));
    rom_assert_eq(11, CAUSE_BANK1, rom_get_cause() & CAUSE_BANK1);

    /*
     * Level-triggered, which is the whole of the output contract.
     * Clearing
     * the
     * last pending bit a processor can see drops the line, with no
     * acknowledgement anywhere and nothing else written.
     */
    int_clear(NUMBER_BANK0);
    rom_assert_eq(12, 0, hub_read(PI_INT_PEND0));
    rom_assert_eq(13, 0, rom_get_cause() & CAUSE_BANK0);

    /*
     * The second line is still up: dropping one did not drop both.
     */
    rom_assert_eq(14, CAUSE_BANK1, rom_get_cause() & CAUSE_BANK1);

    /*
     * Masking is the other way to drop a line, and it leaves the bit
     * pending
     * —
     * which is how an operating system implements interrupt priority
     * levels,
     * rewriting masks on every level change without disturbing what is
     * pending.
     */
    hub_write(PI_INT_MASK1_A, 0);
    rom_assert_eq(15, bank1, hub_read(PI_INT_PEND1));
    rom_assert_eq(16, 0, rom_get_cause() & CAUSE_BANK1);

    int_clear(NUMBER_BANK1);
    rom_assert_eq(17, 0, hub_read(PI_INT_PEND1));
}
