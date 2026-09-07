/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The monitor's register dump names the CP0 state a guest debugger needs. */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sn0-user-image.h"

#define MACHINE "-M origin200 -m 128M -cpu R10000 -accel tcg,thread=multi -S"

#define HANDLER         0x400
#define ENTRY           0x800
#define ENTRYHI_VALUE   0x00400005u
#define FAULT_ADDRESS   0x12345670u
#define ERROREPC_VALUE  0x1bad0000u
#define INDEX_VALUE     3
#define WIRED_VALUE     1

/*
 * Program the registers under test, then touch an unmapped address so the
 * refill exception rewrites EntryHi's VPN2 and BadVAddr on the way to the
 * handler, which flags the mailbox and parks.
 */
static void build_image(SN0Image *p)
{
    unsigned int at;

    at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
    sn0_patch_branch(p, at, ENTRY);
    for (unsigned int vec = SN0_VEC_TLB_REFILL; vec <= SN0_VEC_GENERAL;
         vec += 0x80) {
        p->pos = vec;
        at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
        sn0_patch_branch(p, at, HANDLER);
    }

    p->pos = HANDLER;
    sn0_emit_li(p, SN0_R_K0, SN0_KSEG1(SN0_USER_MAILBOX));
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_K1, 1);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_K1, 0);
    sn0_emit_park(p);

    p->pos = ENTRY;
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_li(p, SN0_R_T0, ENTRYHI_VALUE);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_ENTRYHI);
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T0, INDEX_VALUE);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_INDEX);
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T0, WIRED_VALUE);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_WIRED);
    sn0_emit_li(p, SN0_R_T0, ERROREPC_VALUE);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_ERROREPC);
    sn0_emit_li(p, SN0_R_T0, FAULT_ADDRESS);
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_T0, SN0_R_T1, 0);
    sn0_emit_park(p);
}

static void test_info_registers_cp0(void)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    g_autofree char *registers = NULL;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    build_image(image);
    path = sn0_image_write(image, "sn0-cpu-registers");
    qts = qtest_initf(MACHINE " -bios %s", path);
    unlink(path);

    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    g_assert_cmpuint(sn0_wait_word(qts, 0, SN0_USER_MAILBOX, 30), ==, 1);
    qtest_qmp_assert_success(qts, "{ 'execute': 'stop' }");
    registers = qtest_hmp(qts, "info registers");
    qtest_quit(qts);

    g_test_message("%s", registers);
    g_assert_nonnull(strstr(registers, "EntryHi 0x0000000012344005"));
    g_assert_nonnull(strstr(registers, "ASID 0x05"));
    g_assert_nonnull(strstr(registers, "BadVAddr 0x0000000012345670"));
    g_assert_nonnull(strstr(registers, "ErrorEPC 0x000000001bad0000"));
    g_assert_nonnull(strstr(registers, "Index 0x00000003"));
    g_assert_nonnull(strstr(registers, "Wired 0x00000001"));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/cpu-registers/info-registers-cp0",
                   test_info_registers_cp0);
    return g_test_run();
}
