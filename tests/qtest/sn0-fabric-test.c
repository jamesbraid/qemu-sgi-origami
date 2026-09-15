/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The fabric interface on the dual-module Origin 200: every class of node-qualified
 * traffic resolves by the programmed node number of a reachable Hub, through
 * the machine-owned fabric, and nothing resolves by board position.
 *
 * The two-node Origin 200 is two Hubs joined by one CrayLink cable, so its
 * fabric is the direct adapter: each Hub reaches itself by its own number and
 * the other Hub by that Hub's programmed number, and nothing else. The tests
 * renumber node 1 to a number that is no board position (7) to tell a map keyed
 * by programmed number from the board-keyed aliases the fork had before this
 * change, which could reach a peer only at its board stride.
 *
 * The qtest backdoor is processor 0's view of the machine, so it too reaches
 * a node by that node's programmed number: once node 1 is renumbered to 7,
 * its memory, its Hub, and its Bridge sit at stride 7 for every access here.
 *
 * Contract: the routed-fabric design and its test contract in the Origami
 * project tree (docs/designs/2026-09-03-origin2000-routed-fabric-design.md and
 * docs/plans/origin2000-fabric-test-contract.md); the address-map datasheet's
 * programmed-node-number rule; the BTE datasheet's rule that its addresses
 * are physical, node field included.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sn0-user-image.h"

/* This file's own copies, the way every sn0-*-test.c stands alone. */
#define SPACE_XTALK         0x20000000000ULL
#define NODE_STRIDE         0x100000000ULL
#define HUB_LOCAL           (SPACE_XTALK + 0x01000000)
#define HUB_BY_NODE         (SPACE_XTALK + 0x01800000)

#define NI_STATUS           0x600000
#define NI_SCRATCH_0        0x600100
#define NVS_VALID           (1ULL << 63)
#define NI_VECTOR_PARMS     0x600200
#define NI_VECTOR           0x600208
#define NI_VECTOR_DATA      0x600210
#define NI_VECTOR_STATUS    0x600300
#define NI_VECTOR_READ_DATA 0x600310
#define NI_VECTOR_CLEAR     0x600380
#define NI_REGION_SIZE      (1ULL << 17)
#define NI_NODE_MASK        (0x1ffULL << 8)
#define NI_NODE(n)          ((uint64_t)(n) << 8)

#define NVS_TYPE_MASK       0x7ULL
#define PIOTYPE_READ        0
#define PIOTYPE_WRITE       1

#define PI_CALIAS_SIZE      0x000028
#define PI_CALIAS_SIZE_4K   1
#define PI_INT_PEND0        0x000098
#define NI_STATUS_LINK_UP   (1ULL << 29)

#define BTE_BASE(e)         (0x410000 + (e) * 0x10000)
#define BTE_LENGTH(e)       (BTE_BASE(e) + 0x00)
#define BTE_SOURCE(e)       (BTE_BASE(e) + 0x08)
#define BTE_DEST(e)         (BTE_BASE(e) + 0x10)
#define BTE_CONTROL(e)      (BTE_BASE(e) + 0x18)
#define BTE_NOTIFY(e)       (BTE_BASE(e) + 0x20)
#define BTE_INTERRUPT(e)    (BTE_BASE(e) + 0x28)
#define BTE_BUSY            (1ULL << 20)
#define BTE_ERROR           (1ULL << 16)
#define BTE_LINE            128
#define BTE_CTL_NOTIFY      (1ULL << 4)
#define BTE_INT_LEVEL(l)    ((uint64_t)(l) << 16)

/* The Bridge at widget 8 of each node, its registers four bytes into a slot. */
#define BRIDGE_WIDGET       8
#define BRIDGE_BASE         (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))
#define BRIDGE_AT(node)     (BRIDGE_BASE + (uint64_t)(node) * NODE_STRIDE)
#define REG_DATA            4
#define REG(node, offset)   (BRIDGE_AT(node) + (offset) + REG_DATA)
#define BRIDGE_DIRECT_MAP   0x080
#define BRIDGE_INT_ENABLE   0x108
#define BRIDGE_INT_HOST(n)  (0x130 + 8 * (n))
#define BRIDGE_DEVICE(n)    (0x200 + 8 * (n))
#define DEV_SWAP_DIRECT     (1u << 19)
#define DEV_MEMORY_SPACE    (1u << 12)
#define BRIDGE_CFG0         0x020000
#define CFG0(node, device)  (BRIDGE_AT(node) + BRIDGE_CFG0 + (device) * 0x1000)
#define PCI_COMMAND         0x04
#define PCI_COMMAND_MEMORY  0x0002
#define PCI_COMMAND_MASTER  0x0004
#define PCI_BAR0            0x10
#define CARD_MMIO(slot)     (0x10000000u + (slot) * 0x1000000u)

static const uint64_t window_offset[8] = {
    0x200000, 0x400000, 0x600000, 0x700000,
    0x800000, 0x900000, 0xa00000, 0xb00000,
};
#define WINDOW(node, slot)  (BRIDGE_AT(node) + window_offset[slot])

/* The bus-mastering test card's registers (hw/misc/sgi-bridge-dma-test.c). */
#define DMA_REG_ADDRESS     0x00
#define DMA_REG_LENGTH      0x04
#define DMA_REG_SEED        0x08
#define DMA_REG_GO          0x0c
#define DMA_REG_INTERRUPT   0x10
#define DMA_REG_ADDRESS_HI  0x18

/* Node 1's processor slice A: processors 0-1 are node 0's. */
#define NODE1_CPU           2

/* A number that is no board position on a two-node machine. */
#define FAR_NUMBER          7

#define MEM_PROBE           0x00800000ULL
#define MEM_PLANT_NODE0     0x1111222233334444ULL
#define MEM_PLANT_NODE1     0x5555666677778888ULL

#define MACHINE_DUAL \
    "-M origin200 -m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"

/*
 * A read through one processor's own view. The qtest protocol dispatches
 * through the first CPU, so it cannot see another processor's view; the
 * monitor's examine command, given the processor in the same request, can.
 * A failed completion surfaces as "Cannot access memory", the point of
 * several assertions here.
 */
static bool cpu_view_readq(QTestState *qts, unsigned int cpu, uint64_t phys,
                           uint64_t *value)
{
    g_autofree char *command = g_strdup_printf("xp/1gx 0x%" PRIx64, phys);
    g_autoptr(QDict) response = NULL;
    const char *dumped;
    const char *hex;

    response = qtest_qmp(qts,
                         "{ 'execute': 'human-monitor-command',"
                         "  'arguments': { 'command-line': %s,"
                         "                 'cpu-index': %u } }",
                         command, cpu);
    g_assert(qdict_haskey(response, "return"));
    dumped = qdict_get_str(response, "return");
    if (strstr(dumped, "Cannot access memory")) {
        return false;
    }
    hex = strchr(dumped, ':');
    g_assert_nonnull(hex);
    g_assert_cmpint(sscanf(hex + 1, " 0x%" SCNx64, value), ==, 1);
    return true;
}

static uint64_t cpu_view_word64(QTestState *qts, unsigned int cpu,
                                uint64_t phys)
{
    uint64_t value = 0;

    g_assert_true(cpu_view_readq(qts, cpu, phys, &value));
    return value;
}

/*
 * Program a Hub's node number through its local window at the number it
 * holds now, the way its own processor would, and read it back at the
 * number it holds afterwards.
 */
static void renumber(QTestState *qts, unsigned int from, unsigned int to)
{
    qtest_writeq(qts, (uint64_t)from * NODE_STRIDE + HUB_LOCAL + NI_STATUS,
                 NI_REGION_SIZE | NI_NODE(to));
    g_assert_cmphex(qtest_readq(qts, (uint64_t)to * NODE_STRIDE +
                                HUB_LOCAL + NI_STATUS) & NI_NODE_MASK,
                    ==, NI_NODE(to));
}

/* Write the fabric's down-links property through the monitor. */
static void fabric_set_down_links(QTestState *qts, const char *value)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'qom-set',"
        "  'arguments': { 'path': '/machine/fabric',"
        "                 'property': 'down-links', 'value': %s } }", value);
    g_assert(qdict_haskey(response, "return"));
}

static uint64_t vector_operation(QTestState *qts, uint64_t hub,
                                 uint64_t address, uint64_t type)
{
    qtest_writeq(qts, hub + NI_VECTOR, 0);
    qtest_writeq(qts, hub + NI_VECTOR_PARMS, address | type);
    return qtest_readq(qts, hub + NI_VECTOR_STATUS);
}

static void memset_line(QTestState *qts, uint64_t where, uint8_t fill)
{
    qtest_memset(qts, where, fill, BTE_LINE);
}

static void assert_line(QTestState *qts, uint64_t where, uint8_t fill)
{
    uint8_t got[BTE_LINE];
    unsigned int i;

    qtest_bufread(qts, where, got, sizeof(got));
    for (i = 0; i < sizeof(got); i++) {
        g_assert_cmphex(got[i], ==, fill);
    }
}

/*
 * The direct adapter's map is own number plus the peer's programmed number,
 * wherever those numbers are. A peer renumbered off its board position is
 * reachable at its new number and at nothing else; today's board-keyed
 * aliases leave it unreachable, which is the second declared Origin 200
 * change.
 */
static void test_direct_adapter_map_is_own_plus_peer(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_DUAL);
    uint64_t value;
    sn0_program_nasid(qts, 1);

    qtest_writeq(qts, MEM_PROBE, MEM_PLANT_NODE0);
    qtest_writeq(qts, NODE_STRIDE + MEM_PROBE, MEM_PLANT_NODE1);

    renumber(qts, 1, FAR_NUMBER);

    /* Node 0's processor reaches node 1 by the new number ... */
    g_assert_cmphex(cpu_view_word64(qts, 0, FAR_NUMBER * NODE_STRIDE +
                                    MEM_PROBE), ==, MEM_PLANT_NODE1);
    /* ... and at neither its old number nor a number nobody holds. */
    g_assert_false(cpu_view_readq(qts, 0, 1 * NODE_STRIDE + MEM_PROBE,
                                  &value));
    g_assert_false(cpu_view_readq(qts, 0, 2 * NODE_STRIDE + MEM_PROBE,
                                  &value));
    /* Node 1's own processor reaches its own memory by its own number. */
    g_assert_cmphex(cpu_view_word64(qts, NODE1_CPU, FAR_NUMBER * NODE_STRIDE +
                                    MEM_PROBE), ==, MEM_PLANT_NODE1);
    /*
     * Node 0 is still node 0 from both sides. A node-field-zero read from
     * node 1's processor stays local inside the reset-size cached-alias
     * window by contract, so shrink that window to 4 KiB first; beyond it
     * the read falls to the claim layer, which routes number zero.
     */
    g_assert_cmphex(cpu_view_word64(qts, 0, MEM_PROBE), ==, MEM_PLANT_NODE0);
    qtest_writeq(qts, FAR_NUMBER * NODE_STRIDE + HUB_LOCAL + PI_CALIAS_SIZE,
                 PI_CALIAS_SIZE_4K);
    g_assert_cmphex(cpu_view_word64(qts, NODE1_CPU, MEM_PROBE), ==,
                    MEM_PLANT_NODE0);

    /* Back to the board number, the old stride answers again. */
    renumber(qts, FAR_NUMBER, 1);
    g_assert_cmphex(cpu_view_word64(qts, 0, 1 * NODE_STRIDE + MEM_PROBE), ==,
                    MEM_PLANT_NODE1);
    g_assert_false(cpu_view_readq(qts, 0, FAR_NUMBER * NODE_STRIDE + MEM_PROBE,
                                  &value));

    qtest_quit(qts);
}

/*
 * The BTE's source, destination, and notification addresses are physical
 * addresses whose node field names a programmed number, exactly as a
 * processor's do; nothing about an engine's address resolves by the board it
 * sits on. This test replaces the withdrawn assertion that a node-field-zero
 * engine address stayed on the engine's own node.
 */
static void test_bte_resolves_by_programmed_number(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_DUAL);
    const uint64_t node1 = NODE_STRIDE;
    const uint64_t far = FAR_NUMBER * NODE_STRIDE;
    const uint64_t hub1 = HUB_BY_NODE + node1;
    const uint64_t hub_far = HUB_BY_NODE + far;
    sn0_program_nasid(qts, 1);

    memset_line(qts, 0x01500000, 0xaa);
    memset_line(qts, node1 + 0x01500000, 0xbb);
    memset_line(qts, 0x01600000, 0xcc);
    memset_line(qts, node1 + 0x01600000, 0xcc);

    /*
     * A node-field-zero source from node 1's engine names NASID 0, which
     * is node 0's memory, and a NASID-1 destination is node 1's own; the
     * completion interrupt aimed at number 1 lands on node 1.
     */
    qtest_writeq(qts, hub1 + BTE_LENGTH(0), BTE_BUSY | 1);
    qtest_writeq(qts, hub1 + BTE_SOURCE(0), 0x01500000);
    qtest_writeq(qts, hub1 + BTE_DEST(0), node1 + 0x01600000);
    qtest_writeq(qts, hub1 + BTE_INTERRUPT(0), BTE_INT_LEVEL(20) | 1);
    qtest_writeq(qts, hub1 + BTE_CONTROL(0), 0);
    g_assert_cmphex(qtest_readq(qts, hub1 + BTE_LENGTH(0)), ==, 0);
    assert_line(qts, node1 + 0x01600000, 0xaa);
    assert_line(qts, 0x01600000, 0xcc);
    g_assert_cmphex(qtest_readq(qts, hub1 + PI_INT_PEND0), ==, 1ULL << 20);
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + PI_INT_PEND0), ==, 0);

    /*
     * Node 1 renumbered off its board position: its engine, its memory,
     * and its notification word all follow the new number, and a source
     * carrying the number it no longer holds names nothing and fails.
     */
    renumber(qts, 1, FAR_NUMBER);
    memset_line(qts, far + 0x01600000, 0xcc);
    qtest_writeq(qts, hub_far + BTE_LENGTH(0), BTE_BUSY | 1);
    qtest_writeq(qts, hub_far + BTE_SOURCE(0), node1 + 0x01500000);
    qtest_writeq(qts, hub_far + BTE_DEST(0), far + 0x01600000);
    qtest_writeq(qts, hub_far + BTE_CONTROL(0), 0);
    g_assert_cmphex(qtest_readq(qts, hub_far + BTE_LENGTH(0)) & BTE_ERROR,
                    ==, BTE_ERROR);
    assert_line(qts, far + 0x01600000, 0xcc);

    qtest_memset(qts, 0x01700000, 0xff, 8);
    qtest_memset(qts, far + 0x01700000, 0xff, 8);
    qtest_writeq(qts, hub_far + BTE_LENGTH(1), BTE_BUSY | 1);
    qtest_writeq(qts, hub_far + BTE_SOURCE(1), 0x01500000);
    qtest_writeq(qts, hub_far + BTE_DEST(1), far + 0x01680000);
    qtest_writeq(qts, hub_far + BTE_NOTIFY(1), far + 0x01700000);
    qtest_writeq(qts, hub_far + BTE_CONTROL(1), BTE_CTL_NOTIFY);
    g_assert_cmphex(qtest_readq(qts, hub_far + BTE_LENGTH(1)), ==, 0);
    assert_line(qts, far + 0x01680000, 0xaa);
    g_assert_cmphex(qtest_readq(qts, far + 0x01700000), ==, 0);
    g_assert_cmphex(qtest_readq(qts, 0x01700000), ==, ~0ULL);

    qtest_quit(qts);
}

/*
 * The completion interrupt goes to the Hub programmed with the destination
 * number, which on the dual is the peer: a compatibility pin, since the
 * self-or-peer special case and the map agree here.
 */
static void test_bte_completion_interrupt_goes_by_number(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_DUAL);
    const uint64_t hub0 = HUB_BY_NODE;
    const uint64_t hub1 = HUB_BY_NODE + NODE_STRIDE;
    sn0_program_nasid(qts, 1);

    memset_line(qts, 0x01500000, 0xaa);
    qtest_writeq(qts, hub0 + BTE_LENGTH(0), BTE_BUSY | 1);
    qtest_writeq(qts, hub0 + BTE_SOURCE(0), 0x01500000);
    qtest_writeq(qts, hub0 + BTE_DEST(0), 0x01600000);
    qtest_writeq(qts, hub0 + BTE_INTERRUPT(0), BTE_INT_LEVEL(40) | 1);
    qtest_writeq(qts, hub0 + BTE_CONTROL(0), 0);
    g_assert_cmphex(qtest_readq(qts, hub0 + BTE_LENGTH(0)), ==, 0);
    g_assert_cmphex(qtest_readq(qts, hub1 + PI_INT_PEND0), ==, 1ULL << 40);
    g_assert_cmphex(qtest_readq(qts, hub0 + PI_INT_PEND0), ==, 0);

    /* The peer renumbered: only its new number names it. */
    renumber(qts, 1, FAR_NUMBER);
    qtest_writeq(qts, hub0 + BTE_LENGTH(0), BTE_BUSY | 1);
    qtest_writeq(qts, hub0 + BTE_INTERRUPT(0), BTE_INT_LEVEL(41) | FAR_NUMBER);
    qtest_writeq(qts, hub0 + BTE_CONTROL(0), 0);
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + FAR_NUMBER * NODE_STRIDE +
                                PI_INT_PEND0), ==,
                    (1ULL << 40) | (1ULL << 41));

    qtest_quit(qts);
}

/* Put the test card in slot 3 of a node's Bridge and give it its registers. */
static void bridge_card_in_slot(QTestState *qts, unsigned int node,
                                unsigned int slot, uint32_t swap_bits)
{
    qtest_writel(qts, CFG0(node, slot) + PCI_BAR0, CARD_MMIO(slot));
    qtest_writel(qts, CFG0(node, slot) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, REG(node, BRIDGE_DEVICE(slot)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(slot) >> 20) | swap_bits);
}

/*
 * A Bridge interrupt packet is a write to the destination Hub's
 * interrupt-modify register through the Bridge's fabric view: node 1's
 * Bridge aimed at number 0 pends on node 0, and aimed at node 1's own new
 * number pends on node 1.
 */
static void test_bridge_interrupt_reaches_the_peer_hub(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_DUAL
                                 " -device sgi-bridge-dma-test,addr=3,bus=pci.1");
    const uint64_t hub0 = HUB_BY_NODE;
    const uint64_t hub1 = HUB_BY_NODE + NODE_STRIDE;
    sn0_program_nasid(qts, 1);

    bridge_card_in_slot(qts, 1, 3, 0);
    qtest_writel(qts, REG(1, BRIDGE_INT_HOST(3)), (0u << 8) | 42);
    qtest_writel(qts, REG(1, BRIDGE_INT_ENABLE), 1u << 3);

    qtest_writel(qts, WINDOW(1, 3) + DMA_REG_INTERRUPT, 1);
    g_assert_cmphex(qtest_readq(qts, hub0 + PI_INT_PEND0) & (1ULL << 42), ==,
                    1ULL << 42);
    g_assert_cmphex(qtest_readq(qts, hub1 + PI_INT_PEND0) & (1ULL << 42), ==,
                    0);
    qtest_writel(qts, WINDOW(1, 3) + DMA_REG_INTERRUPT, 0);

    renumber(qts, 1, FAR_NUMBER);
    qtest_writel(qts, REG(FAR_NUMBER, BRIDGE_INT_HOST(3)),
                 (FAR_NUMBER << 8) | 43);
    qtest_writel(qts, WINDOW(FAR_NUMBER, 3) + DMA_REG_INTERRUPT, 1);
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + FAR_NUMBER * NODE_STRIDE +
                                PI_INT_PEND0) & (1ULL << 43), ==,
                    1ULL << 43);
    g_assert_cmphex(qtest_readq(qts, hub0 + PI_INT_PEND0) & (1ULL << 43), ==,
                    0);

    qtest_quit(qts);
}

/*
 * A Bridge DMA whose 64-bit address carries a node number lands on the Hub
 * programmed with that number: node 1's Bridge writing NASID 0 fills node
 * 0's RAM, and writing node 1's own new number fills node 1's.
 */
static void test_bridge_dma_reaches_the_peer_node(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_DUAL
                                 " -device sgi-bridge-dma-test,addr=6,bus=pci.1");
    /* Widget 0xa in the top nibble, then the 40-bit system address. */
    const uint64_t pci_node0 = 0xa000000000bc4000ULL;
    const uint64_t pci_far = 0xa000000000bc4000ULL |
                             ((uint64_t)FAR_NUMBER << 32);
    const uint64_t sys = 0x00bc4000ULL;
    uint8_t seen[16];
    unsigned int i;
    sn0_program_nasid(qts, 1);

    bridge_card_in_slot(qts, 1, 6, DEV_SWAP_DIRECT);
    qtest_writel(qts, REG(1, BRIDGE_DIRECT_MAP), 0);
    qtest_memset(qts, sys, 0, sizeof(seen));
    qtest_memset(qts, NODE_STRIDE + sys, 0, sizeof(seen));

    qtest_writel(qts, WINDOW(1, 6) + DMA_REG_ADDRESS, (uint32_t)pci_node0);
    qtest_writel(qts, WINDOW(1, 6) + DMA_REG_ADDRESS_HI, pci_node0 >> 32);
    qtest_writel(qts, WINDOW(1, 6) + DMA_REG_LENGTH, sizeof(seen));
    qtest_writel(qts, WINDOW(1, 6) + DMA_REG_SEED, 0x51);
    qtest_writel(qts, WINDOW(1, 6) + DMA_REG_GO, 1);

    qtest_memread(qts, sys, seen, sizeof(seen));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(0x51 + i));
    }
    qtest_memread(qts, NODE_STRIDE + sys, seen, sizeof(seen));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, 0);
    }

    renumber(qts, 1, FAR_NUMBER);
    qtest_writel(qts, WINDOW(FAR_NUMBER, 6) + DMA_REG_ADDRESS,
                 (uint32_t)pci_far);
    qtest_writel(qts, WINDOW(FAR_NUMBER, 6) + DMA_REG_ADDRESS_HI,
                 pci_far >> 32);
    qtest_writel(qts, WINDOW(FAR_NUMBER, 6) + DMA_REG_SEED, 0x71);
    qtest_writel(qts, WINDOW(FAR_NUMBER, 6) + DMA_REG_GO, 1);
    qtest_memread(qts, FAR_NUMBER * NODE_STRIDE + sys, seen, sizeof(seen));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(0x71 + i));
    }

    qtest_quit(qts);
}

/* The migrate-and-poll steps of tests/qtest/sn0-migration-test.c. */
static void migrate_stopped_vm(QTestState *from, const char *uri)
{
    g_autoptr(QDict) response = NULL;

    response = qtest_qmp(from,
                         "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }",
                         uri);
    g_assert(qdict_haskey(response, "return"));

    for (;;) {
        g_autoptr(QDict) query = qtest_qmp(from,
                                           "{ 'execute': 'query-migrate' }");
        QDict *ret = qdict_get_qdict(query, "return");
        const char *status = qdict_get_str(ret, "status");

        if (!strcmp(status, "completed")) {
            break;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_usleep(1000);
    }
}

/*
 * Reachability is derived, not carried: a renumbered peer, a latched vector
 * reply, and a remote read all come back the same after a migration.
 */
static void test_direct_adapter_migration(const void *data)
{
    g_autofree char *tmpdir = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    const uint64_t hub0 = HUB_BY_NODE;
    uint64_t status;

    tmpdir = g_dir_make_tmp("sn0-fabric-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpdir);

    from = qtest_init(MACHINE_DUAL " -S");
    sn0_program_nasid(from, 1);
    qtest_writeq(from, NODE_STRIDE + MEM_PROBE, MEM_PLANT_NODE1);
    qtest_writeq(from, NODE_STRIDE + HUB_LOCAL + NI_SCRATCH_0,
                 0x0123456789abcdefULL);
    renumber(from, 1, FAR_NUMBER);
    status = vector_operation(from, hub0, NI_SCRATCH_0, PIOTYPE_READ);
    g_assert_cmphex(status & (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_READ);

    dest_args = g_strdup_printf(MACHINE_DUAL " -incoming %s", uri);
    to = qtest_init(dest_args);
    migrate_stopped_vm(from, uri);

    g_assert_cmphex(qtest_readq(to, hub0 + NI_VECTOR_STATUS) &
                    (NVS_VALID | NVS_TYPE_MASK), ==,
                    NVS_VALID | PIOTYPE_READ);
    g_assert_cmphex(qtest_readq(to, hub0 + NI_VECTOR_READ_DATA), ==,
                    0x0123456789abcdefULL);
    g_assert_cmphex(cpu_view_word64(to, 0, FAR_NUMBER * NODE_STRIDE +
                                    MEM_PROBE), ==, MEM_PLANT_NODE1);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * The status register's link-up bit is the fabric's to set. On the dual it
 * reads set; marking the one link down clears it on both Hubs and a bringup
 * back sets it again, and a system reset leaves it following the fabric.
 */
static void test_link_status_follows_the_fabric(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_DUAL);
    uint64_t hub1 = HUB_BY_NODE + NODE_STRIDE;

    sn0_program_nasid(qts, 1);
    /* That numbering is itself a vector op; clear its latched completion. */
    qtest_readq(qts, HUB_BY_NODE + NI_VECTOR_CLEAR);

    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + NI_STATUS) &
                    NI_STATUS_LINK_UP, ==, NI_STATUS_LINK_UP);
    g_assert_cmphex(qtest_readq(qts, hub1 + NI_STATUS) & NI_STATUS_LINK_UP,
                    ==, NI_STATUS_LINK_UP);

    fabric_set_down_links(qts, "peer-craylink");
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + NI_STATUS) &
                    NI_STATUS_LINK_UP, ==, 0);
    g_assert_cmphex(qtest_readq(qts, hub1 + NI_STATUS) & NI_STATUS_LINK_UP,
                    ==, 0);
    /* With the link down a vector to the peer latches no success. */
    g_assert_cmphex(vector_operation(qts, HUB_BY_NODE, NI_SCRATCH_0,
                                     PIOTYPE_READ) & NVS_VALID, ==, 0);

    fabric_set_down_links(qts, "");
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + NI_STATUS) &
                    NI_STATUS_LINK_UP, ==, NI_STATUS_LINK_UP);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/sn0/fabric/direct-adapter-map-is-own-plus-peer",
                        NULL, test_direct_adapter_map_is_own_plus_peer);
    qtest_add_data_func("/sn0/fabric/link-status-follows-the-fabric",
                        NULL, test_link_status_follows_the_fabric);
    qtest_add_data_func("/sn0/fabric/bte-resolves-by-programmed-number",
                        NULL, test_bte_resolves_by_programmed_number);
    qtest_add_data_func("/sn0/fabric/bte-completion-interrupt-goes-by-number",
                        NULL, test_bte_completion_interrupt_goes_by_number);
    qtest_add_data_func("/sn0/fabric/bridge-interrupt-reaches-the-peer-hub",
                        NULL, test_bridge_interrupt_reaches_the_peer_hub);
    qtest_add_data_func("/sn0/fabric/bridge-dma-reaches-the-peer-node",
                        NULL, test_bridge_dma_reaches_the_peer_node);
    qtest_add_data_func("/sn0/fabric/direct-adapter-migration",
                        NULL, test_direct_adapter_migration);

    return g_test_run();
}
