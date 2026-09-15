/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The stock PROM's cross-chassis rendezvous primitives on the dual-module
 * Origin 200: vector-routed PIO to a peer Hub that has no node number yet,
 * and direct NASID-qualified access to one that does.
 *
 * These re-home five of the eight bare-metal cells
 * tests/functional/mips64/test_sn0_smp.py carried under
 * synthesized-firmware=on, now that synthesized firmware -- the thing that
 * used to assign every Hub its node number before the guest code in these
 * cells ran a single instruction -- is deleted. Each cell's own MIPS
 * program is unchanged; what changed is how a CPU learns which node and
 * slice it is (SN0_NODE_ID_PHYS and its own Hub-local processor number,
 * the way sn0-cross-node-test.c already established, rather than reading
 * its own Hub's node field, which a real PROM has not assigned yet
 * either -- these cells never depended on that field being right, only on
 * every CPU behaving as its assigned role would), and that the qtest
 * harness programs both Hubs' NASIDs before releasing any CPU, the way
 * the PROM's own node-numbering phase precedes every one of these
 * exchanges. Two vector-routed cells (the port-error read and the I/O-
 * protect write) do not depend on either Hub's programmed number at all
 * -- that is the reviewed point of vector routing, reaching a neighbor
 * Hub by the route rather than its number -- but the harness still
 * programs both, since its own verification reads a few of them by
 * node-explicit physical address, which does.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sn0-user-image.h"

#define R_T3 11
#define R_T4 12
#define R_S3 19
#define SN0_OPC_SLTIU 11

#define MACHINE_ARGS \
    "-M origin200 " SN0_TWO_NODE_TOPOLOGY \
    " -cpu R10000 -accel tcg,thread=multi -S"

#define HUB_BY_NODE       UINT64_C(0x20001800000)
#define HUB_NODE_STRIDE   UINT64_C(0x100000000)

#define HUB_NI_STATUS            0x600000
#define HUB_NI_SCRATCH_0         0x600100
#define HUB_NI_SCRATCH_1         0x600108
#define HUB_NI_VECTOR_PARMS      0x600200
#define HUB_NI_VECTOR            0x600208
#define HUB_NI_VECTOR_DATA       0x600210
#define HUB_NI_VECTOR_STATUS     0x600300
#define HUB_NI_VECTOR_READ_DATA  0x600310
#define HUB_NI_PORT_ERROR        0x608008
#define HUB_NI_IO_PROTECT        0x600400
#define HUB_NI_BASE              HUB_NI_STATUS
#define REL(offset)              ((offset) - HUB_NI_BASE)

#define HUB_PI_INT_MODIFY  0x90
#define HUB_PI_INT_PEND0   0x98
#define HUB_PI_INT_PEND1   0xa0
#define HUB_PI_RT_COUNT    0x030100

#define NI_VECTOR_VALID          (UINT64_C(1) << 63)
#define NI_VECTOR_RESPONSE_MASK  0x7ULL
#define NI_VECTOR_TYPE_WRITE     1

#define PORT_ERROR_READ_DONE   0x51
#define IO_PROTECT_WRITE_DONE  0x52

/*
 * Vector routing: a zero route reaches the peer directly connected over
 * the CrayLink, by wire rather than by programmed number (Hub network-
 * interface datasheet, "Vector routing"). @target_address is the target
 * register's own offset from HUB_NI_BASE, ORed into the parameters word
 * unsigned -- it can carry bit 15 (the port-error register's offset
 * does), so it must not sign-extend the way an ADDIU load would.
 */
static void sn0_emit_vector_read(SN0Image *p, unsigned int hub_reg,
                                 unsigned int target_reg,
                                 unsigned int target_address)
{
    sn0_emit_i(p, SN0_OPC_SD, hub_reg, SN0_R_ZERO, REL(HUB_NI_VECTOR));
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, target_reg, target_address);
    sn0_emit_i(p, SN0_OPC_SD, hub_reg, target_reg, REL(HUB_NI_VECTOR_PARMS));
    sn0_emit_i(p, SN0_OPC_LD, hub_reg, target_reg,
              REL(HUB_NI_VECTOR_READ_DATA));
}

/*
 * Every cell in this file starts the same way: 64-bit kernel mode, this
 * CPU's own Hub-local NI and PI blocks in two registers.
 */
static void sn0_emit_prologue(SN0Image *p, unsigned int ni_reg,
                              unsigned int pi_reg)
{
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_li64(p, ni_reg, UINT64_C(0x9200000001000000) | HUB_NI_BASE);
    sn0_emit_li64(p, pi_reg, UINT64_C(0x9200000001000000));
}

/*
 * Global identity (0-3): the node the host seeded at SN0_NODE_ID_PHYS,
 * or-ed with this CPU's own Hub-local slice. Replaces reading the local
 * Hub's own node field, which no PROM has programmed yet in any of these
 * cells -- these are exactly the stock PROM's pre-numbering exchanges.
 */
static void sn0_emit_identity(SN0Image *p, unsigned int reg)
{
    sn0_emit_li(p, SN0_R_T1, SN0_KSEG1(SN0_NODE_ID_PHYS));
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_T1, SN0_R_T1, 0);
    sn0_emit_cpu_number(p, reg);
    sn0_emit_or(p, reg, SN0_R_T1, reg);
}

static void seed_nodes(QTestState *qts)
{
    uint8_t zero[4], two[4];

    stl_be_p(zero, 0);
    stl_be_p(two, 2);
    g_assert_true(qtest_memwrite_cpu(qts, 0, SN0_NODE_ID_PHYS,
                                     zero, sizeof(zero)));
    g_assert_true(qtest_memwrite_cpu(qts, 2, SN0_NODE_ID_PHYS,
                                     two, sizeof(two)));
    sn0_program_nasid(qts, 1);
}

static uint64_t wait_word(QTestState *qts, uint64_t addr, unsigned int seconds)
{
    int64_t deadline = g_get_monotonic_time() + seconds * G_TIME_SPAN_SECOND;
    uint64_t value;

    do {
        value = qtest_readq(qts, addr);
        if (value) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    return value;
}

/*
 * Node zero's A slice (identity 0) vector-reads the peer Hub's LLP port
 * error register, the read the stock PROM's link diagnostic makes; every
 * other CPU parks. docs/datasheets/hub-network-interface.md's reviewed
 * prescription is that the register reads zero and does not fault.
 */
static void build_peer_port_error_read(SN0Image *p)
{
    unsigned int not_actor, park;

    sn0_emit_prologue(p, SN0_R_S0, SN0_R_S1);
    sn0_emit_identity(p, SN0_R_S2);
    not_actor = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_S2, SN0_R_ZERO);

    sn0_emit_vector_read(p, SN0_R_S0, SN0_R_T0, REL(HUB_NI_PORT_ERROR));
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_ZERO, SN0_R_T1, PORT_ERROR_READ_DONE);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T1, REL(HUB_NI_SCRATCH_1));

    park = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, not_actor, p->pos);
    sn0_emit_park(p);
    sn0_patch_branch(p, park, p->pos);
    sn0_emit_park(p);
}

static void test_peer_port_error_vector_read_completes_clean(const void *data)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t status, read_data;

    build_peer_port_error_read(image);
    path = sn0_image_write(image, "sn0-smp-peer-port-error");
    qts = qtest_initf(MACHINE_ARGS " -bios %s", path);
    unlink(path);
    seed_nodes(qts);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");

    wait_word(qts, HUB_BY_NODE + HUB_NI_SCRATCH_1, 30);
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + HUB_NI_SCRATCH_1), ==,
                    PORT_ERROR_READ_DONE);

    status = qtest_readq(qts, HUB_BY_NODE + HUB_NI_VECTOR_STATUS);
    read_data = qtest_readq(qts, HUB_BY_NODE + HUB_NI_VECTOR_READ_DATA);
    g_assert_cmphex(status & (NI_VECTOR_VALID | NI_VECTOR_RESPONSE_MASK), ==,
                    NI_VECTOR_VALID);
    g_assert_cmphex(read_data, ==, 0);

    qtest_quit(qts);
}

/*
 * The link diagnostic's first vector operation, before its reads: a
 * WRITE granting itself cross-link PIO access to the peer's PIO
 * protection register. The completion must latch clean, the write
 * response type rather than an address error.
 */
static void build_peer_io_protect_write(SN0Image *p)
{
    unsigned int not_actor, park;

    sn0_emit_prologue(p, SN0_R_S0, SN0_R_S1);
    sn0_emit_identity(p, SN0_R_S2);
    not_actor = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_S2, SN0_R_ZERO);

    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_ZERO, REL(HUB_NI_VECTOR));
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_ZERO, REL(HUB_NI_VECTOR_DATA));
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_ZERO, SN0_R_T0,
              REL(HUB_NI_IO_PROTECT) | NI_VECTOR_TYPE_WRITE);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T0, REL(HUB_NI_VECTOR_PARMS));

    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_ZERO, SN0_R_T0, IO_PROTECT_WRITE_DONE);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T0, REL(HUB_NI_SCRATCH_1));

    park = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, not_actor, p->pos);
    sn0_emit_park(p);
    sn0_patch_branch(p, park, p->pos);
    sn0_emit_park(p);
}

static void
test_peer_io_protect_vector_write_completes_clean(const void *data)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t status;

    build_peer_io_protect_write(image);
    path = sn0_image_write(image, "sn0-smp-peer-io-protect");
    qts = qtest_initf(MACHINE_ARGS " -bios %s", path);
    unlink(path);
    seed_nodes(qts);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");

    wait_word(qts, HUB_BY_NODE + HUB_NI_SCRATCH_1, 30);
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + HUB_NI_SCRATCH_1), ==,
                    IO_PROTECT_WRITE_DONE);

    status = qtest_readq(qts, HUB_BY_NODE + HUB_NI_VECTOR_STATUS);
    g_assert_cmphex(status & (NI_VECTOR_VALID | NI_VECTOR_RESPONSE_MASK), ==,
                    NI_VECTOR_VALID | NI_VECTOR_TYPE_WRITE);

    qtest_quit(qts);
}

#define REMOTE_SCRATCH_DEADLINE_TICKS  12500  /* 10 ms at 1.25 MHz */
#define REMOTE_SCRATCH_IDENTITY_BASE   0x30
#define REMOTE_SCRATCH_ARMED           0x31
#define REMOTE_SCRATCH_GO              0x32
#define REMOTE_SCRATCH_DONE            0x33
#define REMOTE_SCRATCH_TIMEOUT         0x3f

/*
 * Node zero writes directly into node one's own Hub scratch register by
 * its NASID-qualified physical address -- the stock PROM's other way to
 * reach a neighbor, once that neighbor's number is known, rather than by
 * vector route -- and requires the peer to answer inside an RTC-ticks
 * deadline. Node one only polls its own local register (the one node
 * zero just wrote) and answers on its own scratch word. Both slice-B
 * CPUs park; both slice-A CPUs run this same image and take one of the
 * two roles by their own identity.
 */
static void build_remote_scratch_notification(SN0Image *p)
{
    unsigned int park_b, node1_branch, wait_armed, wait_done, complete;
    unsigned int branch;

    sn0_emit_prologue(p, SN0_R_S0, SN0_R_S1);
    sn0_emit_li64(p, SN0_R_S2, UINT64_C(0x9200000001000000) |
                              HUB_PI_RT_COUNT);
    /*
     * s3: node one's NI_SCRATCH_0 by its NASID-qualified physical
     * address (NASID 1 is physical bit 32, this file's local-window form
     * with that bit set on top, the way sn0-fabric-test.c's own renumber
     * helper reaches a specific Hub's local window).
     */
    sn0_emit_li64(p, R_S3, UINT64_C(0x9200000101000000) |
                          HUB_NI_SCRATCH_0);

    sn0_emit_identity(p, SN0_R_T0);
    sn0_emit_i(p, SN0_OPC_ANDI, SN0_R_T0, SN0_R_T1, 1);
    park_b = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T1, SN0_R_ZERO);
    sn0_emit_srl(p, SN0_R_T0, SN0_R_T0, 1);      /* t0 = own node, 0 or 1 */

    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_T0, SN0_R_T1,
              REMOTE_SCRATCH_IDENTITY_BASE);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T1, REL(HUB_NI_SCRATCH_0));
    node1_branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_ZERO);

    /* Node zero: wait for the peer's ARMED, then GO with a deadline. */
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T1, REMOTE_SCRATCH_ARMED);
    wait_armed = p->pos;
    sn0_emit_vector_read(p, SN0_R_S0, SN0_R_T0, REL(HUB_NI_SCRATCH_1));
    branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_T1);
    sn0_patch_branch(p, branch, wait_armed);

    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S2, SN0_R_T2, 0);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_T2, SN0_R_T2,
              REMOTE_SCRATCH_DEADLINE_TICKS);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T1, REMOTE_SCRATCH_GO);
    sn0_emit_i(p, SN0_OPC_SD, R_S3, SN0_R_T1, 0);
    sn0_emit(p, 0x0000000f);                      /* sync */
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T1, REMOTE_SCRATCH_DONE);

    wait_done = p->pos;
    sn0_emit_vector_read(p, SN0_R_S0, SN0_R_T0, REL(HUB_NI_SCRATCH_1));
    complete = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_T0, SN0_R_T1);
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S2, R_T3, 0);
    sn0_emit(p, (R_T3 << 21) | (SN0_R_T2 << 16) | (R_T4 << 11) | 0x2b);
    branch = sn0_emit_branch(p, SN0_OPC_BNE, R_T4, SN0_R_ZERO);
    sn0_patch_branch(p, branch, wait_done);

    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T1, REMOTE_SCRATCH_TIMEOUT);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T1, REL(HUB_NI_SCRATCH_1));
    sn0_emit_park(p);

    sn0_patch_branch(p, complete, p->pos);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T1, REL(HUB_NI_SCRATCH_1));
    sn0_emit_park(p);

    /*
     * Node one: publish ARMED, wait for node zero's direct write of GO,
     * publish DONE.
     */
    sn0_patch_branch(p, node1_branch, p->pos);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T1, REMOTE_SCRATCH_ARMED);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T1, REL(HUB_NI_SCRATCH_1));
    sn0_emit(p, 0x0000000f);                      /* sync */
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T1, REMOTE_SCRATCH_GO);
    {
        unsigned int wait_go = p->pos;

        sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T0, REL(HUB_NI_SCRATCH_0));
        branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_T1);
        sn0_patch_branch(p, branch, wait_go);
    }
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T1, REMOTE_SCRATCH_DONE);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T1, REL(HUB_NI_SCRATCH_1));
    sn0_emit_park(p);

    sn0_patch_branch(p, park_b, p->pos);
    sn0_emit_park(p);
}

static void test_remote_hub_scratch_write_schedules_peer(const void *data)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t hub0, hub1;

    build_remote_scratch_notification(image);
    path = sn0_image_write(image, "sn0-smp-remote-scratch");
    qts = qtest_initf(MACHINE_ARGS " -bios %s", path);
    unlink(path);
    seed_nodes(qts);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");

    hub0 = HUB_BY_NODE;
    hub1 = HUB_BY_NODE + HUB_NODE_STRIDE;

    wait_word(qts, hub0 + HUB_NI_SCRATCH_1, 30);
    g_assert_cmphex(qtest_readq(qts, hub0 + HUB_NI_SCRATCH_1), ==,
                    REMOTE_SCRATCH_DONE);

    g_assert_cmphex(qtest_readq(qts, hub0 + HUB_NI_SCRATCH_0), ==,
                    REMOTE_SCRATCH_IDENTITY_BASE);
    g_assert_cmphex(qtest_readq(qts, hub1 + HUB_NI_SCRATCH_0), ==,
                    REMOTE_SCRATCH_GO);
    g_assert_cmphex(qtest_readq(qts, hub1 + HUB_NI_SCRATCH_1), ==,
                    REMOTE_SCRATCH_DONE);

    qtest_quit(qts);
}

#define PEER_SCRATCH_DEADLINE_TICKS  12500  /* 10 ms at 1.25 MHz */
#define PEER_SCRATCH_IDENTITY_BASE   0x10
#define PEER_SCRATCH_ARMED           0x21
#define PEER_SCRATCH_GO              0x22
#define PEER_SCRATCH_DONE            0x23
#define PEER_SCRATCH_TIMEOUT         0x2f
#define PEER_SCRATCH_INTERRUPT       114
#define PEER_SCRATCH_PEND1 \
    (UINT64_C(1) << (PEER_SCRATCH_INTERRUPT - 64))
#define PI_INT_MODIFY_SET            0x100

/*
 * Both nodes' A slices publish an identity and ARMED, entirely by vector
 * route -- no NASID needed, the way the stock PROM's own pre-numbering
 * handoff works. Node zero then waits for the peer's ARMED, starts an
 * RTC-ticks deadline, and answers GO; node one answers GO in turn, then
 * raises interrupt 114 on its own PI block as its last, device-side
 * action. Node zero must see DONE inside the deadline.
 */
static void build_peer_scratch_deadline(SN0Image *p)
{
    unsigned int park_b, node1_branch, wait_armed, wait_done, timed_out;
    unsigned int wait_go, wait_peer_done, branch;

    sn0_emit_prologue(p, SN0_R_S0, SN0_R_S1);
    sn0_emit_li64(p, SN0_R_S2, UINT64_C(0x9200000001000000) |
                              HUB_PI_RT_COUNT);

    sn0_emit_identity(p, SN0_R_T0);
    sn0_emit_i(p, SN0_OPC_ANDI, SN0_R_T0, SN0_R_T1, 1);
    park_b = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T1, SN0_R_ZERO);
    sn0_emit_srl(p, SN0_R_T0, SN0_R_T0, 1);       /* t0 = own node, 0 or 1 */

    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_T0, SN0_R_T1, PEER_SCRATCH_IDENTITY_BASE);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T1, REL(HUB_NI_SCRATCH_0));
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T1, PEER_SCRATCH_ARMED);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T1, REL(HUB_NI_SCRATCH_1));
    sn0_emit(p, 0x0000000f);                       /* sync */
    node1_branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_ZERO);

    /* Node zero: wait for the peer's ARMED, then run the timed handoff. */
    wait_armed = p->pos;
    sn0_emit_vector_read(p, SN0_R_S0, SN0_R_T0, REL(HUB_NI_SCRATCH_1));
    branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_T1);
    sn0_patch_branch(p, branch, wait_armed);

    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S2, SN0_R_T2, 0);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_T2, SN0_R_T2,
              PEER_SCRATCH_DEADLINE_TICKS);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T1, PEER_SCRATCH_GO);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T1, REL(HUB_NI_SCRATCH_1));
    sn0_emit(p, 0x0000000f);                       /* sync */
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T1, PEER_SCRATCH_DONE);

    wait_done = p->pos;
    sn0_emit_vector_read(p, SN0_R_S0, SN0_R_T0, REL(HUB_NI_SCRATCH_1));
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S2, R_T3, 0);
    sn0_emit(p, (R_T3 << 21) | (SN0_R_T2 << 16) | (R_T4 << 11) | 0x2b);
    timed_out = sn0_emit_branch(p, SN0_OPC_BEQ, R_T4, SN0_R_ZERO);
    branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_T1);
    sn0_patch_branch(p, branch, wait_done);

    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T1, REL(HUB_NI_SCRATCH_1));
    sn0_emit_park(p);

    sn0_patch_branch(p, timed_out, p->pos);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T1, PEER_SCRATCH_TIMEOUT);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T1, REL(HUB_NI_SCRATCH_1));
    sn0_emit_park(p);

    /*
     * Node one: answer GO, wait for the coordinator's DONE, then raise
     * interrupt 114 on its own PI block as its last action.
     */
    sn0_patch_branch(p, node1_branch, p->pos);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T1, PEER_SCRATCH_GO);
    wait_go = p->pos;
    sn0_emit_vector_read(p, SN0_R_S0, SN0_R_T0, REL(HUB_NI_SCRATCH_1));
    branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_T1);
    sn0_patch_branch(p, branch, wait_go);

    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T1, PEER_SCRATCH_DONE);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T1, REL(HUB_NI_SCRATCH_1));
    sn0_emit(p, 0x0000000f);                       /* sync */
    wait_peer_done = p->pos;
    sn0_emit_vector_read(p, SN0_R_S0, SN0_R_T0, REL(HUB_NI_SCRATCH_1));
    branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_T1);
    sn0_patch_branch(p, branch, wait_peer_done);

    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T0,
              PI_INT_MODIFY_SET | PEER_SCRATCH_INTERRUPT);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S1, SN0_R_T0, HUB_PI_INT_MODIFY);
    sn0_emit_park(p);

    sn0_patch_branch(p, park_b, p->pos);
    sn0_emit_park(p);
}

static void run_peer_scratch_deadline(bool thread_multi)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t hub0, hub1;
    uint64_t scratch0[2], scratch1[2];
    unsigned int node;

    build_peer_scratch_deadline(image);
    path = sn0_image_write(image, "sn0-smp-peer-scratch");
    if (thread_multi) {
        qts = qtest_initf(
            "-M origin200 " SN0_TWO_NODE_TOPOLOGY
            " -cpu R10000 -accel tcg,thread=multi -S -bios %s", path);
    } else {
        qts = qtest_initf(
            "-M origin200 " SN0_TWO_NODE_TOPOLOGY
            " -cpu R10000 "
            "-accel tcg,thread=single,rr-kick-period-ns=100000 "
            "-S -bios %s", path);
    }
    unlink(path);
    seed_nodes(qts);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");

    hub0 = HUB_BY_NODE;
    hub1 = HUB_BY_NODE + HUB_NODE_STRIDE;

    /*
     * Node one raises interrupt 114 only after it has seen the
     * coordinator's DONE; the coordinator's TIMEOUT is the only other
     * terminal state. Wait for either.
     */
    for (;;) {
        uint64_t pend1 = qtest_readq(qts, hub1 + HUB_PI_INT_PEND1);
        uint64_t done = qtest_readq(qts, hub0 + HUB_NI_SCRATCH_1);

        if ((pend1 & PEER_SCRATCH_PEND1) || done == PEER_SCRATCH_TIMEOUT) {
            break;
        }
        g_usleep(1000);
    }

    for (node = 0; node < 2; node++) {
        uint64_t hub = HUB_BY_NODE + (uint64_t)node * HUB_NODE_STRIDE;

        scratch0[node] = qtest_readq(qts, hub + HUB_NI_SCRATCH_0);
        scratch1[node] = qtest_readq(qts, hub + HUB_NI_SCRATCH_1);
    }

    for (node = 0; node < 2; node++) {
        g_assert_cmphex(scratch0[node], ==, PEER_SCRATCH_IDENTITY_BASE + node);
        g_assert_cmphex(scratch1[node], ==, PEER_SCRATCH_DONE);
    }
    g_assert_cmphex(qtest_readq(qts, hub0 + HUB_PI_INT_PEND1), ==, 0);
    g_assert_cmphex(qtest_readq(qts, hub1 + HUB_PI_INT_PEND1), ==,
                    PEER_SCRATCH_PEND1);

    qtest_quit(qts);
}

static void test_peer_scratch_handoff_before_rtc_deadline(const void *data)
{
    run_peer_scratch_deadline(false);
}

static void test_peer_scratch_handoff_before_rtc_deadline_thread_multi(
    const void *data)
{
    run_peer_scratch_deadline(true);
}

#define HUB_PI_INT_MASK0_A  0xa8
#define HUB_PI_INT_MASK1_A  0xb0
#define HUB_PI_INT_MASK0_B  0xb8
#define HUB_PI_INT_MASK1_B  0xc0

#define PEND_READ_STATE        UINT64_C(0x100033000)  /* node one, uncached */
#define PEND_READ_ROUND        0x00
#define PEND_READ_WINDOW_B     0x08
#define PEND_READ_WINDOW_A     0x10
#define PEND_READ_SET_A        0x18
#define PEND_READ_DONE_B       0x20
#define PEND_READ_DONE_A       0x28
#define PEND_READ_TIMEOUT      0x30
#define PEND_READ_FAIR         0x58  /* 4 words */
#define PEND_READ_ROUNDS       8
#define PEND_READ_WAIT_LIMIT   64
#define PEND_READ_WAIT_LIMIT_UNBOUNDED 0xffffffffu
#define PEND_READ_INTERRUPT        114  /* bank 1, bit 50 */
#define PEND_READ_UNRELATED_BANK1  112
#define PEND_READ_UNRELATED_BANK0  12
#define PEND_READ_PEND1_BIT    (UINT64_C(1) << (PEND_READ_INTERRUPT - 64))
#define PEND_READ_MASK1_B_VALUE \
    (PEND_READ_PEND1_BIT | (UINT64_C(1) << (PEND_READ_UNRELATED_BANK1 - 64)))

static void sn0_emit_dsrl32(SN0Image *p, unsigned int rd, unsigned int rt,
                            unsigned int sa)
{
    sn0_emit(p, (rt << 16) | (rd << 11) | (sa << 6) | 0x3e);
}

static void sn0_emit_dsll32(SN0Image *p, unsigned int rd, unsigned int rt,
                            unsigned int sa)
{
    sn0_emit(p, (rt << 16) | (rd << 11) | (sa << 6) | 0x3c);
}

/*
 * Leaves 1 in t0 when this CPU's local PI_INT_PEND1 carries the
 * rendezvous interrupt (bank 1, bit 50), 0 otherwise.
 */
static void sn0_emit_pend1_bit_test(SN0Image *p, unsigned int pi_reg)
{
    sn0_emit_i(p, SN0_OPC_LD, pi_reg, SN0_R_T0, HUB_PI_INT_PEND1);
    sn0_emit_dsrl32(p, SN0_R_T0, SN0_R_T0,
                    PEND_READ_INTERRUPT - 64 - 32);
    sn0_emit_i(p, SN0_OPC_ANDI, SN0_R_T0, SN0_R_T0, 1);
}

static void sn0_emit_wait_nonzero(SN0Image *p, unsigned int base_reg,
                                  unsigned int offset)
{
    unsigned int wait = p->pos;
    unsigned int branch;

    sn0_emit_i(p, SN0_OPC_LD, base_reg, SN0_R_T0, offset);
    branch = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_T0, SN0_R_ZERO);
    sn0_patch_branch(p, branch, wait);
}

/*
 * The stock PROM's slice-pair rendezvous, as its four-CPU/two-node boot
 * actually runs it: bank 1's interrupt 114 set by slice A and cleared by
 * slice B through PI_INT_MODIFY, each side bounded-polling PI_INT_PEND1
 * for the other's edge, with further bank-1 bits pending across every
 * poll. Node one's pair does not participate; its CPUs prove that the
 * rendezvous does not starve them across repeated rounds. All local Hub
 * traffic, so no NASID needed for the guest code; the harness still
 * programs both, since it checks node one's Hub by node-explicit address.
 */
static void build_pending_read_rendezvous(SN0Image *p, uint32_t wait_limit)
{
    unsigned int select_a, select_b, observe, parked, observer_park;
    unsigned int a_round, a_poll, cleared, a_timeout, a_park;
    unsigned int b_round, b_poll, set_seen, b_timeout;
    unsigned int b_wait_set_a, b_wait_done_a, b_park;
    unsigned int branch;

    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);

    sn0_emit_li64(p, SN0_R_S0, PEND_READ_STATE | UINT64_C(0x9000000000000000));
    sn0_emit_li64(p, R_S3, UINT64_C(0x9200000001000000));  /* local PI */
    sn0_emit_identity(p, SN0_R_S1);                 /* s1 = global index */
    sn0_emit_sll(p, SN0_R_S2, SN0_R_S1, 3);          /* s2 = index * 8 */

    select_a = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_S1, SN0_R_ZERO);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T0, 1);
    select_b = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_S1, SN0_R_T0);

    /* Node one's pair: observe the round counter and count fairness. */
    sn0_emit(p, (SN0_R_S0 << 21) | (SN0_R_S2 << 16) | (R_S3 << 11) |
             0x2d);                                  /* daddu s3, s0, s2 */
    sn0_emit_wait_nonzero(p, SN0_R_S0, PEND_READ_ROUND);
    observe = p->pos;
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T0, PEND_READ_DONE_A);
    sn0_emit_i(p, SN0_OPC_SLTIU, SN0_R_T0, SN0_R_T1, PEND_READ_ROUNDS);
    parked = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_T1, SN0_R_ZERO);
    sn0_emit_i(p, SN0_OPC_LD, R_S3, SN0_R_T0, PEND_READ_FAIR);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_T0, SN0_R_T0, 1);
    sn0_emit_i(p, SN0_OPC_SD, R_S3, SN0_R_T0, PEND_READ_FAIR);
    branch = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, branch, observe);
    sn0_patch_branch(p, parked, p->pos);
    observer_park = p->pos;
    branch = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, branch, observer_park);

    /*
     * Slice A: publish the interrupt each round, then bounded-poll for
     * B's acknowledging clear.
     */
    sn0_patch_branch(p, select_a, p->pos);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_S2, 1);   /* s2 = round */
    a_round = p->pos;
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T0, PEND_READ_ROUND);
    branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_S2);
    sn0_patch_branch(p, branch, a_round);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T0,
              PI_INT_MODIFY_SET | PEND_READ_INTERRUPT);
    sn0_emit_i(p, SN0_OPC_SD, R_S3, SN0_R_T0, HUB_PI_INT_MODIFY);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T0, 1);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T0, PEND_READ_WINDOW_A);
    sn0_emit(p, 0x0000000f);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_S2, PEND_READ_SET_A);
    sn0_emit(p, 0x0000000f);
    sn0_emit_li(p, SN0_R_T2, wait_limit);
    a_poll = p->pos;
    sn0_emit_pend1_bit_test(p, R_S3);
    cleared = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_T0, SN0_R_ZERO);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_T2, SN0_R_T2, (uint16_t)-1);
    branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T2, SN0_R_ZERO);
    sn0_patch_branch(p, branch, a_poll);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T0, 2);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T0, PEND_READ_TIMEOUT);
    a_timeout = p->pos;
    branch = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, branch, a_timeout);
    sn0_patch_branch(p, cleared, p->pos);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_ZERO, PEND_READ_WINDOW_A);
    sn0_emit(p, 0x0000000f);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_S2, PEND_READ_DONE_A);
    sn0_emit(p, 0x0000000f);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_S2, SN0_R_S2, 1);
    sn0_emit_i(p, SN0_OPC_SLTIU, SN0_R_S2, SN0_R_T0, PEND_READ_ROUNDS + 1);
    branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_ZERO);
    sn0_patch_branch(p, branch, a_round);
    a_park = p->pos;
    branch = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, branch, a_park);

    /*
     * Slice B: owns the mask and the acknowledgement. Arms each round,
     * bounded-polls for A's set, clears only once A's poll is announced.
     */
    sn0_patch_branch(p, select_b, p->pos);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T0,
              PEND_READ_MASK1_B_VALUE >> 48);
    sn0_emit_dsll32(p, SN0_R_T0, SN0_R_T0, 16);
    sn0_emit_i(p, SN0_OPC_SD, R_S3, SN0_R_T0, HUB_PI_INT_MASK1_B);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T0,
              PI_INT_MODIFY_SET | PEND_READ_UNRELATED_BANK1);
    sn0_emit_i(p, SN0_OPC_SD, R_S3, SN0_R_T0, HUB_PI_INT_MODIFY);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T0,
              PI_INT_MODIFY_SET | PEND_READ_UNRELATED_BANK0);
    sn0_emit_i(p, SN0_OPC_SD, R_S3, SN0_R_T0, HUB_PI_INT_MODIFY);

    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_S2, 1);
    b_round = p->pos;
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_S2, PEND_READ_ROUND);
    sn0_emit(p, 0x0000000f);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T0, 1);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T0, PEND_READ_WINDOW_B);
    sn0_emit(p, 0x0000000f);
    sn0_emit_li(p, SN0_R_T2, wait_limit);
    b_poll = p->pos;
    sn0_emit_pend1_bit_test(p, R_S3);
    set_seen = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_ZERO);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_T2, SN0_R_T2, (uint16_t)-1);
    branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T2, SN0_R_ZERO);
    sn0_patch_branch(p, branch, b_poll);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T0, 1);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_T0, PEND_READ_TIMEOUT);
    b_timeout = p->pos;
    branch = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, branch, b_timeout);
    sn0_patch_branch(p, set_seen, p->pos);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_ZERO, PEND_READ_WINDOW_B);
    sn0_emit(p, 0x0000000f);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_S2, PEND_READ_DONE_B);
    sn0_emit(p, 0x0000000f);
    b_wait_set_a = p->pos;
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T0, PEND_READ_SET_A);
    branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_S2);
    sn0_patch_branch(p, branch, b_wait_set_a);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, SN0_R_T0, PEND_READ_INTERRUPT);
    sn0_emit_i(p, SN0_OPC_SD, R_S3, SN0_R_T0, HUB_PI_INT_MODIFY);
    b_wait_done_a = p->pos;
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T0, PEND_READ_DONE_A);
    branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_S2);
    sn0_patch_branch(p, branch, b_wait_done_a);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_S2, SN0_R_S2, 1);
    sn0_emit_i(p, SN0_OPC_SLTIU, SN0_R_S2, SN0_R_T0, PEND_READ_ROUNDS + 1);
    branch = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T0, SN0_R_ZERO);
    sn0_patch_branch(p, branch, b_round);
    b_park = p->pos;
    branch = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, branch, b_park);
}

static void run_pending_read_rendezvous(uint32_t wait_limit, bool thread_multi)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t hub0, hub1;
    uint64_t done_a, timeout, fair[4];
    unsigned int reg;

    build_pending_read_rendezvous(image, wait_limit);
    path = sn0_image_write(image, "sn0-smp-pending-read");
    if (thread_multi) {
        qts = qtest_initf(
            "-M origin200 " SN0_TWO_NODE_TOPOLOGY
            " -cpu R10000 -accel tcg,thread=multi -S -bios %s", path);
    } else {
        qts = qtest_initf(
            "-M origin200 " SN0_TWO_NODE_TOPOLOGY
            " -cpu R10000 "
            "-accel tcg,thread=single,rr-kick-period-ns=10000000 "
            "-S -bios %s", path);
    }
    unlink(path);
    seed_nodes(qts);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");

    hub0 = HUB_BY_NODE;
    hub1 = HUB_BY_NODE + HUB_NODE_STRIDE;

    for (;;) {
        done_a = qtest_readq(qts, PEND_READ_STATE + PEND_READ_DONE_A);
        timeout = qtest_readq(qts, PEND_READ_STATE + PEND_READ_TIMEOUT);
        if (done_a == PEND_READ_ROUNDS || timeout) {
            break;
        }
        g_usleep(1000);
    }

    g_assert_cmphex(timeout, ==, 0);
    g_assert_cmphex(done_a, ==, PEND_READ_ROUNDS);
    for (reg = 0; reg < 4; reg++) {
        fair[reg] = qtest_readq(qts, PEND_READ_STATE + PEND_READ_FAIR +
                                8 * reg);
    }
    g_assert_cmphex(fair[0], ==, 0);
    g_assert_cmphex(fair[1], ==, 0);
    g_assert_cmpuint(fair[2], >, 0);
    g_assert_cmpuint(fair[3], >, 0);

    g_assert_cmphex(qtest_readq(qts, hub0 + HUB_PI_INT_PEND0), ==,
                    UINT64_C(1) << PEND_READ_UNRELATED_BANK0);
    g_assert_cmphex(qtest_readq(qts, hub0 + HUB_PI_INT_PEND1), ==,
                    UINT64_C(1) << (PEND_READ_UNRELATED_BANK1 - 64));
    g_assert_cmphex(qtest_readq(qts, hub0 + HUB_PI_INT_MASK0_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, hub0 + HUB_PI_INT_MASK1_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, hub0 + HUB_PI_INT_MASK0_B), ==, 0);
    g_assert_cmphex(qtest_readq(qts, hub0 + HUB_PI_INT_MASK1_B), ==,
                    PEND_READ_MASK1_B_VALUE);
    g_assert_cmphex(qtest_readq(qts, hub1 + HUB_PI_INT_PEND0), ==, 0);
    g_assert_cmphex(qtest_readq(qts, hub1 + HUB_PI_INT_PEND1), ==, 0);
    g_assert_cmphex(qtest_readq(qts, hub1 + HUB_PI_INT_MASK0_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, hub1 + HUB_PI_INT_MASK1_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, hub1 + HUB_PI_INT_MASK0_B), ==, 0);
    g_assert_cmphex(qtest_readq(qts, hub1 + HUB_PI_INT_MASK1_B), ==, 0);

    qtest_quit(qts);
}

static void test_pending_read_schedules_local_peer(const void *data)
{
    run_pending_read_rendezvous(PEND_READ_WAIT_LIMIT, false);
}

static void test_pending_read_rendezvous_thread_multi(const void *data)
{
    run_pending_read_rendezvous(PEND_READ_WAIT_LIMIT_UNBOUNDED, true);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!qtest_has_accel("tcg")) {
        return 0;
    }
    qtest_add_data_func(
        "sn0/smp-rendezvous/peer-port-error-vector-read-completes-clean",
        NULL, test_peer_port_error_vector_read_completes_clean);
    qtest_add_data_func(
        "sn0/smp-rendezvous/peer-io-protect-vector-write-completes-clean",
        NULL, test_peer_io_protect_vector_write_completes_clean);
    qtest_add_data_func(
        "sn0/smp-rendezvous/remote-hub-scratch-write-schedules-peer",
        NULL, test_remote_hub_scratch_write_schedules_peer);
    qtest_add_data_func(
        "sn0/smp-rendezvous/peer-scratch-handoff-before-rtc-deadline",
        NULL, test_peer_scratch_handoff_before_rtc_deadline);
    qtest_add_data_func(
        "sn0/smp-rendezvous/peer-scratch-handoff-before-rtc-deadline-"
        "thread-multi",
        NULL, test_peer_scratch_handoff_before_rtc_deadline_thread_multi);
    qtest_add_data_func(
        "sn0/smp-rendezvous/pending-read-schedules-local-peer",
        NULL, test_pending_read_schedules_local_peer);
    qtest_add_data_func(
        "sn0/smp-rendezvous/pending-read-rendezvous-thread-multi",
        NULL, test_pending_read_rendezvous_thread_multi);
    return g_test_run();
}
