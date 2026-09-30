/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Two-node, four-CPU Origin 200 TCG tests: node-one CPUs execute and a
 * running CPU addresses both nodes. The low-probability initialization race
 * is covered by a separate soak. Each CPU fetches its reset vector from
 * local flash; the removed secondary halt and node-zero fetch redirect
 * leave no launch step or remote image fetch. The host seeds each node's
 * position-independent SN0_NODE_ID_PHYS window before CPU release; Hub PI
 * bit 0 gives the local slice. Their OR is the global CPU index used here.
 * Unqualified uncached reads resolve to the caller's node. With Status.KX,
 * NASID 1 has physical bit 32 (node_stride 0x1_0000_0000) and xkphys address
 * 0x9600_0001_0000_0000 | P from any CPU; node zero has no independent
 * address from node one. This is current model behavior, not a hardware
 * claim (see DUAL_NODE_TEST_STATE in
 * tests/functional/mips64/test_sn0_smp.py).
 * A node-zero CPU reads
 * distinct host-planted words locally and through NASID 1, proving live CPU
 * selection rather than the passive monitor promdata read in
 * sn0-address-map-test.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sn0-user-image.h"

#define R_T3 11
#define R_T4 12
#define R_T5 13

#define MACHINE_ARGS \
    "-M origin200,topology=origin200-dual " SN0_TWO_NODE_TOPOLOGY \
    " -cpu R10000 -accel tcg,thread=multi -S"

/*
 * Result slots, node-relative: every node holds its own copy at the same
 * offset, so the host tells them apart by reading node-explicit physical
 * addresses rather than by anything the code computes.
 */
#define RESULT_BASE          0x2000u   /* two slots, one per local slice */
#define PRIMARY_MAGIC         0x5a5aa5a5u

#define NODE_DATA_OFF         0x3000u  /* same offset, planted differently */
#define NODE0_DATA            UINT64_C(0x00000000feed0000)
#define NODE1_DATA            UINT64_C(0x00000000feed0001)
#define R_NODE0_SLOT          0x3100u  /* the reader's own node-zero memory */
#define R_NODE1_SLOT          0x3108u

#define NUMA_NODE1_BASE        UINT64_C(0x100000000)  /* node_stride */

/*
 * Every CPU runs this. It learns its own identity, then writes a mark to
 * its own node's copy of RESULT_BASE at its local slice's slot: CPU 0's
 * mark is PRIMARY_MAGIC, every other CPU's is its identity repeated in
 * both halfwords.
 */
#define ENTRY 0x800

static void stub_exception_vectors(SN0Image *p)
{
    unsigned int at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);

    sn0_patch_branch(p, at, ENTRY);
    for (unsigned int vec = SN0_VEC_TLB_REFILL; vec <= SN0_VEC_GENERAL;
         vec += 0x80) {
        p->pos = vec;
        sn0_emit_park(p);
    }
    p->pos = ENTRY;
}

static void build_execute_image(SN0Image *p)
{
    unsigned int primary, store;

    stub_exception_vectors(p);
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);

    sn0_emit_li(p, SN0_R_T1, SN0_KSEG1(SN0_NODE_ID_PHYS));
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_T1, SN0_R_T1, 0);   /* t1 = node_base */
    sn0_emit_cpu_number(p, SN0_R_T2);                   /* t2 = local slice */
    sn0_emit_or(p, SN0_R_S0, SN0_R_T1, SN0_R_T2);        /* s0 = identity */

    primary = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_S0, SN0_R_ZERO);
    sn0_emit_sll(p, R_T5, SN0_R_S0, 16);
    sn0_emit_or(p, R_T5, R_T5, SN0_R_S0);
    store = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, primary, p->pos);
    sn0_emit_li(p, R_T5, PRIMARY_MAGIC);
    sn0_patch_branch(p, store, p->pos);

    sn0_emit_sll(p, R_T3, SN0_R_T2, 3);                 /* t3 = slice * 8 */
    sn0_emit_li(p, R_T4, SN0_KSEG1(RESULT_BASE));
    sn0_emit_addu(p, R_T4, R_T4, R_T3);
    sn0_emit_i(p, SN0_OPC_SW, R_T4, R_T5, 0);
    sn0_emit_park(p);
}

/*
 * Every CPU learns its identity the same way; only identity 0 (node zero,
 * slice zero) does anything else. It reads its own node unqualified and
 * node one by its NASID-explicit xkphys, storing each readback to its own
 * node's result slots. Only this direction is a fact the current model
 * supports: node one's memory has a NASID-explicit form reachable from any
 * CPU, but node zero's does not (the addressing-fact comment in
 * tests/functional/mips64/test_sn0_smp.py), so the reader has to be on
 * node zero, not node one.
 */
static void build_fabric_read_image(SN0Image *p)
{
    unsigned int not_reader, park;

    stub_exception_vectors(p);
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);

    sn0_emit_li(p, SN0_R_T1, SN0_KSEG1(SN0_NODE_ID_PHYS));
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_T1, SN0_R_T1, 0);
    sn0_emit_cpu_number(p, SN0_R_T2);
    sn0_emit_or(p, SN0_R_S0, SN0_R_T1, SN0_R_T2);

    not_reader = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_S0, SN0_R_ZERO);

    sn0_emit_li(p, SN0_R_T0, SN0_KSEG1(NODE_DATA_OFF));
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_T0, SN0_R_T2, 0);
    sn0_emit_li(p, R_T3, SN0_KSEG1(R_NODE0_SLOT));
    sn0_emit_i(p, SN0_OPC_SD, R_T3, SN0_R_T2, 0);

    sn0_emit_li64(p, SN0_R_T0,
                  UINT64_C(0x9600000100000000) | NODE_DATA_OFF);
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_T0, SN0_R_T2, 0);
    sn0_emit_li(p, R_T3, SN0_KSEG1(R_NODE1_SLOT));
    sn0_emit_i(p, SN0_OPC_SD, R_T3, SN0_R_T2, 0);

    park = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, not_reader, p->pos);
    sn0_emit_park(p);
    sn0_patch_branch(p, park, p->pos);
    sn0_emit_park(p);
}

/*
 * Wait for the guest to reach a state, not for a period of time, using the
 * default backdoor: every explicit physical address in this file, node-
 * strided or not, is reachable that way, the way the qtest protocol's
 * first-CPU view already reaches every node's own memory.
 */
static uint32_t wait_word(QTestState *qts, uint64_t addr, unsigned int seconds)
{
    int64_t deadline = g_get_monotonic_time() + seconds * G_TIME_SPAN_SECOND;
    uint32_t value;

    do {
        value = qtest_readl(qts, addr);
        if (value) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    return value;
}

/*
 * A node-qualified physical stride, from the qtest backdoor exactly as from
 * a running CPU, is a fabric-routed request the target Hub must claim by
 * its own programmed number. There is no PROM here to have done that
 * silently, so both Hubs are programmed before anything crosses nodes.
 */
static void seed_nodes(QTestState *qts)
{
    uint8_t zero[4], one[4];

    stl_be_p(zero, 0);
    stl_be_p(one, 2);
    g_assert_true(qtest_memwrite_cpu(qts, 0, SN0_NODE_ID_PHYS,
                                     zero, sizeof(zero)));
    g_assert_true(qtest_memwrite_cpu(qts, 2, SN0_NODE_ID_PHYS,
                                     one, sizeof(one)));
    sn0_program_nasid(qts, 1);
}

static void test_second_node_cpus_execute(const void *data)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    build_execute_image(image);
    path = sn0_image_write(image, "sn0-cross-node-execute");
    qts = qtest_initf(MACHINE_ARGS " -bios %s", path);
    unlink(path);

    seed_nodes(qts);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");

    wait_word(qts, RESULT_BASE, 30);
    wait_word(qts, NUMA_NODE1_BASE + RESULT_BASE, 30);
    wait_word(qts, NUMA_NODE1_BASE + RESULT_BASE + 8, 30);

    g_assert_cmphex(qtest_readl(qts, RESULT_BASE), ==, PRIMARY_MAGIC);
    g_assert_cmphex(qtest_readl(qts, NUMA_NODE1_BASE + RESULT_BASE), ==,
                    0x00020002);
    g_assert_cmphex(qtest_readl(qts, NUMA_NODE1_BASE + RESULT_BASE + 8), ==,
                    0x00030003);

    qtest_quit(qts);
}

static void test_second_node_reads_across_fabric(const void *data)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    build_fabric_read_image(image);
    path = sn0_image_write(image, "sn0-cross-node-fabric-read");
    qts = qtest_initf(MACHINE_ARGS " -bios %s", path);
    unlink(path);

    seed_nodes(qts);

    /*
     * Plant a distinct word in each node's own copy of NODE_DATA_OFF before
     * releasing any CPU, so the reader's first load already sees them.
     */
    qtest_writeq(qts, NODE_DATA_OFF, NODE0_DATA);
    qtest_writeq(qts, NUMA_NODE1_BASE + NODE_DATA_OFF, NODE1_DATA);
    g_assert_cmphex(qtest_readq(qts, NODE_DATA_OFF), ==, NODE0_DATA);
    g_assert_cmphex(qtest_readq(qts, NUMA_NODE1_BASE + NODE_DATA_OFF), ==,
                    NODE1_DATA);

    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");

    /*
     * Each slot is a big-endian doubleword whose data lives in the low
     * word (NODE0_DATA and NODE1_DATA both fit in 32 bits); poll that
     * half, then check the whole word below. The reader is on node zero,
     * so both slots are its own node's plain memory.
     */
    wait_word(qts, R_NODE0_SLOT + 4, 30);
    wait_word(qts, R_NODE1_SLOT + 4, 30);

    /*
     * The running node-zero CPU reached node one across the fabric and its
     * own node, and the two values are distinct: the NASID selects the
     * node.
     */
    g_assert_cmphex(NODE0_DATA, !=, NODE1_DATA);
    g_assert_cmphex(qtest_readq(qts, R_NODE0_SLOT), ==, NODE0_DATA);
    g_assert_cmphex(qtest_readq(qts, R_NODE1_SLOT), ==, NODE1_DATA);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("sn0/cross-node/second-node-cpus-execute",
                        NULL, test_second_node_cpus_execute);
    qtest_add_data_func("sn0/cross-node/second-node-reads-across-fabric",
                        NULL, test_second_node_reads_across_fabric);
    return g_test_run();
}
