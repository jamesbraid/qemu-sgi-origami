/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check the R10000 FrameMask, Diagnostic, and performance counter
 * registers. See the MIPS R10000 User's Manual version 2.0.
 */

#include "rom.h"

const char rom_name[] = "diagregs";

/*
 * The performance counters and their event specifier registers all live
 * behind
 * coprocessor 0 register 25, chosen by a selector in the low bits of
 * the move
 * instruction. Decoding the manual's move instruction figures: the
 * selector
 * is
 * instruction bits 5:0, bit 0 picks a counter when set and an event
 * specifier
 * register when clear, and bits 5:1 pick which of the two. So selector
 * 0 is
 * event specifier 0, 1 is counter 0, 2 is event specifier 1, and 3 is
 * counter
 * 1
 * — which the manual's coprocessor 0 instruction encoding table
 * confirms by
 * listing the selector 01 as a move to performance counter 0 and 11 as
 * a move
 * to performance counter 1.
 *
 * The three-operand form of the coprocessor 0 move instructions is not
 * part
 * of
 * the MIPS IV assembly these ROMs are built for, so each access is
 * emitted as
 * an instruction word: register 8 carries the value, register 25 is the
 * coprocessor register, and the selector goes in the low bits.
 */
#define PERF_SPEC0 0
#define PERF_CNT0  1
#define PERF_SPEC1 2
#define PERF_CNT1  3

/*
 * The selector names below are themselves macros, so they have to be
 * expanded
 * before they are turned into assembly text.
 */
#define PERF_TEXT_(x) #x
#define PERF_TEXT(x) PERF_TEXT_(x)

#define PERF_READ(sel)                                                        \
    ({                                                                        \
        uint64_t out__;                                                       \
        __asm__ __volatile__(                                                 \
            ".word (0x40000000 | (8 << 16) | (25 << 11) | "                   \
            PERF_TEXT(sel) ")\n\t"                                            \
            "move %0, $8"                                                     \
            : "=r"(out__) : : "$8");                                          \
        (uint32_t)out__;                                                      \
    })

#define PERF_WRITE(sel, val)                                                  \
    __asm__ __volatile__(                                                     \
        "move $8, %0\n\t"                                                     \
        ".word (0x40800000 | (8 << 16) | (25 << 11) | "                       \
        PERF_TEXT(sel) ")"                                                    \
        : : "r"((uint64_t)(uint32_t)(val)) : "$8")

/*
 * A value for an event specifier register that means something rather
 * than
 * being a bit pattern: the control register format puts the event
 * number in
 * bits 8:5, the interrupt enable in bit 4, and the mode bits selecting
 * which
 * privilege levels are counted in bits 3:0. Event 1 counted in kernel
 * mode
 * with
 * no interrupt is therefore an event field of 1 and the kernel mode bit
 * set.
 */
#define PERF_EVENT_SHIFT 5
#define PERF_MODE_KERNEL (1u << 1)
#define PERF_SPEC_VALUE  ((1u << PERF_EVENT_SHIFT) | PERF_MODE_KERNEL)

/*
 * Diagnostic register fields, from the R10000 manual's Diagnostic
 * register
 * format (Figure 14-21). Only the branch prediction mode is writable;
 * the
 * rest
 * report branch cache and instruction-translation-buffer state, and the
 * gaps
 * between them are reserved.
 *
 *   63:52 reserved   51:48 miss counter   47:32 reserved   31:28 branch
 * stack
 *   index   27:23 reserved   22 disable branch return cache   21 that
 * cache
 *   valid   20 written   19 hit   18 mispredicted   17:16 prediction
 * mode
 *   15:14 prediction state   13:12 reserved   11:3 prediction table
 * index
 *   2 reserved   1:0 prediction table operation
 */
#define DIAG_BPMODE_SHIFT 16
#define DIAG_BPMODE_MASK  (3ull << DIAG_BPMODE_SHIFT)
#define DIAG_BPSTATE_MASK (3ull << 14)
#define DIAG_BPIDX_MASK   (0x1ffull << 3)
#define DIAG_BPOP_MASK    (3ull << 0)

/*
 * Every bit the manual's Diagnostic register format leaves reserved. Of
 * these
 * it says: "Reserved. Must be written as zeroes, and returns zeroes
 * when
 * read."
 * That is the one statement about this register firm enough to hold a
 * model
 * to
 * across its whole width, so it is what the checks below lean on.
 */
#define DIAG_RESERVED (0xfff0ffff00000000ull | (0x1full << 23) | \
                       (3ull << 12) | (1ull << 2))

void rom_main(void)
{
    /*
     * The frame mask holds sixteen bits, masking bits 33:18 of the low
     * halves
     * of a translation buffer entry, which are physical address bits
     * 39:24.
     * The
     * manual states the remaining bits are ignored on write and read as
     * zeroes,
     * and that the register contains zero after reset. Whether the
     * masking
     * actually alters a buffer write is not checked here: the framemask
     * ROM
     * asserts that behaviour, and this one asserts the register itself.
     */
    rom_assert_eq(1, 0, rom_get_framemask());

    rom_set_framemask(0xffffffffu);
    rom_assert_eq(2, 0x0000ffffu, rom_get_framemask());

    rom_set_framemask(0);
    rom_assert_eq(3, 0, rom_get_framemask());

    /*
     * The diagnostic register is not a scratch register, which is the
     * mistake
     * worth guarding against. The manual gives it twelve fields and
     * states of
     * them that "All fields are read-only (all writes are ignored)",
     * naming
     * one
     * exception: "BPMode: this is a read-write field for branch
     * prediction
     * algorithm control", with values 00 for the two-bit counter
     * scheme, 01
     * for
     * always predict not taken, 10 for always predict taken and 11 for
     * predicting backward branches taken and forward branches not, and
     * "The
     * default mode is 00 on processor reset".
     *
     * So the contract checked here is that the writable field is
     * writable,
     * that
     * it comes out of reset holding zero, and that every bit the manual
     * reserves
     * reads zero however hard it is written to. A model that made the
     * whole
     * register a 64-bit scratch would fail the reserved-bit checks, and
     * one
     * that
     * hardcoded a read of zero would fail the prediction mode checks.
     *
     * Linux is the reason a hardcoded zero matters in practice: its
     * startup
     * reads this register, sets a bit and writes it back on an R12000
     * and an
     * R14000. It does not do so on an R10000, and the bit it sets — bit
     * 26 —
     * falls inside a field this manual reserves, so it belongs to a
     * later
     * member
     * of the family rather than to this one and is not asserted here.
     * Nothing
     * in
     * Linux reads the bit back to confirm it, either.
     *
     * The register is 64 bits wide and Linux reaches it with the
     * doubleword
     * coprocessor moves, so every access below is at that width.
     */
    rom_assert_eq(4, 0, rom_get_brdiag() & DIAG_RESERVED);
    rom_assert_eq(5, 0, rom_get_brdiag() & DIAG_BPMODE_MASK);

    rom_set_brdiag(0ull << DIAG_BPMODE_SHIFT);
    rom_assert_eq(6, 0ull << DIAG_BPMODE_SHIFT,
                  rom_get_brdiag() & DIAG_BPMODE_MASK);
    rom_set_brdiag(1ull << DIAG_BPMODE_SHIFT);
    rom_assert_eq(7, 1ull << DIAG_BPMODE_SHIFT,
                  rom_get_brdiag() & DIAG_BPMODE_MASK);
    rom_set_brdiag(2ull << DIAG_BPMODE_SHIFT);
    rom_assert_eq(8, 2ull << DIAG_BPMODE_SHIFT,
                  rom_get_brdiag() & DIAG_BPMODE_MASK);
    rom_set_brdiag(3ull << DIAG_BPMODE_SHIFT);
    rom_assert_eq(9, 3ull << DIAG_BPMODE_SHIFT,
                  rom_get_brdiag() & DIAG_BPMODE_MASK);

    /*
     * Writing every bit still leaves the reserved ones reading zero,
     * and
     * lands
     * the prediction mode on its highest value rather than losing the
     * write.
     */
    rom_set_brdiag(~0ull);
    rom_assert_eq(10, 0, rom_get_brdiag() & DIAG_RESERVED);
    rom_assert_eq(11, DIAG_BPMODE_MASK, rom_get_brdiag() & DIAG_BPMODE_MASK);

    rom_set_brdiag(0);
    rom_assert_eq(12, 0, rom_get_brdiag() & DIAG_BPMODE_MASK);
    rom_assert_eq(13, 0, rom_get_brdiag() & DIAG_RESERVED);

    /*
     * Three more fields software writes. The blanket statement that
     * every
     * field is read-only is contradicted by the same section's own
     * account of
     * how a branch prediction table access is made: it gives a code
     * sequence
     * that moves "the Index and the BPT read operation into the Idx and
     * BPOp
     * field of the Diagnostic register" with a write and then reads the
     * state
     * back, and it describes the state field as holding "the 2-bit
     * state to
     * read/write when a branch prediction table read/write operation is
     * executed". Where the general statement and the specific procedure
     * disagree, the procedure is what a diagnostic program actually
     * follows.
     *
     * The table behind those fields holds 512 two-bit entries, indexed
     * by
     * VA[11:3] of a branch, and the operation field names what a write
     * to
     * the register starts: 00 reads the indexed entry into the state
     * field,
     * 01 writes the state field into the indexed entry, and 10 and 11
     * initialize a whole line — eight entries named by the upper six
     * index
     * bits, the lower three ignored — to all zeroes or all ones. The
     * manual's
     * own read sequence separates the operation from the read-back with
     * a
     * jump, because on hardware the table access competes with
     * instruction
     * fetch; a model performs it at once, which is the state the
     * sequence is
     * written to wait for, so a diagnostic following the manual works
     * unchanged.
     *
     * What no assertion here can check is the table's other writer: on
     * real
     * hardware every verified conditional branch updates an entry, and
     * this
     * model predicts nothing, so entries hold what the operations below
     * put
     * there and nothing else. The manual's own sequence avoids
     * conditional
     * branches for exactly this reason, so a diagnostic that follows it
     * sees
     * no difference.
     */
#define DIAG_BPSTATE(s) ((uint64_t)(s) << 14)
#define DIAG_BPIDX(i)   ((uint64_t)(i) << 3)
#define DIAG_BPOP_READ   0ull
#define DIAG_BPOP_WRITE  1ull
#define DIAG_BPOP_ZEROES 2ull
#define DIAG_BPOP_ONES   3ull

    /*
     * A table write keeps every written field in the register.
     */
    rom_set_brdiag(DIAG_BPSTATE(2) | DIAG_BPIDX(5) | DIAG_BPOP_WRITE);
    rom_assert_eq(14, DIAG_BPSTATE(2) | DIAG_BPIDX(5) | DIAG_BPOP_WRITE,
                  rom_get_brdiag());

    /*
     * A second entry, so a read-back cannot pass by echoing the last
     * write.
     */
    rom_set_brdiag(DIAG_BPSTATE(1) | DIAG_BPIDX(6) | DIAG_BPOP_WRITE);

    /*
     * A table read replaces the state field with the indexed entry —
     * the
     * state written alongside the read operation is the value the read
     * overwrites, so it cannot be what comes back — and leaves the
     * index
     * and operation fields as written.
     */
    rom_set_brdiag(DIAG_BPSTATE(3) | DIAG_BPIDX(5) | DIAG_BPOP_READ);
    rom_assert_eq(15, DIAG_BPSTATE(2) | DIAG_BPIDX(5), rom_get_brdiag());
    rom_set_brdiag(DIAG_BPIDX(6) | DIAG_BPOP_READ);
    rom_assert_eq(16, DIAG_BPSTATE(1) | DIAG_BPIDX(6), rom_get_brdiag());

    /*
     * Line initialization: the upper six index bits name the line, the
     * lower
     * three are ignored, so initializing with a mid-line index still
     * writes
     * all eight entries of line zero. Entry eight sits in the next line
     * and
     * must keep its value.
     */
    rom_set_brdiag(DIAG_BPSTATE(1) | DIAG_BPIDX(8) | DIAG_BPOP_WRITE);
    rom_set_brdiag(DIAG_BPIDX(4) | DIAG_BPOP_ONES);
    rom_set_brdiag(DIAG_BPIDX(5) | DIAG_BPOP_READ);
    rom_assert_eq(17, DIAG_BPSTATE(3) | DIAG_BPIDX(5), rom_get_brdiag());
    rom_set_brdiag(DIAG_BPIDX(0) | DIAG_BPOP_READ);
    rom_assert_eq(18, DIAG_BPSTATE(3) | DIAG_BPIDX(0), rom_get_brdiag());
    rom_set_brdiag(DIAG_BPIDX(8) | DIAG_BPOP_READ);
    rom_assert_eq(19, DIAG_BPSTATE(1) | DIAG_BPIDX(8), rom_get_brdiag());

    rom_set_brdiag(DIAG_BPIDX(7) | DIAG_BPOP_ZEROES);
    rom_set_brdiag(DIAG_BPIDX(5) | DIAG_BPOP_READ);
    rom_assert_eq(20, DIAG_BPIDX(5), rom_get_brdiag());

    /*
     * The last line, so the index arithmetic covers the whole table.
     */
    rom_set_brdiag(DIAG_BPIDX(511) | DIAG_BPOP_ONES);
    rom_set_brdiag(DIAG_BPIDX(511) | DIAG_BPOP_READ);
    rom_assert_eq(21, DIAG_BPSTATE(3) | DIAG_BPIDX(511), rom_get_brdiag());
    rom_set_brdiag(DIAG_BPIDX(504) | DIAG_BPOP_READ);
    rom_assert_eq(22, DIAG_BPSTATE(3) | DIAG_BPIDX(504), rom_get_brdiag());
    rom_set_brdiag(DIAG_BPIDX(503) | DIAG_BPOP_READ);
    rom_assert_eq(23, DIAG_BPIDX(503), rom_get_brdiag());

    /*
     * Clearing the register reads entry zero, cleared just above.
     */
    rom_set_brdiag(0);
    rom_assert_eq(24, 0, rom_get_brdiag());

    /*
     * Every selector behind coprocessor 0 register 25 must be readable
     * and
     * writable without taking an exception, which is the whole of what
     * a boot
     * path needs. Nothing is armed to catch a fault, so an exception
     * here is
     * reported by the harness as unexpected, and the count afterwards
     * is
     * checked as well so that a fault which somehow resumed cannot pass
     * unnoticed.
     */
    rom_exc_reset();

    PERF_WRITE(PERF_SPEC0, 0);
    PERF_WRITE(PERF_CNT0, 0);
    PERF_WRITE(PERF_SPEC1, 0);
    PERF_WRITE(PERF_CNT1, 0);
    (void)PERF_READ(PERF_SPEC0);
    (void)PERF_READ(PERF_CNT0);
    (void)PERF_READ(PERF_SPEC1);
    (void)PERF_READ(PERF_CNT1);

    rom_assert_eq(25, 0, rom_exc.count);

    /*
     * An event specifier register holds what is written to it. The
     * counters
     * themselves are deliberately not checked for a round trip: a real
     * R10000
     * increments them as events occur, so an assertion that a counter
     * reads
     * back the value just written would be wrong for hardware even
     * where it
     * happened to hold against a model that counts nothing.
     *
     * The bits above the event and mode fields are not checked either.
     * The
     * manual's control register format shows them as zero but does not
     * say
     * what
     * a write to them does, so there is nothing to hold a model to.
     */
    PERF_WRITE(PERF_SPEC0, PERF_SPEC_VALUE);
    rom_assert_eq(26, PERF_SPEC_VALUE, PERF_READ(PERF_SPEC0));

    PERF_WRITE(PERF_SPEC1, PERF_SPEC_VALUE);
    rom_assert_eq(27, PERF_SPEC_VALUE, PERF_READ(PERF_SPEC1));

    rom_assert_eq(28, 0, rom_exc.count);
}
