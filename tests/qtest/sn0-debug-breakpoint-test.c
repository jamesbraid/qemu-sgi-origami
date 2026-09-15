/* SPDX-License-Identifier: GPL-2.0-or-later */
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

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sn0-user-image.h"

#define SINGLE_MACHINE \
    "-M origin200 -m 128M -cpu R10000 -accel tcg,thread=multi -S"
#define FOUR_CPU_MACHINE \
    "-M origin200 " SN0_TWO_NODE_TOPOLOGY \
    " -cpu R10000 -accel tcg,thread=multi -S"

#define VECTOR_PC       UINT64_C(0xffffffffbfc00380)
#define HANDLER         0x400
#define ENTRY           0x800
#define TARGET_ASID     5
#define CONTROL_ASID    6
#define CALL_V0         0x100
#define FINISH_V0       2
#define TARGET_CODE     (SN0_USER_TEXT + 0x800)
#define CONTROL_CODE    (SN0_USER_TEXT + 0x900)

#define MB_STRIDE       32
#define MB_COUNT        0
#define MB_STATUS       4
#define STATUS_DONE     1
#define STATUS_FAIL     0xff

static uint32_t mailbox_of(unsigned int cpu)
{
    return SN0_USER_MAILBOX + cpu * MB_STRIDE;
}

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
static void build_image(SN0Image *p, unsigned int target, int peer)
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

static void seed_node(QTestState *qts, unsigned int cpu, uint32_t node_base,
                      const SN0Image *target, const SN0Image *control)
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
    build_image(image, target, four_cpu ? 3 : -1);
    build_target_code(target_code);
    build_control_code(control_code);
    path = sn0_image_write(image, "sn0-debug-breakpoint");
    qts = qtest_initf("%s -bios %s",
                      four_cpu ? FOUR_CPU_MACHINE : SINGLE_MACHINE, path);
    unlink(path);
    seed_node(qts, 0, 0, target_code, control_code);
    if (four_cpu) {
        seed_node(qts, 2, 2, target_code, control_code);
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
    qtest_add_func("/sn0/debug-breakpoint/filtered-stop", test_filtered_stop);
    qtest_add_func("/sn0/debug-breakpoint/filtered-stop-on-cpu2",
                   test_filtered_stop_on_cpu2);
    qtest_add_func("/sn0/debug-breakpoint/errors", test_errors);
    return g_test_run();
}
