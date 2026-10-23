/* SPDX-License-Identifier: BSD-3-Clause */
/* R10000 secondary index selection after an in-process CPU reset. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "sn0-user-image.h"

#define ENTRY             0x800
#define FAULT_ENTRY       0x400
#define STATE_PHYS        0x3000
#define STATE_FULL        (0x40000000u + STATE_PHYS)
#define STATE_GUEST       SN0_KSEG1(STATE_PHYS)
#define GENERATION        0x00
#define COMPLETED         0x04
#define FAULT             0x0c
#define RESULT_BASE       0x20
#define RESULT_STRIDE     0x20
#define CONFIG            0x08
#define TAG_A             0x10
#define TAG_B             0x14
#define LINE_A            UINT64_C(0xffffffff80001000)
#define TAG_VALUE_A       0x5a5a4c00u
#define TAG_VALUE_B       0x2a5a0c00u

typedef struct ScacheResetCase {
    const char *cpu;
    uint32_t config;
    uint32_t expected_a;
} ScacheResetCase;

static void emit_tag_store(SN0Image *p, unsigned int address_reg,
                           uint32_t value)
{
    sn0_emit_li(p, SN0_R_T0, value);
    sn0_emit_mtc0(p, SN0_R_T0, 28);
    sn0_emit(p, mips_cache(0x0b, address_reg, 0));
}

static void emit_tag_read(SN0Image *p, unsigned int address_reg,
                          unsigned int result_offset)
{
    sn0_emit_mtc0(p, SN0_R_ZERO, 28);
    sn0_emit(p, mips_cache(0x07, address_reg, 0));
    sn0_emit_mfc0(p, SN0_R_T0, 28);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S1, SN0_R_T0, result_offset);
}

static void build_image(SN0Image *p)
{
    unsigned int jump = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);

    sn0_patch_branch(p, jump, ENTRY);
    for (unsigned int vector = SN0_VEC_TLB_REFILL;
         vector <= SN0_VEC_GENERAL; vector += 0x80) {
        p->pos = vector;
        jump = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
        sn0_patch_branch(p, jump, FAULT_ENTRY);
    }
    p->pos = FAULT_ENTRY;
    sn0_emit_li(p, SN0_R_K0, STATE_GUEST);
    sn0_emit_li(p, SN0_R_K1, 1);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_K1, FAULT);
    sn0_emit_park(p);

    p->pos = ENTRY;
    sn0_emit_li(p, SN0_R_S0, STATE_GUEST);
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_S0, SN0_R_T0, GENERATION);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_T0, SN0_R_T0, 1);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S0, SN0_R_T0, GENERATION);
    sn0_emit_sll(p, SN0_R_T1, SN0_R_T0, 5);
    sn0_emit_addu(p, SN0_R_S1, SN0_R_S0, SN0_R_T1);
    sn0_emit_mfc0(p, SN0_R_T0, 16);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S1, SN0_R_T0, CONFIG);

    sn0_emit_mtc0(p, SN0_R_ZERO, 29);
    sn0_emit_li64(p, SN0_R_S2, LINE_A);
    sn0_emit_li64(p, SN0_R_T8, LINE_A + 64);
    emit_tag_store(p, SN0_R_S2, TAG_VALUE_A);
    emit_tag_store(p, SN0_R_T8, TAG_VALUE_B);
    emit_tag_read(p, SN0_R_S2, TAG_A);
    emit_tag_read(p, SN0_R_T8, TAG_B);

    sn0_emit_i(p, SN0_OPC_LW, SN0_R_S0, SN0_R_T0, GENERATION);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S0, SN0_R_T0, COMPLETED);
    sn0_emit_park(p);
}

static void wait_for_pass(QTestState *qts, unsigned int generation)
{
    SgiWait wait;

    sgi_wait_start(&wait, "R10000 secondary set pass %u", generation);
    while (qtest_readl(qts, STATE_FULL + COMPLETED) != generation) {
        g_assert_cmphex(qtest_readl(qts, STATE_FULL + FAULT), ==, 0);
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmphex(qtest_readl(qts, STATE_FULL + GENERATION), ==,
                    generation);
}

static void check_pass(QTestState *qts, const ScacheResetCase *c,
                       unsigned int generation)
{
    unsigned int base = STATE_FULL + RESULT_BASE +
                        (generation - 1) * RESULT_STRIDE;

    g_assert_cmphex(qtest_readl(qts, base + CONFIG), ==, c->config);
    g_assert_cmphex(qtest_readl(qts, base + TAG_A), ==, c->expected_a);
    g_assert_cmphex(qtest_readl(qts, base + TAG_B), ==, TAG_VALUE_B);
}

static void run_case(gconstpointer opaque)
{
    const ScacheResetCase *c = opaque;
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    build_image(image);
    path = sn0_image_write(image, "o2-r10k-scache-reset");
    qts = qtest_initf("-M o2-test,password-clear-input=0,"
                      "rom-image=%s,rom-size=524288,"
                      "rom-offset=0,rom-pad-byte=238 "
                      "-cpu %s -m 64M -nodefaults "
                      "-accel tcg,thread=single -S", path, c->cpu);
    unlink(path);
    qtest_writel(qts, STATE_FULL + GENERATION, 0);
    qtest_writel(qts, STATE_FULL + COMPLETED, 0);
    qtest_writel(qts, STATE_FULL + FAULT, 0);

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_pass(qts, 1);
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    check_pass(qts, c, 1);

    qtest_writel(qts, STATE_FULL + COMPLETED, 0);
    qtest_system_reset(qts);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_pass(qts, 2);
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    check_pass(qts, c, 2);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    static const ScacheResetCase cases[] = {
        { "R10000", 0x6c19a603u, TAG_VALUE_B },
        { "R10000,reset-mode=0x00198603", 0x6c198603u, TAG_VALUE_A },
    };

    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("/o2/r10k/scache/reset/128", &cases[0], run_case);
    qtest_add_data_func("/o2/r10k/scache/reset/64", &cases[1], run_case);
    return g_test_run();
}
