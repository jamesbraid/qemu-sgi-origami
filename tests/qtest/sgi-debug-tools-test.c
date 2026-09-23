/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The host-side debugging and tracing surfaces a guest debugger relies on:
 * the monitor's CP0 register dump, the CPU trace lines a host-side syscall
 * tracer parses, x-query-mips-tlb's view of one CPU's TLB, and ASID- and
 * ExcCode-filtered breakpoints through x-mips-debug-breakpoint.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sn0-user-image.h"

#define MACHINE \
    "-M origin200 -m 128M -cpu R10000 -accel tcg,thread=multi -S"
#define SINGLE_MACHINE  MACHINE
#define FOUR_CPU_MACHINE \
    "-M origin200,topology=origin200-dual " SN0_TWO_NODE_TOPOLOGY \
    " -cpu R10000 -accel tcg,thread=multi -S"

#define HANDLER         0x400
#define ENTRY           0x800

/* The monitor's register dump names the CP0 state a guest debugger needs. */

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
static void cpu_registers_build_image(SN0Image *p)
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
    cpu_registers_build_image(image);
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

/*
 * Pin the CPU trace lines a host-side syscall tracer parses: exception
 * entry, syscall arguments, ERET and TLB writes, each stamped with the CPU
 * index and the EntryHi ASID.  A guest that runs one known syscall from
 * user code under ASID 5 must produce exactly these lines.
 */

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
static unsigned int syscall_trace_build_image(SN0Image *p)
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
    tlbwi = syscall_trace_build_image(image);
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

/*
 * x-query-mips-tlb reads one CPU's TLB and TLB-related CP0 registers on
 * that CPU's own thread.  Each CPU writes two entries with tlbwr under
 * Wired = 63, so both land in index 63 and the emulator keeps the first
 * as a shadow beyond the architectural entries; the query must report the
 * live entry, the shadow only when asked, and on the two-node machine the
 * queried CPU's own ASID rather than another CPU's.
 */

#define NB_TLB          64
#define WIRED           (NB_TLB - 1)
#define TLB_INDEX_VALUE 9
#define BASE_ASID       5
#define MB_STRIDE       32

#define FIRST_VPN2      0x00400000u
#define FIRST_PFN0      0x00200000u
#define FIRST_PFN1      0x00201000u
#define SECOND_VPN2     0x00600000u
#define SECOND_PFN0     0x00300000u
#define SECOND_PFN1     0x00301000u

/* EntryLo: PFN from bit 6, C in 5:3, D bit 2, V bit 1, G bit 0. */
#define FIRST_LO0       ((FIRST_PFN0 >> 6) | (3 << 3) | 0x6)
#define FIRST_LO1       ((FIRST_PFN1 >> 6) | (2 << 3) | 0x2)
#define FIRST_UC1       2
#define SECOND_LO0      ((SECOND_PFN0 >> 6) | (3 << 3) | 0x7)
#define SECOND_LO1      ((SECOND_PFN1 >> 6) | (3 << 3) | 0x7)

static uint32_t mailbox_of(unsigned int cpu)
{
    return SN0_USER_MAILBOX + cpu * MB_STRIDE;
}

/*
 * Each CPU tags its entries with ASID 5 plus its own index, writes the
 * first pair not global with an uncached attribute on the odd half, then
 * the second pair global, and flags its mailbox.
 */
static void tlb_query_build_image(SN0Image *p)
{
    unsigned int at;

    at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
    sn0_patch_branch(p, at, ENTRY);
    for (unsigned int vec = SN0_VEC_TLB_REFILL; vec <= SN0_VEC_GENERAL;
         vec += 0x80) {
        p->pos = vec;
        sn0_emit_park(p);
    }

    p->pos = ENTRY;
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_li(p, SN0_R_T1, SN0_KSEG1(SN0_NODE_ID_PHYS));
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_T1, SN0_R_T1, 0);
    sn0_emit_cpu_number(p, SN0_R_T2);
    sn0_emit_or(p, SN0_R_S0, SN0_R_T1, SN0_R_T2);
    sn0_emit_sll(p, SN0_R_T0, SN0_R_S0, 5);
    sn0_emit_li(p, SN0_R_S2, SN0_KSEG1(SN0_USER_MAILBOX));
    sn0_emit_addu(p, SN0_R_S2, SN0_R_S2, SN0_R_T0);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_S0, SN0_R_S1, BASE_ASID);

    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T0, WIRED);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_WIRED);
    sn0_emit_mtc0(p, SN0_R_ZERO, SN0_CP0_PAGEMASK);
    sn0_emit_li(p, SN0_R_T0, FIRST_VPN2);
    sn0_emit_or(p, SN0_R_T0, SN0_R_T0, SN0_R_S1);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_ENTRYHI);
    sn0_emit_li(p, SN0_R_T0, FIRST_LO0);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_ENTRYLO0);
    sn0_emit_li64(p, SN0_R_T0, ((uint64_t)FIRST_UC1 << 62) | FIRST_LO1);
    sn0_emit_dmtc0(p, SN0_R_T0, SN0_CP0_ENTRYLO1);
    sn0_emit_tlbwr(p);
    sn0_emit_li(p, SN0_R_T0, SECOND_VPN2);
    sn0_emit_or(p, SN0_R_T0, SN0_R_T0, SN0_R_S1);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_ENTRYHI);
    sn0_emit_li(p, SN0_R_T0, SECOND_LO0);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_ENTRYLO0);
    sn0_emit_li(p, SN0_R_T0, SECOND_LO1);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_ENTRYLO1);
    sn0_emit_tlbwr(p);
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T0, TLB_INDEX_VALUE);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_INDEX);

    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T8, 1);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S2, SN0_R_T8, 0);
    sn0_emit_park(p);
}

static void tlb_query_seed_node(QTestState *qts, unsigned int cpu,
                                uint32_t node_base)
{
    uint8_t word[4];

    stl_be_p(word, node_base);
    g_assert_true(qtest_memwrite_cpu(qts, cpu, SN0_NODE_ID_PHYS, word,
                                     sizeof(word)));
}

static QDict *query_tlb(QTestState *qts, unsigned int cpu, bool shadow,
                        QDict **state)
{
    QDict *response;

    if (shadow) {
        response = qtest_qmp(qts,
            "{ 'execute': 'x-query-mips-tlb', 'arguments': "
            "{ 'cpu-index': %u, 'shadow': true } }", cpu);
    } else {
        response = qtest_qmp(qts,
            "{ 'execute': 'x-query-mips-tlb', 'arguments': "
            "{ 'cpu-index': %u } }", cpu);
    }
    *state = qdict_get_qdict(response, "return");
    g_assert_nonnull(*state);
    return response;
}

static QDict *entry_at(QList *entries, unsigned int index)
{
    const QListEntry *entry = qlist_first(entries);

    for (unsigned int i = 0; i < index; i++) {
        g_assert_nonnull(entry);
        entry = qlist_next(entry);
    }
    g_assert_nonnull(entry);
    return qobject_to(QDict, qlist_entry_obj(entry));
}

static void assert_entry(QDict *e, unsigned int index, uint32_t vpn2,
                         unsigned int asid, bool global,
                         uint32_t pfn0, bool dirty0, unsigned int cache0,
                         uint32_t pfn1, bool dirty1, unsigned int cache1,
                         unsigned int uc1)
{
    g_assert_cmpint(qdict_get_int(e, "index"), ==, index);
    g_assert_cmphex(qdict_get_uint(e, "vpn2"), ==, vpn2);
    g_assert_cmpint(qdict_get_int(e, "asid"), ==, asid);
    g_assert_true(qdict_get_bool(e, "global") == global);
    g_assert_cmpint(qdict_get_int(e, "page-mask"), ==, 0);
    g_assert_cmphex(qdict_get_uint(e, "pfn0"), ==, pfn0);
    g_assert_true(qdict_get_bool(e, "valid0"));
    g_assert_true(qdict_get_bool(e, "dirty0") == dirty0);
    g_assert_cmpint(qdict_get_int(e, "cache0"), ==, cache0);
    g_assert_cmpint(qdict_get_int(e, "uc0"), ==, 0);
    g_assert_cmphex(qdict_get_uint(e, "pfn1"), ==, pfn1);
    g_assert_true(qdict_get_bool(e, "valid1"));
    g_assert_true(qdict_get_bool(e, "dirty1") == dirty1);
    g_assert_cmpint(qdict_get_int(e, "cache1"), ==, cache1);
    g_assert_cmpint(qdict_get_int(e, "uc1"), ==, uc1);
}

static void check_tlb(QTestState *qts, unsigned int cpu)
{
    unsigned int asid = BASE_ASID + cpu;
    QDict *response, *state;
    QList *entries;

    response = query_tlb(qts, cpu, false, &state);
    g_assert_cmpint(qdict_get_int(state, "cpu-index"), ==, cpu);
    g_assert_cmphex(qdict_get_uint(state, "entryhi"), ==, SECOND_VPN2 | asid);
    g_assert_cmpint(qdict_get_int(state, "asid"), ==, asid);
    g_assert_cmpint(qdict_get_int(state, "index"), ==, TLB_INDEX_VALUE);
    g_assert_cmpint(qdict_get_int(state, "wired"), ==, WIRED);
    g_assert_cmpint(qdict_get_int(state, "nb-tlb"), ==, NB_TLB);
    g_assert_cmpint(qdict_get_int(state, "tlb-in-use"), ==, NB_TLB + 1);
    entries = qdict_get_qlist(state, "entries");
    g_assert_cmpuint(qlist_size(entries), ==, NB_TLB);
    g_assert_false(qdict_get_bool(entry_at(entries, 0), "valid0"));
    assert_entry(entry_at(entries, WIRED), WIRED, SECOND_VPN2, asid, true,
                 SECOND_PFN0, true, 3, SECOND_PFN1, true, 3, 0);
    qobject_unref(response);

    response = query_tlb(qts, cpu, true, &state);
    entries = qdict_get_qlist(state, "entries");
    g_assert_cmpuint(qlist_size(entries), ==, NB_TLB + 1);
    assert_entry(entry_at(entries, NB_TLB), NB_TLB, FIRST_VPN2, asid, false,
                 FIRST_PFN0, true, 3, FIRST_PFN1, false, 2, FIRST_UC1);
    qobject_unref(response);
}

static void run_query(bool four_cpu)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;
    QDict *response;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    tlb_query_build_image(image);
    path = sn0_image_write(image, "sn0-tlb-query");
    qts = qtest_initf("%s -bios %s",
                      four_cpu ? FOUR_CPU_MACHINE : SINGLE_MACHINE, path);
    unlink(path);
    tlb_query_seed_node(qts, 0, 0);
    if (four_cpu) {
        tlb_query_seed_node(qts, 2, 2);
    }
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    g_assert_cmpuint(sn0_wait_word(qts, 0, mailbox_of(0), 30), ==, 1);
    check_tlb(qts, 0);
    if (four_cpu) {
        g_assert_cmpuint(sn0_wait_word(qts, 2, mailbox_of(2), 30), ==, 1);
        check_tlb(qts, 2);
    }

    response = qtest_qmp(qts, "{ 'execute': 'x-query-mips-tlb', 'arguments': "
                              "{ 'cpu-index': 9 } }");
    g_assert_true(qdict_haskey(response, "error"));
    qobject_unref(response);
    qtest_quit(qts);
}

static void test_query(void)
{
    run_query(false);
}

static void test_query_cpu2(void)
{
    run_query(true);
}

/*
 * ASID- and ExcCode-filtered breakpoints through x-mips-debug-breakpoint.
 *
 * The target CPU takes three syscalls under ASID 5 and a break under ASID
 * 6 before the syscall under ASID 6 that the breakpoint filters for; the
 * machine must stop only there, on that CPU, with the earlier hits counted.
 * On the two-node machine the other CPUs take the same syscall under ASID
 * 6 first, so a breakpoint that ignored its CPU index would stop the
 * machine before the target ever reached user code.
 */

#define VECTOR_PC       UINT64_C(0xffffffffbfc00380)
#define TARGET_ASID     5
#define CONTROL_ASID    6
#define CALL_V0         0x100
#define TARGET_CODE     (SN0_USER_TEXT + 0x800)
#define CONTROL_CODE    (SN0_USER_TEXT + 0x900)

/*
 * The general vector accepts syscalls and breaks: it counts the hit, moves
 * EntryHi's ASID to a0, steps EPC past the instruction and returns; v0 == 2
 * ends the run.  Anything else reports STATUS_FAIL.  s2 holds the CPU's
 * mailbox pointer throughout and the user programs never touch it.
 *
 * Each CPU learns its index from the host-seeded node word and the Hub's
 * processor number.  The target CPU runs the target program under ASID 5,
 * after waiting for @peer to finish when there is one; every other CPU
 * runs the control program under ASID 6.
 */
static void debug_breakpoint_build_image(SN0Image *p, unsigned int target,
                                         int peer)
{
    unsigned int at, refill, xrefill, fail, ok, finish, control, common;

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
    sn0_emit_mfc0(p, SN0_R_K1, SN0_CP0_CAUSE);
    sn0_emit_i(p, SN0_OPC_ANDI, SN0_R_K1, SN0_R_K1, 0x7c);
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T8, 8 << 2);
    ok = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_K1, SN0_R_T8);
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T8, 9 << 2);
    fail = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_K1, SN0_R_T8);
    sn0_patch_branch(p, ok, p->pos);
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T8, FINISH_V0);
    finish = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_V0, SN0_R_T8);
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_S2, SN0_R_T8, MB_COUNT);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_T8, SN0_R_T8, 1);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S2, SN0_R_T8, MB_COUNT);
    sn0_emit_mfc0(p, SN0_R_T8, SN0_CP0_ENTRYHI);
    sn0_emit_srl(p, SN0_R_T8, SN0_R_T8, 8);
    sn0_emit_sll(p, SN0_R_T8, SN0_R_T8, 8);
    sn0_emit_or(p, SN0_R_T8, SN0_R_T8, SN0_R_A0);
    sn0_emit_mtc0(p, SN0_R_T8, SN0_CP0_ENTRYHI);
    sn0_emit_mfc0(p, SN0_R_T8, SN0_CP0_EPC);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_T8, SN0_R_T8, 4);
    sn0_emit_mtc0(p, SN0_R_T8, SN0_CP0_EPC);
    sn0_emit_eret(p);

    sn0_patch_branch(p, finish, p->pos);
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T8, STATUS_DONE);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S2, SN0_R_T8, MB_STATUS);
    sn0_emit_park(p);

    sn0_patch_branch(p, fail, p->pos);
    sn0_patch_branch(p, refill, p->pos);
    sn0_patch_branch(p, xrefill, p->pos);
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T8, STATUS_FAIL);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S2, SN0_R_T8, MB_STATUS);
    sn0_emit_park(p);
    g_assert_cmpuint(p->pos, <=, ENTRY);

    p->pos = ENTRY;
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_li(p, SN0_R_T1, SN0_KSEG1(SN0_NODE_ID_PHYS));
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_T1, SN0_R_T1, 0);
    sn0_emit_cpu_number(p, SN0_R_T2);
    sn0_emit_or(p, SN0_R_S0, SN0_R_T1, SN0_R_T2);
    sn0_emit_sll(p, SN0_R_T0, SN0_R_S0, 5);
    sn0_emit_li(p, SN0_R_S2, SN0_KSEG1(SN0_USER_MAILBOX));
    sn0_emit_addu(p, SN0_R_S2, SN0_R_S2, SN0_R_T0);
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T1, target);
    control = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_S0, SN0_R_T1);
    if (peer >= 0) {
        unsigned int wait;

        sn0_emit_li(p, SN0_R_T1, SN0_KSEG1(mailbox_of(peer)) + MB_STATUS);
        wait = p->pos;
        sn0_emit_i(p, SN0_OPC_LW, SN0_R_T1, SN0_R_T0, 0);
        at = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_T0, 0);
        sn0_patch_branch(p, at, wait);
    }
    sn0_emit_map_page(p, 0, SN0_USER_TEXT, SN0_USER_PHYS, TARGET_ASID);
    sn0_emit_li(p, SN0_R_T0, TARGET_CODE);
    common = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
    sn0_patch_branch(p, control, p->pos);
    sn0_emit_map_page(p, 0, SN0_USER_TEXT, SN0_USER_PHYS, CONTROL_ASID);
    sn0_emit_li(p, SN0_R_T0, CONTROL_CODE);
    sn0_patch_branch(p, common, p->pos);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_EPC);
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_TO_USER);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_eret(p);
}

static void build_target_code(SN0Image *u)
{
    sn0_emit_li(u, SN0_R_V0, CALL_V0);
    sn0_emit_i(u, SN0_OPC_ADDIU, 0, SN0_R_A0, TARGET_ASID);
    sn0_emit_syscall(u);                              /* 1: ASID 5 */
    sn0_emit_syscall(u);                              /* 2: ASID 5 */
    sn0_emit_i(u, SN0_OPC_ADDIU, 0, SN0_R_A0, CONTROL_ASID);
    sn0_emit_syscall(u);                              /* 3: ASID 5 -> 6 */
    sn0_emit_break(u);                                /* 4: ASID 6, code 9 */
    sn0_emit_syscall(u);                              /* 5: the filtered hit */
    sn0_emit_i(u, SN0_OPC_ADDIU, 0, SN0_R_V0, FINISH_V0);
    sn0_emit_syscall(u);
    sn0_emit_park(u);
}

static void build_control_code(SN0Image *u)
{
    sn0_emit_li(u, SN0_R_V0, CALL_V0);
    sn0_emit_i(u, SN0_OPC_ADDIU, 0, SN0_R_A0, CONTROL_ASID);
    for (unsigned int i = 0; i < 3; i++) {
        sn0_emit_syscall(u);
    }
    sn0_emit_i(u, SN0_OPC_ADDIU, 0, SN0_R_V0, FINISH_V0);
    sn0_emit_syscall(u);
    sn0_emit_park(u);
}

static void debug_breakpoint_seed_node(QTestState *qts, unsigned int cpu,
                                       uint32_t node_base,
                                       const SN0Image *target,
                                       const SN0Image *control)
{
    uint8_t word[4];

    stl_be_p(word, node_base);
    g_assert_true(qtest_memwrite_cpu(qts, cpu, SN0_NODE_ID_PHYS, word,
                                     sizeof(word)));
    g_assert_true(qtest_memwrite_cpu(qts, cpu, SN0_USER_CODE,
                                     target->bytes, target->pos));
    g_assert_true(qtest_memwrite_cpu(qts, cpu, SN0_USER_CODE + 0x100,
                                     control->bytes, control->pos));
}

static void read_mailbox(QTestState *qts, unsigned int cpu,
                         uint32_t *count, uint32_t *status)
{
    uint8_t mailbox[8];

    g_assert_true(qtest_memread_cpu(qts, cpu, mailbox_of(cpu), mailbox,
                                    sizeof(mailbox)));
    *count = ldl_be_p(mailbox + MB_COUNT);
    *status = ldl_be_p(mailbox + MB_STATUS);
}

static void breakpoint(QTestState *qts, unsigned int cpu, bool remove)
{
    if (remove) {
        qtest_qmp_assert_success(qts,
            "{ 'execute': 'x-mips-debug-breakpoint', 'arguments': "
            "{ 'cpu-index': %u, 'pc': %" PRIu64 ", 'asid': %d, "
            "'exccode': 8, 'remove': true } }",
            cpu, VECTOR_PC, CONTROL_ASID);
    } else {
        qtest_qmp_assert_success(qts,
            "{ 'execute': 'x-mips-debug-breakpoint', 'arguments': "
            "{ 'cpu-index': %u, 'pc': %" PRIu64 ", 'asid': %d, "
            "'exccode': 8 } }",
            cpu, VECTOR_PC, CONTROL_ASID);
    }
}

static QDict *query_breakpoints(QTestState *qts, QList **table)
{
    QDict *response;

    response = qtest_qmp(qts,
                         "{ 'execute': 'x-query-mips-debug-breakpoints' }");
    *table = qdict_get_qlist(response, "return");
    g_assert_nonnull(*table);
    return response;
}

/* HMP through QMP gets a fresh monitor per command, so "cpu N" cannot stick. */
static char *info_registers(QTestState *qts, unsigned int cpu)
{
    QDict *response;
    char *registers;

    response = qtest_qmp(qts,
        "{ 'execute': 'human-monitor-command', 'arguments': "
        "{ 'command-line': 'info registers', 'cpu-index': %u } }", cpu);
    registers = g_strdup(qdict_get_str(response, "return"));
    qobject_unref(response);
    return registers;
}

static void assert_status(QTestState *qts, const char *expected)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'query-status' }");
    QDict *status = qdict_get_qdict(response, "return");

    g_assert_cmpstr(qdict_get_str(status, "status"), ==, expected);
    qobject_unref(response);
}

static void run_filtered_stop(bool four_cpu)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree SN0Image *target_code = g_new0(SN0Image, 1);
    g_autofree SN0Image *control_code = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    g_autofree char *registers = NULL;
    unsigned int target = four_cpu ? 2 : 0;
    uint32_t count, status;
    QTestState *qts;
    QDict *response, *entry;
    QList *table;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    debug_breakpoint_build_image(image, target, four_cpu ? 3 : -1);
    build_target_code(target_code);
    build_control_code(control_code);
    path = sn0_image_write(image, "sn0-debug-breakpoint");
    qts = qtest_initf("%s -bios %s",
                      four_cpu ? FOUR_CPU_MACHINE : SINGLE_MACHINE, path);
    unlink(path);
    debug_breakpoint_seed_node(qts, 0, 0, target_code, control_code);
    if (four_cpu) {
        debug_breakpoint_seed_node(qts, 2, 2, target_code, control_code);
    }

    breakpoint(qts, target, false);
    response = query_breakpoints(qts, &table);
    g_assert_cmpuint(qlist_size(table), ==, 1);
    entry = qobject_to(QDict, qlist_entry_obj(qlist_first(table)));
    g_assert_cmpint(qdict_get_int(entry, "cpu-index"), ==, target);
    g_assert_cmphex(qdict_get_uint(entry, "pc"), ==, VECTOR_PC);
    g_assert_cmpint(qdict_get_int(entry, "asid"), ==, CONTROL_ASID);
    g_assert_cmpint(qdict_get_int(entry, "exccode"), ==, 8);
    qobject_unref(response);

    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    qtest_qmp_eventwait(qts, "STOP");
    assert_status(qts, "debug");
    registers = info_registers(qts, target);
    g_test_message("%s", registers);
    g_assert_nonnull(strstr(registers, "pc=0xffffffffbfc00380 "));
    g_assert_nonnull(strstr(registers, " ASID 0x06 "));

    /* Three ASID-5 syscalls and the ASID-6 break ran without stopping. */
    read_mailbox(qts, target, &count, &status);
    g_assert_cmpuint(count, ==, 4);
    g_assert_cmpuint(status, ==, 0);
    if (four_cpu) {
        read_mailbox(qts, 3, &count, &status);
        g_assert_cmpuint(count, ==, 3);
        g_assert_cmpuint(status, ==, STATUS_DONE);
    }

    breakpoint(qts, target, true);
    response = query_breakpoints(qts, &table);
    g_assert_cmpuint(qlist_size(table), ==, 0);
    qobject_unref(response);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    g_assert_cmpuint(sn0_wait_word(qts, target, mailbox_of(target) + MB_STATUS,
                                   30), ==, STATUS_DONE);
    read_mailbox(qts, target, &count, &status);
    g_assert_cmpuint(count, ==, 5);
    if (four_cpu) {
        for (unsigned int cpu = 0; cpu < 2; cpu++) {
            g_assert_cmpuint(sn0_wait_word(qts, cpu,
                                           mailbox_of(cpu) + MB_STATUS, 30),
                             ==, STATUS_DONE);
        }
    }
    qtest_quit(qts);
}

static void test_filtered_stop(void)
{
    run_filtered_stop(false);
}

static void test_filtered_stop_on_cpu2(void)
{
    run_filtered_stop(true);
}

static void test_errors(void)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;
    QDict *response;

    /* Nothing here runs guest code; the machine just needs a valid -bios. */
    sn0_emit_park(image);
    path = sn0_image_write(image, "sn0-debug-breakpoint-errors");
    qts = qtest_initf("%s -bios %s", SINGLE_MACHINE, path);
    unlink(path);

    response = qtest_qmp(qts,
        "{ 'execute': 'x-mips-debug-breakpoint', 'arguments': "
        "{ 'cpu-index': 7, 'pc': %" PRIu64 " } }", VECTOR_PC);
    g_assert_true(qdict_haskey(response, "error"));
    qobject_unref(response);

    response = qtest_qmp(qts,
        "{ 'execute': 'x-mips-debug-breakpoint', 'arguments': "
        "{ 'cpu-index': 0, 'pc': %" PRIu64 ", 'remove': true } }",
        VECTOR_PC);
    g_assert_true(qdict_haskey(response, "error"));
    qobject_unref(response);

    breakpoint(qts, 0, false);
    response = qtest_qmp(qts,
        "{ 'execute': 'x-mips-debug-breakpoint', 'arguments': "
        "{ 'cpu-index': 0, 'pc': %" PRIu64 ", 'asid': %d, 'exccode': 8 } }",
        VECTOR_PC, CONTROL_ASID);
    g_assert_true(qdict_haskey(response, "error"));
    qobject_unref(response);
    breakpoint(qts, 0, true);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/cpu-registers/info-registers-cp0",
                   test_info_registers_cp0);
    qtest_add_func("/sn0/syscall-trace/one-syscall", test_syscall_trace);
    qtest_add_func("/sn0/tlb-query/live-and-shadow", test_query);
    qtest_add_func("/sn0/tlb-query/live-and-shadow-on-cpu2", test_query_cpu2);
    qtest_add_func("/sn0/debug-breakpoint/filtered-stop", test_filtered_stop);
    qtest_add_func("/sn0/debug-breakpoint/filtered-stop-on-cpu2",
                   test_filtered_stop_on_cpu2);
    qtest_add_func("/sn0/debug-breakpoint/errors", test_errors);
    return g_test_run();
}
