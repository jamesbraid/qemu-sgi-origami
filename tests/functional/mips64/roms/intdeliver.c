/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A pending timer interrupt is delivered when Status unmasks it or ERET
 * clears EXL.
 */

#include "rom.h"

const char rom_name[] = "intdeliver";

/* Status.IM7 enables the timer interrupt. */
#define ROM_ST_IM7 (1u << 15)

/* Bound waits so a stopped Count fails instead of hanging. */
#define TIMER_TARGET (1u << 22)
#define POLL_LIMIT   50000000

#define VEC_BOOTSTRAP_GENERAL (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)

/* The bounded wait resumes after the handler. Its counter avoids $26/$27. */
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

/* ERET clears EXL before the pending interrupt can arrive in the wait loop. */
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

/* Retry only if Count crosses Compare between the separate arming writes. */
static void raise_timer_line(void)
{
    int i, tries;

    for (tries = 0; tries < 8; tries++) {
        rom_set_count(0);
        rom_set_compare(TIMER_TARGET);
        if (rom_get_count() >= TIMER_TARGET) {
            /* Count crossed the target while arming; retry. */
            continue;
        }
        for (i = 0; i < POLL_LIMIT && !(rom_get_cause() & ROM_CAUSE_IP7);
             i++) {
        }
        return;
    }
}

/* Move Compare ahead of Count and retry if IP7 remains asserted. */
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

    /* Start in kernel mode with EXL, ERL, IE, and IM7 clear. */
    base = rom_get_status() & ~(ROM_ST_IE | ROM_ST_IM7);
    rom_set_status(base);
    rom_assert_eq(1, 0, rom_get_status() &
                  (ROM_ST_IE | ROM_ST_EXL | ROM_ST_ERL));
    park_timer();
    rom_assert_eq(2, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /* IP7 becomes pending while delivery is disabled. */
    raise_timer_line();
    rom_assert_eq(3, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_assert_eq(4, 0, rom_exc.count);

    /*
     * Opening IE delivers the pending interrupt once through the general
     * vector.
     */
    rom_exc.status_set = base;
    UNMASK_AND_WAIT(base | ROM_ST_IE | ROM_ST_IM7);
    rom_assert_eq(5, 1, rom_exc.count);
    rom_assert_eq(6, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(7, ROM_EXC_INT, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(8, ROM_CAUSE_IP7, rom_exc.cause & ROM_CAUSE_IP7);

    /* A Compare write clears IP7. */
    park_timer();
    rom_assert_eq(9, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /* With IE set and IM7 clear, IP7 stays pending until IM7 is opened. */
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
     * EXL masks the interrupt despite IE and IM7. ERET clears EXL and
     * delivers it.
     */
    rom_set_status(base | ROM_ST_IE | ROM_ST_IM7 | ROM_ST_EXL);
    raise_timer_line();
    rom_assert_eq(14, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_assert_eq(15, 2, rom_exc.count);

    rom_exc.status_set = base;
    ERET_AND_WAIT();
    rom_assert_eq(16, 3, rom_exc.count);
    rom_assert_eq(17, ROM_EXC_INT, ROM_CAUSE_EXC(rom_exc.cause));

    rom_set_status(base);
    park_timer();
    rom_assert_eq(18, 0, rom_get_cause() & ROM_CAUSE_IP7);
}
