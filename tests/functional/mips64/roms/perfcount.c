/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test the R10000 performance counters and their shared Cause.IP[7]
 * interrupt. Counter event 0 counts cycles; other events must not be
 * fabricated by the model. Checks cover selector decoding, mode
 * qualifiers, overflow, and interaction with the timer interrupt.
 *
 * Source: R10000 User's Manual, version 2.0, sections 14.20 and 17.
 */

#include "rom.h"

const char rom_name[] = "perfcount";

/*
 * The same arming distance and poll bound the timer ROM uses: waits end
 * on
 * their condition, the bound only turns a wedge into a numbered
 * failure.
 */
#define TIMER_TARGET (1u << 22)
#define POLL_LIMIT   50000000

/*
 * Control register fields, from Figure 14-22.
 */
#define PC_EVENT(e) ((uint32_t)(e) << 5)
#define PC_IE       (1u << 4)
#define PC_U        (1u << 3)
#define PC_S        (1u << 2)
#define PC_K        (1u << 1)
#define PC_EXL      (1u << 0)

/*
 * The four registers behind coprocessor 0 register 25, named by the low
 * bits
 * of the opcode as Tables 14-19 and 14-20 lay them out. The suite is
 * assembled as MIPS III, where the assembler has no syntax for those
 * bits,
 * so each accessor states the later dialect that has one — the
 * instruction
 * word it emits, COP0 with the selector in bits 2:0, is exactly the
 * encoding
 * those tables give.
 */
#define ROM_PERF(name, sel)                                                   \
    static inline uint32_t rom_get_##name(void)                               \
    {                                                                         \
        uint32_t v;                                                           \
        __asm__ __volatile__(".set push\n\t.set mips32\n\t"                   \
                             "mfc0 %0, $25, " #sel "\n\t"                     \
                             ".set pop" : "=r"(v));                           \
        return v;                                                             \
    }                                                                         \
    static inline void rom_set_##name(uint32_t v)                             \
    {                                                                         \
        __asm__ __volatile__(".set push\n\t.set mips32\n\t"                   \
                             "mtc0 %0, $25, " #sel "\n\t"                     \
                             ".set pop" : : "r"(v));                          \
    }

ROM_PERF(perfctl0, 0)
ROM_PERF(perfcnt0, 1)
ROM_PERF(perfctl1, 2)
ROM_PERF(perfcnt1, 3)

/*
 * Let a known amount of virtual time pass, measured by the Count
 * register so
 * the wait is against the same clock cycles are derived from.
 */
static void wait_ticks(uint32_t ticks)
{
    uint32_t start = rom_get_count();
    int i;

    for (i = 0; i < POLL_LIMIT && rom_get_count() - start < ticks; i++) {
    }
}

void rom_main(void)
{
    uint32_t first, v;
    int i;

    rom_exc_reset();

    /*
     * Interrupts stay off in the Status register for the whole ROM, so
     * a
     * pending IP[7] is only ever observed as a bit in the Cause
     * register.
     */
    rom_set_status(rom_get_status() & ~ROM_ST_IE);

    /*
     * Park the timer far away so it cannot raise IP[7] underneath the
     * overflow tests; Compare's write also clears any pending timer
     * level.
     */
    rom_set_count(0);
    rom_set_compare(0x7fffffffu);

    /*
     * Out of reset, the Event field of both control registers is zero —
     * the
     * one reset promise the manual makes about these registers.
     */
    rom_assert_eq(1, 0, (rom_get_perfctl0() >> 5) & 0xf);
    rom_assert_eq(2, 0, (rom_get_perfctl1() >> 5) & 0xf);

    /*
     * The control registers keep bits 8:0 and nothing else: writing all
     * ones
     * reads back exactly the event field, the interrupt enable and the
     * four
     * count enable bits.
     */
    rom_set_perfctl0(0xffffffffu);
    rom_assert_eq(3, 0x1ffu, rom_get_perfctl0());
    rom_set_perfctl1(0xffffffffu);
    rom_assert_eq(4, 0x1ffu, rom_get_perfctl1());

    /*
     * The counters are full 32-bit read/write registers, and the four
     * selectors reach four distinct registers: distinct values written
     * through each selector all read back through their own.
     *
     * The controls are set to distinct patterns whose event fields name
     * events nothing in this loop performs, so the counters hold still
     * for
     * the exact comparison. (An all-ones control would do the same —
     * its
     * event is not Cycles — but naming the intent beats leaning on it.)
     */
    rom_set_perfctl0(PC_EVENT(10) | PC_U);
    rom_set_perfctl1(PC_EVENT(5) | PC_S);
    rom_set_perfcnt0(0x13572468u);
    rom_set_perfcnt1(0x2468ace0u);
    rom_assert_eq(5, PC_EVENT(10) | PC_U, rom_get_perfctl0());
    rom_assert_eq(6, PC_EVENT(5) | PC_S, rom_get_perfctl1());
    rom_assert_eq(7, 0x13572468u, rom_get_perfcnt0());
    rom_assert_eq(8, 0x2468ace0u, rom_get_perfcnt1());

    /*
     * The overflow interrupt is a level derived from the pair (counter
     * bit
     * 31, control interrupt enable): preloading a counter with bit 31
     * set
     * while the enable is on asserts IP[7] with no interrupt delivered
     * —
     * interrupts are masked — and clearing either half takes it away.
     */
    rom_set_perfctl0(PC_IE);
    rom_set_perfctl1(0);
    rom_set_perfcnt1(0);
    rom_assert_eq(9, 0, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_perfcnt0(0x80000000u);
    rom_assert_eq(10, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * Writing a value with bit 31 clear is the manual's way to clear
     * it.
     */
    rom_set_perfcnt0(0x7fffffffu);
    rom_assert_eq(11, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * Clearing the enable instead: the pending condition needs both.
     */
    rom_set_perfcnt0(0x80000000u);
    rom_assert_eq(12, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_perfctl0(0);
    rom_assert_eq(13, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * Either counter is enough, and each needs its own clearing.
     */
    rom_set_perfctl0(PC_IE);
    rom_set_perfctl1(PC_IE);
    rom_set_perfcnt0(0x80000000u);
    rom_set_perfcnt1(0x80000000u);
    rom_assert_eq(14, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_perfcnt0(0);
    rom_assert_eq(15, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_perfcnt1(0);
    rom_assert_eq(16, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * The manual's own sentence about sharing IP[7]: a Compare write
     * clears
     * the timer interrupt but not a simultaneous counter overflow.
     */
    rom_set_perfcnt0(0x80000000u);
    rom_set_compare(0x7fffffffu);
    rom_assert_eq(17, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_perfcnt0(0);
    rom_assert_eq(18, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * And the other direction: a pending timer interrupt survives the
     * counter overflow being cleared, and only its own Compare write
     * takes
     * it down. The timer is armed the way the interrupt-delivery ROM
     * hardened its arming: writing Count and Compare is two
     * instructions,
     * and a host that stalls the emulated processor between them for
     * longer than the arming distance lets Count cross Compare before
     * Compare is written — the match is then a whole counter wrap away,
     * the old poll on Count returned at once (Count already past the
     * target), and the assertion read a line that was never owed. A
     * lost
     * window is detectable right after the writes and is re-armed; the
     * wait polls the line itself, the same shape as the delivery ROM. A
     * line that fails to rise from a cleanly armed crossing still falls
     * through to the assertion.
     */
    for (v = 0; v < 8; v++) {
        rom_set_count(0);
        rom_set_compare(TIMER_TARGET);
        if (rom_get_count() >= TIMER_TARGET) {
            continue;
        }
        for (i = 0; i < POLL_LIMIT && !(rom_get_cause() & ROM_CAUSE_IP7);
             i++) {
        }
        break;
    }
    rom_assert_eq(19, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_perfcnt0(0x80000000u);
    rom_set_perfcnt0(0);
    rom_assert_eq(20, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_compare(0x7fffffffu);
    rom_set_count(0);
    rom_assert_eq(21, 0, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_perfctl0(0);
    rom_set_perfctl1(0);

    /*
     * Cycles, the one event the emulator can honestly derive. Selected
     * with
     * the K enable while the ROM runs in kernel mode with EXL and ERL
     * clear,
     * the counter moves on its own and only forward.
     */
    rom_set_perfctl0(PC_EVENT(0) | PC_K);
    rom_set_perfcnt0(0);
    first = 0;
    for (i = 0; i < POLL_LIMIT && (first = rom_get_perfcnt0()) == 0; i++) {
    }
    rom_assert(22, first > 0);
    for (i = 0; i < POLL_LIMIT && (v = rom_get_perfcnt0()) <= first; i++) {
    }
    rom_assert(23, v > first);

    /*
     * The count enable bits gate by mode: the same event with only the
     * U
     * enable counts nothing while the processor is in kernel mode.
     */
    rom_set_perfctl0(PC_EVENT(0) | PC_U);
    rom_set_perfcnt0(12345);
    wait_ticks(20000);
    rom_assert_eq(24, 12345, rom_get_perfcnt0());

    /*
     * The EXL enable is the exception-level qualifier: with EXL raised
     * the
     * K-qualified counter stands still and the EXL-qualified one moves.
     * The K-qualified counter is live until the Status write lands, so
     * its
     * held value is whatever it reached by then — the assertion is that
     * it
     * does not move any further while EXL stays up, not that it never
     * moved.
     */
    rom_set_perfctl0(PC_EVENT(0) | PC_EXL);
    rom_set_perfcnt0(0);
    rom_set_perfctl1(PC_EVENT(0) | PC_K);
    rom_set_perfcnt1(54321);
    rom_set_status(rom_get_status() | ROM_ST_EXL);
    first = rom_get_perfcnt1();
    wait_ticks(20000);
    v = rom_get_perfcnt1();
    rom_assert_eq(25, first, v);
    first = rom_get_perfcnt0();
    rom_set_status(rom_get_status() & ~ROM_ST_EXL);
    rom_assert(26, first > 0);
    rom_set_perfctl1(0);
    rom_set_perfcnt1(0);

    /*
     * A counter whose event the emulator cannot derive holds exactly
     * what
     * was written, whatever the mode enables say: no invented
     * increments.
     */
    rom_set_perfctl1(PC_EVENT(8) | PC_U | PC_S | PC_K | PC_EXL);
    rom_set_perfcnt1(0xcafe1234u);
    wait_ticks(20000);
    rom_assert_eq(27, 0xcafe1234u, rom_get_perfcnt1());
    rom_set_perfctl1(0);
    rom_set_perfcnt1(0);

    /*
     * Counting continues after overflow: preloaded just below the wrap
     * with
     * bit 31 already set and the interrupt enable off, the counter
     * keeps
     * moving, wraps through zero and lands small.
     */
    rom_set_perfctl0(PC_EVENT(0) | PC_K);
    rom_set_perfcnt0(0xffff0000u);
    for (i = 0; i < POLL_LIMIT && (v = rom_get_perfcnt0()) >= 0xffff0000u;
         i++) {
    }
    rom_assert(28, v < 0x01000000u);

    /*
     * An overflow reached by counting, not by a write: armed a little
     * below
     * bit 31 with the interrupt enable on, the pending bit must appear
     * in
     * the Cause register on its own, with no access to the counter in
     * the
     * polling loop.
     */
    rom_set_perfctl0(PC_EVENT(0) | PC_K | PC_IE);
    rom_set_perfcnt0(0x80000000u - 0x40000u);
    for (i = 0; i < POLL_LIMIT && !(rom_get_cause() & ROM_CAUSE_IP7); i++) {
    }
    rom_assert_eq(29, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * Leave nothing pending and nothing counting behind.
     */
    rom_set_perfctl0(0);
    rom_set_perfcnt0(0);
    rom_set_perfctl1(0);
    rom_set_perfcnt1(0);
    rom_set_compare(0x7fffffffu);
    rom_assert_eq(30, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * Nothing above raised an exception.
     */
    rom_assert_eq(31, 0, rom_exc.count);

    /*
     * A write that lands a counting counter at or past overflow is a
     * standing level, not an edge to wait for: with the interrupt
     * enable
     * on, bit 31 set in the written value must show in the Cause
     * register
     * at once, not a counter wrap later. The write anchors the counter
     * at
     * exactly the written value, so unlike arming below the edge this
     * is
     * not a race against the clock.
     */
    rom_set_perfctl0(PC_EVENT(0) | PC_K | PC_IE);
    rom_set_perfcnt0(0x80000000u);
    rom_assert_eq(32, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * The far edge is a deadline of its own: rewritten near the wrap,
     * the
     * level stands until the counter runs off bit 31, then falls on its
     * own, with no access to the counter in the polling loop.
     *
     * The write is deliberately close to the wrap so the fall arrives
     * while the poll below is still running, which makes the standing
     * level a window rather than a state: the counter advances on the
     * emulated processor's clock, so a host that stalls this processor
     * between the write and the read for longer than the sixty-odd
     * thousand counts left can run it off bit 31 before the level is
     * ever observed, and the read then sees a line that has already
     * fallen for the right reason. A lost window is detectable and is
     * rewritten, the same shape as the arming above; the level observed
     * inside the loop is the one asserted, so the assertion cannot race
     * a second read.
     */
    for (v = 0, i = 0; i < 8; i++) {
        rom_set_perfcnt0(0xffff0000u);
        v = rom_get_cause() & ROM_CAUSE_IP7;
        if (v) {
            break;
        }
    }
    rom_assert_eq(33, ROM_CAUSE_IP7, v);
    for (i = 0; i < POLL_LIMIT && (rom_get_cause() & ROM_CAUSE_IP7); i++) {
    }
    rom_assert_eq(34, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * Leave nothing pending and nothing counting, again.
     */
    rom_set_perfctl0(0);
    rom_set_perfcnt0(0);
    rom_assert_eq(35, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * Nothing in the write-at-overflow block raised an exception.
     */
    rom_assert_eq(36, 0, rom_exc.count);

    /*
     * The later four-pair models use selectors 4 through 7, but those
     * encodings remain undefined on the R10000.  Probe both ordinary
     * move
     * directions so widening the shared decoder cannot silently expose
     * the
     * additional registers here.
     */
#define EXPECT_RI(id, instruction)                                            \
    do {                                                                      \
        rom_exc_reset();                                                      \
        ROM_TRY(instruction);                                                 \
        rom_assert_eq((id), 1, rom_exc.count);                                \
        rom_assert_eq((id) + 1, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));    \
    } while (0)

    EXPECT_RI(37, ".word 0x4008c804"); /* mfc0 $8, $25, select 4 */
    EXPECT_RI(39, ".word 0x4080c804"); /* mtc0 $0, $25, select 4 */
    EXPECT_RI(41, ".word 0x4008c805"); /* mfc0 $8, $25, select 5 */
    EXPECT_RI(43, ".word 0x4080c805"); /* mtc0 $0, $25, select 5 */
    EXPECT_RI(45, ".word 0x4008c806"); /* mfc0 $8, $25, select 6 */
    EXPECT_RI(47, ".word 0x4080c806"); /* mtc0 $0, $25, select 6 */
    EXPECT_RI(49, ".word 0x4008c807"); /* mfc0 $8, $25, select 7 */
    EXPECT_RI(51, ".word 0x4080c807"); /* mtc0 $0, $25, select 7 */
}
