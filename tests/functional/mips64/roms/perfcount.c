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

/* Bound waits so a stopped clock fails with a ROM assertion. */
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
 * R10000 User's Manual v2.0, Tables 14-19 and 14-20 define CP0 selectors.
 * The MIPS III assembler lacks syntax for them.
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

/* Count measures elapsed time on the same clock as the performance counters. */
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

    /* Keep interrupts masked; observe pending IP[7] in Cause. */
    rom_set_status(rom_get_status() & ~ROM_ST_IE);

    /* Clear any timer level and keep the timer from interfering. */
    rom_set_count(0);
    rom_set_compare(0x7fffffffu);

    /* The manual specifies a zero reset value for both Event fields. */
    rom_assert_eq(1, 0, (rom_get_perfctl0() >> 5) & 0xf);
    rom_assert_eq(2, 0, (rom_get_perfctl1() >> 5) & 0xf);

    /* Figure 14-22 defines control bits 8:0. */
    rom_set_perfctl0(0xffffffffu);
    rom_assert_eq(3, 0x1ffu, rom_get_perfctl0());
    rom_set_perfctl1(0xffffffffu);
    rom_assert_eq(4, 0x1ffu, rom_get_perfctl1());

    /* Select unsupported events so counter readback is stable. */
    rom_set_perfctl0(PC_EVENT(10) | PC_U);
    rom_set_perfctl1(PC_EVENT(5) | PC_S);
    rom_set_perfcnt0(0x13572468u);
    rom_set_perfcnt1(0x2468ace0u);
    rom_assert_eq(5, PC_EVENT(10) | PC_U, rom_get_perfctl0());
    rom_assert_eq(6, PC_EVENT(5) | PC_S, rom_get_perfctl1());
    rom_assert_eq(7, 0x13572468u, rom_get_perfcnt0());
    rom_assert_eq(8, 0x2468ace0u, rom_get_perfcnt1());

    /* IP[7] is the level of counter bit 31 and its interrupt enable. */
    rom_set_perfctl0(PC_IE);
    rom_set_perfctl1(0);
    rom_set_perfcnt1(0);
    rom_assert_eq(9, 0, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_perfcnt0(0x80000000u);
    rom_assert_eq(10, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);

    /* Clearing bit 31 deasserts the overflow level. */
    rom_set_perfcnt0(0x7fffffffu);
    rom_assert_eq(11, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /* Clearing the enable also deasserts the level. */
    rom_set_perfcnt0(0x80000000u);
    rom_assert_eq(12, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_perfctl0(0);
    rom_assert_eq(13, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /* Either counter can assert IP[7]; both must be cleared. */
    rom_set_perfctl0(PC_IE);
    rom_set_perfctl1(PC_IE);
    rom_set_perfcnt0(0x80000000u);
    rom_set_perfcnt1(0x80000000u);
    rom_assert_eq(14, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_perfcnt0(0);
    rom_assert_eq(15, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_perfcnt1(0);
    rom_assert_eq(16, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /* Compare clears the timer source but leaves counter overflow pending. */
    rom_set_perfcnt0(0x80000000u);
    rom_set_compare(0x7fffffffu);
    rom_assert_eq(17, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_set_perfcnt0(0);
    rom_assert_eq(18, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /* Counter overflow must not clear a pending timer interrupt. */
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

    /* Cycles are the only event the model derives. */
    rom_set_perfctl0(PC_EVENT(0) | PC_K);
    rom_set_perfcnt0(0);
    first = 0;
    for (i = 0; i < POLL_LIMIT && (first = rom_get_perfcnt0()) == 0; i++) {
    }
    rom_assert(22, first > 0);
    for (i = 0; i < POLL_LIMIT && (v = rom_get_perfcnt0()) <= first; i++) {
    }
    rom_assert(23, v > first);

    /* User-mode counting is disabled while the ROM runs in kernel mode. */
    rom_set_perfctl0(PC_EVENT(0) | PC_U);
    rom_set_perfcnt0(12345);
    wait_ticks(20000);
    rom_assert_eq(24, 12345, rom_get_perfcnt0());

    /* EXL and kernel mode qualifiers select different counters here. */
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

    /* Unsupported events do not fabricate increments. */
    rom_set_perfctl1(PC_EVENT(8) | PC_U | PC_S | PC_K | PC_EXL);
    rom_set_perfcnt1(0xcafe1234u);
    wait_ticks(20000);
    rom_assert_eq(27, 0xcafe1234u, rom_get_perfcnt1());
    rom_set_perfctl1(0);
    rom_set_perfcnt1(0);

    /* The counter continues through overflow and wraps. */
    rom_set_perfctl0(PC_EVENT(0) | PC_K);
    rom_set_perfcnt0(0xffff0000u);
    for (i = 0; i < POLL_LIMIT && (v = rom_get_perfcnt0()) >= 0xffff0000u;
         i++) {
    }
    rom_assert(28, v < 0x01000000u);

    /* Poll Cause alone to prove counting raises IP[7]. */
    rom_set_perfctl0(PC_EVENT(0) | PC_K | PC_IE);
    rom_set_perfcnt0(0x80000000u - 0x40000u);
    for (i = 0; i < POLL_LIMIT && !(rom_get_cause() & ROM_CAUSE_IP7); i++) {
    }
    rom_assert_eq(29, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);

    /* Leave no interrupt pending or counter enabled. */
    rom_set_perfctl0(0);
    rom_set_perfcnt0(0);
    rom_set_perfctl1(0);
    rom_set_perfcnt1(0);
    rom_set_compare(0x7fffffffu);
    rom_assert_eq(30, 0, rom_get_cause() & ROM_CAUSE_IP7);

    rom_assert_eq(31, 0, rom_exc.count);

    /* A write at overflow asserts the level immediately. */
    rom_set_perfctl0(PC_EVENT(0) | PC_K | PC_IE);
    rom_set_perfcnt0(0x80000000u);
    rom_assert_eq(32, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);

    /* Poll Cause while the overflow level falls as Count advances. */
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

    /* Clear both counters and enables. */
    rom_set_perfctl0(0);
    rom_set_perfcnt0(0);
    rom_assert_eq(35, 0, rom_get_cause() & ROM_CAUSE_IP7);

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
