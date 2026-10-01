/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Synthetic CPU loads exercise HEART's bounded direct-memory ECC contract. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "sn0-user-image.h"

#define HEART_REGS 0x0ff00000ULL
#define WORD 0x20008000ULL
#define MAILBOX 0x20004000ULL
#define UNCACHED(address) (0x9000000000000000ULL | (address))
#define PATTERN 0x87541230abcdef69ULL
#define SENTINEL 0x729034ab85c6def0ULL
#define REPAIRED 0x8a03c1597624bdfeULL
#define SAVED_EPC 0xffffffffbfc01700ULL
#define SAVED_ERROREPC 0xffffffffbfc01800ULL
#define ECC_ENABLE (1ULL << 15)
#define ECC_REPORT 2
#define ECC_DOUBLE (1ULL << 20)
#define ECC_FORCE (1ULL << 21)
#define ECC_IRQ (1ULL << 63)

#define MB_DONE 0
#define MB_CAUSE 8
#define MB_STATUS 16
#define MB_EPC 24
#define MB_DESTINATION 32
#define MB_ERROREPC 40
#define MB_CACHEERR 48
#define MB_REPAIRED 56
#define MB_RETIRED 64
#define MB_CACHEERR_BEFORE 72
#define ENTRY 0x800
#define HANDLER 0x400

/* Profiles cover disabled reporting, masks, delay slots and nested EXL. */
typedef struct HeartCPUCase {
    bool r12000;
    bool delay_slot;
    bool exl;
    bool report;
    bool heart_interrupt;
    bool cpu_interrupt;
    bool forced_repair;
    bool heart_mask;
} HeartCPUCase;

static void emit_dmfc0(SN0Image *p, unsigned rt, unsigned rd)
{
    sn0_emit(p, 0x40200000 | (rt << 16) | (rd << 11));
}

static void save_cp0(SN0Image *p, unsigned rd, unsigned offset)
{
    emit_dmfc0(p, SN0_R_T0, rd);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_K0, SN0_R_T0, offset);
}

static unsigned build_image(SN0Image *p, const HeartCPUCase *c)
{
    unsigned at, fault;
    uint32_t status = SN0_STATUS_KERNEL | (c->exl ? 2 : 0) |
                      (c->cpu_interrupt ? 0xff01 : 0);
    uint64_t mode = ECC_ENABLE | (c->report ? ECC_REPORT : 0) |
                    (c->heart_interrupt ? 1ULL << 13 : 0);

    at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
    sn0_patch_branch(p, at, ENTRY);
    p->pos = SN0_VEC_GENERAL;
    at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
    sn0_patch_branch(p, at, HANDLER);

    p->pos = HANDLER;
    sn0_emit_li64(p, SN0_R_K0, UNCACHED(MAILBOX));
    save_cp0(p, SN0_CP0_CAUSE, MB_CAUSE);
    save_cp0(p, SN0_CP0_STATUS, MB_STATUS);
    save_cp0(p, SN0_CP0_EPC, MB_EPC);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_K0, SN0_R_V0, MB_DESTINATION);
    save_cp0(p, SN0_CP0_ERROREPC, MB_ERROREPC);
    save_cp0(p, 27, MB_CACHEERR);
    /* Repair never acknowledges the separate HEART error latches. */
    sn0_emit_li64(p, SN0_R_T0, c->forced_repair ? ECC_FORCE : 0);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T0, 0);
    sn0_emit_li64(p, SN0_R_T0, REPAIRED);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S1, SN0_R_T0, 0);
    sn0_emit_li64(p, SN0_R_T0, ECC_ENABLE | ECC_REPORT);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T0, 0);
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S1, SN0_R_T1, 0);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_K0, SN0_R_T1, MB_REPAIRED);
    sn0_emit_li(p, SN0_R_T0, 1);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_T0, MB_DONE);
    sn0_emit_park(p);

    p->pos = ENTRY;
    sn0_emit_li(p, SN0_R_T0, status);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_li64(p, SN0_R_T0, SAVED_EPC);
    sn0_emit_dmtc0(p, SN0_R_T0, SN0_CP0_EPC);
    sn0_emit_li64(p, SN0_R_T0, SAVED_ERROREPC);
    sn0_emit_dmtc0(p, SN0_R_T0, SN0_CP0_ERROREPC);
    sn0_emit_li64(p, SN0_R_K0, UNCACHED(MAILBOX));
    save_cp0(p, 27, MB_CACHEERR_BEFORE);
    sn0_emit_li(p, SN0_R_S0, SN0_KSEG1(HEART_REGS));
    sn0_emit_li64(p, SN0_R_S1, UNCACHED(WORD));
    sn0_emit_li64(p, SN0_R_T0, ECC_ENABLE | ECC_REPORT | ECC_DOUBLE);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T0, 0);
    sn0_emit_li64(p, SN0_R_T0, PATTERN);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S1, SN0_R_T0, 0);
    sn0_emit_li64(p, SN0_R_T0, mode);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T0, 0);
    sn0_emit_li64(p, SN0_R_V0, SENTINEL);
    fault = p->pos;
    if (c->delay_slot) {
        sn0_emit_i(p, SN0_OPC_BEQ, 0, 0, 1);
    }
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S1, SN0_R_V0, 0);
    sn0_emit_li(p, SN0_R_T0, 1);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_T0, MB_RETIRED);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_T0, MB_DONE);
    sn0_emit_park(p);
    return fault;
}

static void test_double_bit_cpu(gconstpointer data)
{
    const HeartCPUCase *c = data;
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *s;
    unsigned fault;
    uint64_t cause, status;
    uint32_t done;
    g_autofree char *registers = NULL;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    fault = build_image(image, c);
    path = sn0_image_write(image, "heart-double-bit-cpu");
    s = qtest_initf("-M %s -cpu %s -accel tcg -S -bios %s",
        c->r12000 ? "octane2" : "octane",
        c->r12000 ? "R12000,r12000-prid=0xe35,r12000-fpu-id=0x900,"
        "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152,"
        "r12000-scache-block-words=32" : "R10000", path);
    unlink(path);
    qtest_writel(s, HEART_REGS + 0x20, c->r12000 ? 0x80070000 : 0x80030000);
    /* Vary the HEART mask independently of mode bit 13 and CPU masks. */
    qtest_writeq(s, HEART_REGS + 0x10000,
                 c->heart_mask ? ECC_IRQ : 0);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    done = sn0_wait_word(s, 0, MAILBOX, 10);
    qtest_qmp_assert_success(s, "{'execute':'stop'}");
    registers = qtest_hmp(s, "info registers");
    g_test_message("%s", registers);
    g_assert_cmpuint(done, ==, 1);
    g_assert_cmphex(qtest_readl(s, MAILBOX + MB_RETIRED), ==, 0);
    cause = qtest_readq(s, MAILBOX + MB_CAUSE);
    status = qtest_readq(s, MAILBOX + MB_STATUS);
    g_assert_cmphex((cause >> 2) & 31, ==, 7);
    g_assert_cmphex(status & 2, ==, 2);
    g_assert_cmphex(qtest_readq(s, MAILBOX + MB_EPC), ==,
        c->exl ? SAVED_EPC : 0xffffffffbfc00000ULL + fault);
    if (!c->exl) {
        g_assert_cmphex(cause & (1ULL << 31), ==,
                        c->delay_slot ? 1ULL << 31 : 0);
    }
    g_assert_cmphex(qtest_readq(s, MAILBOX + MB_DESTINATION), ==, SENTINEL);
    g_assert_cmphex(qtest_readq(s, MAILBOX + MB_ERROREPC), ==, SAVED_ERROREPC);
    g_assert_cmphex(qtest_readq(s, MAILBOX + MB_CACHEERR), ==,
                    qtest_readq(s, MAILBOX + MB_CACHEERR_BEFORE));
    g_assert_cmphex(qtest_readq(s, MAILBOX + MB_REPAIRED), ==, REPAIRED);
    g_assert_cmphex(qtest_readq(s, WORD), ==, REPAIRED);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==,
                    c->report ? 2 : 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==,
                    c->report ? ECC_IRQ : 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x98), ==,
                    c->report ? WORD | (0x30ULL << 34) : 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0xa0), ==,
                    c->report ? PATTERN ^ 3 : 0);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    static const HeartCPUCase cases[] = {
        { false, false, false, true, false, false, false, true },
        { false, true, false, true, true, true, true, false },
        { false, false, true, false, false, true, true, true },
        { false, false, false, true, true, true, false, true },
        { true, false, false, true, true, false, true, false },
        { true, true, false, false, false, true, false, true },
        { true, true, true, true, false, false, false, true },
        { true, true, false, true, true, true, true, true },
    };

    g_test_init(&argc, &argv, NULL);
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        unsigned cpu = 10000;
        g_autofree char *name = NULL;

        if (cases[i].r12000) {
            cpu = 12000;
        }
        name = g_strdup_printf("/heart/cpu/r%u/case%u", cpu, i);

        qtest_add_data_func(name, &cases[i], test_double_bit_cpu);
    }
    return g_test_run();
}
