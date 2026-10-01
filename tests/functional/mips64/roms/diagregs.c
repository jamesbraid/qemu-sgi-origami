/* SPDX-License-Identifier: BSD-3-Clause */
/* Check R10000 FrameMask, Diagnostic, and performance-counter registers. */

#include "rom.h"

const char rom_name[] = "diagregs";

/*
 * Selectors for CP0 register 25 choose performance counters and event
 * specifiers. Encode the moves as words because this assembler lacks the
 * three-operand MIPS IV syntax; register 8 carries the value.
 */
#define PERF_SPEC0 0
#define PERF_CNT0  1
#define PERF_SPEC1 2
#define PERF_CNT1  3

/* Expand selector macros before stringifying the assembly operand. */
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

/* Event 1 counted in kernel mode, with interrupts disabled. */
#define PERF_EVENT_SHIFT 5
#define PERF_MODE_KERNEL (1u << 1)
#define PERF_SPEC_VALUE  ((1u << PERF_EVENT_SHIFT) | PERF_MODE_KERNEL)

/*
 * Diagnostic register fields used below. Only BPMODE is ordinary writable
 * state; the prediction-table fields trigger table operations.
 */
#define DIAG_BPMODE_SHIFT 16
#define DIAG_BPMODE_MASK  (3ull << DIAG_BPMODE_SHIFT)
#define DIAG_BPSTATE_MASK (3ull << 14)
#define DIAG_BPIDX_MASK   (0x1ffull << 3)
#define DIAG_BPOP_MASK    (3ull << 0)

/* Reserved bits must read zero after writes. */
#define DIAG_RESERVED (0xfff0ffff00000000ull | (0x1full << 23) | \
                       (3ull << 12) | (1ull << 2))

void rom_main(void)
{
    /*
     * Check FrameMask register reset/readback here; framemask.c separately
     * checks how its mask affects TLB writes.
     */
    rom_assert_eq(1, 0, rom_get_framemask());

    rom_set_framemask(0xffffffffu);
    rom_assert_eq(2, 0x0000ffffu, rom_get_framemask());

    rom_set_framemask(0);
    rom_assert_eq(3, 0, rom_get_framemask());

    /*
     * Check BPMode read/write behavior and reserved-bit readback. A hardcoded
     * zero would pass the latter but fail the mode checks; treating the whole
     * register as scratch would fail reserved-bit checks. Accesses are 64-bit.
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
     * The Diagnostic register's table selectors read, write, or initialize
     * 512 two-bit entries. The test checks these explicit operations only;
     * conditional-branch prediction does not update entries in this model.
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
