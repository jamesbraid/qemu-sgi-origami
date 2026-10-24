/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * Octane CP0 Count/Compare timer: a Compare write that a short run of
 * register instructions separates from the Count read or write it was
 * computed from.
 *
 * The IRIX 6.5.30 IP30 kernel arms its scheduler clock with interrupts
 * masked, either by writing Count zero and then, eight register
 * instructions later, Compare with a value of at least 1000, or by
 * reading Count and writing Compare at least 1000 ahead of it. On the
 * processor those instructions take a few cycles. Without instruction
 * counting, QEMU's virtual clock follows the host and can pass the target
 * before the Compare write executes; a strict equality comparator then
 * waits a full 32-bit wrap (23.9 s at the Octane2's 180 MHz Count).
 *
 * The Octane machines turn on the Count-sample window, which recovers
 * such a crossed target at the Compare write. Instruction counting with a
 * large shift stands in for the host delay here: each instruction
 * advances the virtual clock by about a microsecond, so 32 instructions
 * put Count well past the target on both Count rates, deterministically.
 *
 * The controls are the sequences the window must leave alone: an
 * exception between the sample and the write, a target behind the latest
 * Count read, a target equal to it, the read-Count-minus-one disarm
 * public drivers use, the Count=1/Compare=0 disarm the IP30 kernel
 * issues, and a target that is still ahead.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "sn0-user-image.h"

#define HEART_REGS 0x0ff00000ULL
#define MAILBOX 0x20004000ULL
#define UNCACHED(address) (0x9000000000000000ULL | (address))
#define ENTRY 0x800

#define CP0_COUNT 9
#define CP0_COMPARE 11
#define CAUSE_IP7 0x8000u
#define TARGET 1000u
#define SPACING 32

#define MB_DONE 0
#define MB_CAUSE 4
#define MB_COUNT 8
#define MB_COMPARE 12

#define OCTANE2_CPU "R12000,r12000-prid=0xe35,r12000-fpu-id=0x900," \
    "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152," \
    "r12000-scache-block-words=32"

typedef enum TimerSequence {
    SEQ_CROSSED,
    SEQ_CROSSED_READ,
    SEQ_BEHIND_READ,
    SEQ_EQUAL_READ,
    SEQ_EXCEPTION,
    SEQ_READ_MINUS_ONE,
    SEQ_DISARM,
    SEQ_AHEAD,
} TimerSequence;

typedef struct TimerCase {
    const char *name;
    TimerSequence seq;
    bool ip7;
} TimerCase;

static const TimerCase cases[] = {
    { "crossed-after-write", SEQ_CROSSED, true },
    { "crossed-after-read", SEQ_CROSSED_READ, true },
    { "behind-latest-read", SEQ_BEHIND_READ, false },
    { "target-equals-read", SEQ_EQUAL_READ, false },
    { "exception-between", SEQ_EXCEPTION, false },
    { "read-minus-one", SEQ_READ_MINUS_ONE, false },
    { "count-one-compare-zero", SEQ_DISARM, false },
    { "target-ahead", SEQ_AHEAD, false },
};

static void emit_spacing(SN0Image *p)
{
    for (unsigned int i = 0; i < SPACING; i++) {
        sn0_emit_addu(p, SN0_R_T2, SN0_R_T2, SN0_R_T1);
    }
}

static void build_image(SN0Image *p, TimerSequence seq)
{
    unsigned int at;

    at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
    sn0_patch_branch(p, at, ENTRY);

    /* The general vector steps over the SYSCALL and returns. */
    p->pos = SN0_VEC_GENERAL;
    sn0_emit(p, 0x40200000 | (SN0_R_K1 << 16) | (14 << 11)); /* dmfc0 EPC */
    sn0_emit(p, mips_daddiu(SN0_R_K1, SN0_R_K1, 4));
    sn0_emit_dmtc0(p, SN0_R_K1, 14);
    sn0_emit_eret(p);

    p->pos = ENTRY;
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_li64(p, SN0_R_K0, UNCACHED(MAILBOX));
    sn0_emit_li(p, SN0_R_T1, 1);
    sn0_emit_li(p, SN0_R_A0, seq == SEQ_AHEAD ? 0x10000000u : TARGET);
    sn0_emit_li(p, SN0_R_A1, 1);

    /* Raw CP0 writes: sn0_emit_mtc0() pads each with nops. */
    switch (seq) {
    case SEQ_CROSSED:
    case SEQ_AHEAD:
        sn0_emit(p, mips_mtc0(SN0_R_ZERO, CP0_COUNT));
        emit_spacing(p);
        sn0_emit(p, mips_mtc0(SN0_R_A0, CP0_COMPARE));
        break;
    case SEQ_CROSSED_READ:
        sn0_emit(p, mips_mfc0(SN0_R_T8, CP0_COUNT));
        sn0_emit(p, mips_addiu(SN0_R_T8, SN0_R_T8, TARGET));
        emit_spacing(p);
        sn0_emit(p, mips_mtc0(SN0_R_T8, CP0_COMPARE));
        break;
    case SEQ_BEHIND_READ:
        /* The read moves the window's base past the 1000 target. */
        sn0_emit(p, mips_mtc0(SN0_R_ZERO, CP0_COUNT));
        emit_spacing(p);
        sn0_emit(p, mips_mfc0(SN0_R_T8, CP0_COUNT));
        sn0_emit(p, mips_mtc0(SN0_R_A0, CP0_COMPARE));
        break;
    case SEQ_EQUAL_READ:
        /* A target equal to the sample is a full wrap ahead, not crossed. */
        sn0_emit(p, mips_mfc0(SN0_R_T8, CP0_COUNT));
        emit_spacing(p);
        sn0_emit(p, mips_mtc0(SN0_R_T8, CP0_COMPARE));
        break;
    case SEQ_EXCEPTION:
        sn0_emit(p, mips_mtc0(SN0_R_ZERO, CP0_COUNT));
        sn0_emit_syscall(p);
        emit_spacing(p);
        sn0_emit(p, mips_mtc0(SN0_R_A0, CP0_COMPARE));
        break;
    case SEQ_READ_MINUS_ONE:
        sn0_emit(p, mips_mtc0(SN0_R_ZERO, CP0_COUNT));
        emit_spacing(p);
        sn0_emit(p, mips_mfc0(SN0_R_T8, CP0_COUNT));
        sn0_emit(p, mips_addiu(SN0_R_T8, SN0_R_T8, -1));
        sn0_emit(p, mips_mtc0(SN0_R_T8, CP0_COMPARE));
        break;
    case SEQ_DISARM:
        sn0_emit(p, mips_mtc0(SN0_R_A1, CP0_COUNT));
        sn0_emit(p, mips_mtc0(SN0_R_ZERO, CP0_COMPARE));
        emit_spacing(p);
        break;
    }
    sn0_emit_mfc0(p, SN0_R_T0, SN0_CP0_CAUSE);
    sn0_emit_mfc0(p, SN0_R_T8, CP0_COUNT);
    sn0_emit_mfc0(p, SN0_R_T9, CP0_COMPARE);
    sn0_emit(p, mips_sw(SN0_R_T0, SN0_R_K0, MB_CAUSE));
    sn0_emit(p, mips_sw(SN0_R_T8, SN0_R_K0, MB_COUNT));
    sn0_emit(p, mips_sw(SN0_R_T9, SN0_R_K0, MB_COMPARE));
    sn0_emit(p, mips_sw(SN0_R_T1, SN0_R_K0, MB_DONE));
    sn0_emit_park(p);
}

static void run_case(const TimerCase *c, bool octane2)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *s;
    uint32_t cause, count, compare;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    build_image(image, c->seq);
    path = sn0_image_write(image, "octane-timer");
    s = qtest_initf("-M %s -cpu %s -bios %s -accel tcg,thread=single "
                    "-icount shift=10,sleep=off -S",
                    octane2 ? "octane2" : "octane",
                    octane2 ? OCTANE2_CPU : "R10000", path);
    unlink(path);
    /* Memory bank 0 present, as the HEART CPU test configures it. */
    qtest_writel(s, HEART_REGS + 0x20, octane2 ? 0x80070000 : 0x80030000);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    g_assert_cmpuint(sn0_wait_word(s, 0, MAILBOX + MB_DONE), ==, 1);
    cause = qtest_readl(s, MAILBOX + MB_CAUSE);
    count = qtest_readl(s, MAILBOX + MB_COUNT);
    compare = qtest_readl(s, MAILBOX + MB_COMPARE);
    qtest_quit(s);

    g_test_message("%s: Cause 0x%08x, Count %u, Compare %u after the "
                   "sequence", c->name, cause, count, compare);
    /*
     * Count is past every target but the far one, so equality cannot have
     * raised IP7 on the way: only the window can.
     */
    if (c->seq == SEQ_AHEAD) {
        g_assert_cmpuint(count - compare, >=, 0x80000000u);
    } else {
        g_assert_cmpuint(count - compare, >, 0);
        g_assert_cmpuint(count - compare, <, 0x80000000u);
    }
    g_assert_cmphex(cause & CAUSE_IP7, ==, c->ip7 ? CAUSE_IP7 : 0);
}

static void test_octane(gconstpointer data)
{
    run_case(data, false);
}

static void test_octane2(gconstpointer data)
{
    run_case(data, true);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *octane = g_strdup_printf("/octane/timer/%s",
                                                  cases[i].name);
        g_autofree char *octane2 = g_strdup_printf("/octane2/timer/%s",
                                                   cases[i].name);

        qtest_add_data_func(octane, &cases[i], test_octane);
        qtest_add_data_func(octane2, &cases[i], test_octane2);
    }
    return g_test_run();
}
