/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * x-query-mips-tlb reads one CPU's TLB and TLB-related CP0 registers on
 * that CPU's own thread.  Each CPU writes two entries with tlbwr under
 * Wired = 63, so both land in index 63 and the emulator keeps the first
 * as a shadow beyond the architectural entries; the query must report the
 * live entry, the shadow only when asked, and on the two-node machine the
 * queried CPU's own ASID rather than another CPU's.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sn0-user-image.h"

#define SINGLE_MACHINE \
    "-M origin200 -m 128M -cpu R10000 -accel tcg,thread=multi -S"
#define FOUR_CPU_MACHINE \
    "-M origin200,topology=origin200-dual " SN0_TWO_NODE_TOPOLOGY \
    " -cpu R10000 -accel tcg,thread=multi -S"

#define ENTRY           0x800
#define NB_TLB          64
#define WIRED           (NB_TLB - 1)
#define INDEX_VALUE     9
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
static void build_image(SN0Image *p)
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
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T0, INDEX_VALUE);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_INDEX);

    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T8, 1);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S2, SN0_R_T8, 0);
    sn0_emit_park(p);
}

static void seed_node(QTestState *qts, unsigned int cpu, uint32_t node_base)
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
    g_assert_cmpint(qdict_get_int(state, "index"), ==, INDEX_VALUE);
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
    build_image(image);
    path = sn0_image_write(image, "sn0-tlb-query");
    qts = qtest_initf("%s -bios %s",
                      four_cpu ? FOUR_CPU_MACHINE : SINGLE_MACHINE, path);
    unlink(path);
    seed_node(qts, 0, 0);
    if (four_cpu) {
        seed_node(qts, 2, 2);
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/tlb-query/live-and-shadow", test_query);
    qtest_add_func("/sn0/tlb-query/live-and-shadow-on-cpu2", test_query_cpu2);
    return g_test_run();
}
