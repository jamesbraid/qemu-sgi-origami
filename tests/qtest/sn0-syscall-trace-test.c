/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pin the CPU trace lines a host-side syscall tracer parses: exception
 * entry, syscall arguments, ERET and TLB writes, each stamped with the CPU
 * index and the EntryHi ASID.  A guest that runs one known syscall from
 * user code under ASID 5 must produce exactly these lines.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sn0-user-image.h"

#define MACHINE "-M origin200 -m 128M -cpu R10000 -accel tcg,thread=multi -S"

#define HANDLER         0x400
#define ENTRY           0x800
#define ASID            5
#define SYSCALL_V0      0x1234u
#define SYSCALL_A0      0x5678u
#define FINISH_V0       2

#define MB_COUNT        0
#define MB_STATUS       4
#define STATUS_DONE     1
#define STATUS_FAIL     0xff

/*
 * The general vector counts syscalls and steps EPC past each one; v0 == 2
 * ends the run.  Any other exception, and any refill, reports STATUS_FAIL
 * so a wrong path cannot pass as a missing trace line.
 */
static unsigned int build_image(SN0Image *p)
{
    unsigned int at, refill, xrefill, fail, finish, tlbwi;

    at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
    sn0_patch_branch(p, at, ENTRY);
    p->pos = SN0_VEC_TLB_REFILL;
    refill = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
    p->pos = SN0_VEC_XTLB_REFILL;
    xrefill = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
    p->pos = SN0_VEC_GENERAL;
    at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
    sn0_patch_branch(p, at, HANDLER);

    p->pos = HANDLER;
    sn0_emit_li(p, SN0_R_K0, SN0_KSEG1(SN0_USER_MAILBOX));
    sn0_emit_mfc0(p, SN0_R_K1, SN0_CP0_CAUSE);
    sn0_emit_i(p, SN0_OPC_ANDI, SN0_R_K1, SN0_R_K1, 0x7c);
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T8, 8 << 2);
    fail = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_K1, SN0_R_T8);
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T8, FINISH_V0);
    finish = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_V0, SN0_R_T8);
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_K0, SN0_R_T8, MB_COUNT);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_T8, SN0_R_T8, 1);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_T8, MB_COUNT);
    sn0_emit_mfc0(p, SN0_R_T8, SN0_CP0_EPC);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_T8, SN0_R_T8, 4);
    sn0_emit_mtc0(p, SN0_R_T8, SN0_CP0_EPC);
    sn0_emit_eret(p);

    sn0_patch_branch(p, finish, p->pos);
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T8, STATUS_DONE);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_T8, MB_STATUS);
    sn0_emit_park(p);

    sn0_patch_branch(p, fail, p->pos);
    sn0_patch_branch(p, refill, p->pos);
    sn0_patch_branch(p, xrefill, p->pos);
    sn0_emit_li(p, SN0_R_K0, SN0_KSEG1(SN0_USER_MAILBOX));
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T8, STATUS_FAIL);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_T8, MB_STATUS);
    sn0_emit_park(p);
    g_assert_cmpuint(p->pos, <=, ENTRY);

    p->pos = ENTRY;
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    tlbwi = sn0_emit_map_page(p, 0, SN0_USER_TEXT, SN0_USER_PHYS, ASID);
    sn0_emit_li(p, SN0_R_T0, SN0_USER_TEXT + 0x800);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_EPC);
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_TO_USER);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_eret(p);
    return tlbwi;
}

/* One syscall with known arguments, then the finishing syscall. */
static unsigned int build_user_code(SN0Image *u)
{
    unsigned int syscall_at;

    sn0_emit_li(u, SN0_R_V0, SYSCALL_V0);
    sn0_emit_li(u, SN0_R_A0, SYSCALL_A0);
    sn0_emit_i(u, SN0_OPC_ADDIU, 0, SN0_R_A1, 1);
    sn0_emit_i(u, SN0_OPC_ADDIU, 0, SN0_R_A2, 2);
    sn0_emit_i(u, SN0_OPC_ADDIU, 0, SN0_R_A3, 3);
    syscall_at = u->pos;
    sn0_emit_syscall(u);
    sn0_emit_i(u, SN0_OPC_ADDIU, 0, SN0_R_V0, FINISH_V0);
    sn0_emit_syscall(u);
    sn0_emit_park(u);
    return syscall_at;
}

static unsigned int count_lines(const char *trace, const char *prefix)
{
    unsigned int count = 0;

    for (const char *at = trace; (at = strstr(at, prefix)); at += 1) {
        if (at == trace || at[-1] == '\n') {
            count++;
        }
    }
    return count;
}

static void test_syscall_trace(void)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree SN0Image *user = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    g_autofree char *trace_path = NULL;
    g_autofree char *trace = NULL;
    g_autofree char *expect = NULL;
    uint8_t mailbox[8];
    unsigned int tlbwi, syscall_pc;
    QTestState *qts;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    tlbwi = build_image(image);
    syscall_pc = SN0_USER_TEXT + 0x800 + build_user_code(user);
    path = sn0_image_write(image, "sn0-syscall-trace");
    fd = g_file_open_tmp("sn0-syscall-trace-events-XXXXXX", &trace_path,
                         NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf(MACHINE " -bios %s"
                      " -trace mips_exception,file=%s"
                      " -trace mips_syscall,file=%s"
                      " -trace mips_eret,file=%s"
                      " -trace mips_tlb_write,file=%s",
                      path, trace_path, trace_path, trace_path, trace_path);
    unlink(path);
    qtest_memwrite(qts, SN0_USER_CODE, user->bytes, user->pos);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    g_assert_cmpuint(sn0_wait_word(qts, 0, SN0_USER_MAILBOX + MB_STATUS, 30),
                     ==, STATUS_DONE);
    qtest_memread(qts, SN0_USER_MAILBOX, mailbox, sizeof(mailbox));
    g_assert_cmpuint(ldl_be_p(mailbox + MB_COUNT), ==, 1);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, NULL));
    g_assert_cmpint(g_unlink(trace_path), ==, 0);
    g_test_message("%s", trace);

    expect = g_strdup_printf("mips_tlb_write cpu 0 idx 0 entryhi 0x%x"
                             " lo0 0x%x lo1 0x1 pagemask 0x0"
                             " pc 0xffffffff%08x\n",
                             SN0_USER_TEXT | ASID,
                             (SN0_USER_PHYS >> 6) | 0x1f,
                             SN0_IMAGE_BASE + tlbwi);
    g_assert_nonnull(strstr(trace, expect));
    g_free(expect);

    /* The kernel's own ERET into user code, then the one after the syscall. */
    expect = g_strdup_printf("mips_eret cpu 0 asid 0x5 pc 0x%x"
                             " status 0x00400090 v0 0x0 a3 0x0 user 1\n",
                             SN0_USER_TEXT + 0x800);
    g_assert_nonnull(strstr(trace, expect));
    g_free(expect);
    expect = g_strdup_printf("mips_exception cpu 0 asid 0x5 exccode 8"
                             " name \"syscall\" pc 0x%x badvaddr 0x0"
                             " status 0x00400092 cause 0x", syscall_pc);
    g_assert_nonnull(strstr(trace, expect));
    g_free(expect);
    expect = g_strdup_printf("mips_syscall cpu 0 asid 0x5 pc 0x%x"
                             " v0 0x1234 a0 0x5678 a1 0x1 a2 0x2 a3 0x3\n",
                             syscall_pc);
    g_assert_nonnull(strstr(trace, expect));
    g_free(expect);
    expect = g_strdup_printf("mips_eret cpu 0 asid 0x5 pc 0x%x"
                             " status 0x00400090 v0 0x1234 a3 0x3 user 1\n",
                             syscall_pc + 4);
    g_assert_nonnull(strstr(trace, expect));

    /* The finishing syscall traces too; nothing else returns to user mode. */
    g_assert_cmpuint(count_lines(trace, "mips_syscall "), ==, 2);
    g_assert_cmpuint(count_lines(trace, "mips_eret "), ==, 2);
    g_assert_cmpuint(count_lines(trace, "mips_tlb_write "), ==, 1);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/syscall-trace/one-syscall", test_syscall_trace);
    return g_test_run();
}
