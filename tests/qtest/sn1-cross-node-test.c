/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Two-chassis Origin 300 node decoding in hw/mips/sgi/sn1-node.c. Numbered
 * accesses reach the claiming node; local forms reach the reader.
 *
 * An unclaimed number is a decode error. qtest uses the global map, with
 * nodes placed at node order << 33. Monitor xp with cpu-index reads a CPU's
 * view: CPUs 0/1 belong to node 0, CPUs 2/3 to node 1. The zero-filled PROM
 * does not execute under qtest.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "sgi-test.h"
#include "sgi-wait.h"
#include "sgi-mips-emit.h"
#include "sgi-migrate.h"

#define MACHINE_ARGS "-M origin300,topology=origin300-two-chassis " \
                     "-cpu R14000 -smp 4 -m 1024"

#define NODE_SHIFT 33
#define NODE(n) ((uint64_t)(n) << NODE_SHIFT)
/* The Bedrock's node-named register form, and its local alias (IALIAS). */
#define BEDROCK_NODE(n) (SPACE_IO + NODE(n) + UINT64_C(0x01800000))
#define BEDROCK_LOCAL (SPACE_IO + UINT64_C(0x01000000))
#define HUB_LOCAL_REGISTERS (SPACE_HUB + UINT64_C(0x10000000))
#define HUB_REMOTE_REGISTERS (SPACE_HUB + UINT64_C(0x30000000))
#define LB_REVISION UINT64_C(0x600000)
#define LB_SCRATCH2 UINT64_C(0x608010)
#define MD_MEMORY_CONFIG UINT64_C(0x780008)
#define MD_INSTALLED_SELECT2 ((UINT64_C(2) << 32) | UINT64_C(0x3300))
#define MD_BANK0_SELECT2 ((UINT64_C(2) << 32) | UINT64_C(0x3100))
#define MD_BANK1_BASE UINT64_C(0x40000000)
#define REVISION_NODE_SHIFT 32
#define REVISION_NODE_MASK (UINT64_C(0xff) << REVISION_NODE_SHIFT)
#define CABLE_PATH "/machine/numalink-cable"

#define PROBE UINT64_C(0x1000)
#define WORD0 UINT64_C(0x0000000000000a00)
#define WORD1 UINT64_C(0x0000000000000b11)
#define SCRATCH0 UINT64_C(0x5c0)
#define SCRATCH1 UINT64_C(0x5c1)
/* Slice A's LED in the Hub-special local register window, bits 7:0. */
#define HUB_LED0 (HUB_LOCAL_REGISTERS + UINT64_C(0xc0))
#define LED0 UINT64_C(0xa0)
#define LED1 UINT64_C(0xa1)

static char *prom_path;

static char *create_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp("sn1-cross-node-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

/*
 * qtest's own accesses go through processor 0's view, as a program on node 0
 * would, so node 1 is reached the way the PROM reaches a peer before it has a
 * number: through node 0's vector engine on route 0. The launch sequence is
 * the firmware's (sn1-bedrock-link-test.c).
 */
#define LB_VECTOR_PARAMS UINT64_C(0x60c000)
#define LB_VECTOR_ROUTE UINT64_C(0x60c008)
#define LB_VECTOR_DATA UINT64_C(0x60c010)
#define LB_VECTOR_STATUS UINT64_C(0x60c020)
#define LB_VECTOR_READ UINT64_C(0x60c030)
#define LB_VECTOR_STATUS_CLEAR UINT64_C(0x60c038)
#define VECTOR_WRITE UINT64_C(1)
#define VECTOR_VALID (UINT64_C(1) << 63)
#define VECTOR_REQUEST(n) ((uint64_t)(n) << 40)

static uint64_t peer_access(QTestState *qts, uint64_t hub_offset,
                            bool write, uint64_t data)
{
    uint64_t status;

    (void)qtest_readq(qts, BEDROCK_LOCAL + LB_VECTOR_STATUS_CLEAR);
    qtest_writeq(qts, BEDROCK_LOCAL + LB_VECTOR_ROUTE, 0);
    qtest_writeq(qts, BEDROCK_LOCAL + LB_VECTOR_DATA, data);
    qtest_writeq(qts, BEDROCK_LOCAL + LB_VECTOR_PARAMS,
                 (hub_offset & UINT64_C(0xfffff8)) | VECTOR_REQUEST(0x1c8) |
                 (write ? VECTOR_WRITE : 0));
    status = qtest_readq(qts, BEDROCK_LOCAL + LB_VECTOR_STATUS);
    g_assert_cmphex(status & VECTOR_VALID, ==, VECTOR_VALID);
    g_assert_cmphex(status & 7, ==, write ? VECTOR_WRITE : 0);
    return qtest_readq(qts, BEDROCK_LOCAL + LB_VECTOR_READ);
}

/* Store a node number through the node's own revision word. */
static void set_node_number(QTestState *qts, unsigned int node,
                            unsigned int number)
{
    uint64_t revision = node ?
        peer_access(qts, LB_REVISION, false, 0) :
        qtest_readq(qts, BEDROCK_LOCAL + LB_REVISION);

    revision = (revision & ~REVISION_NODE_MASK) |
               ((uint64_t)number << REVISION_NODE_SHIFT);
    if (node) {
        peer_access(qts, LB_REVISION, true, revision);
    } else {
        qtest_writeq(qts, BEDROCK_LOCAL + LB_REVISION, revision);
    }
}

/*
 * Both nodes' memory answers once each node's memory unit has its banks
 * configured, and each node's RAM and scratch register carry a value that
 * says which node answered. Node 1 is numbered 1 to reach it, then set back
 * to 0: both local blocks leave reset carrying 0.
 */
static QTestState *machine_start(void)
{
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(MACHINE_ARGS " -bios %s", quoted);

    qtest_writeq(qts, BEDROCK_LOCAL + MD_MEMORY_CONFIG, MD_INSTALLED_SELECT2);
    peer_access(qts, MD_MEMORY_CONFIG, true, MD_INSTALLED_SELECT2);
    peer_access(qts, LB_SCRATCH2, true, SCRATCH1);
    qtest_writeq(qts, BEDROCK_LOCAL + LB_SCRATCH2, SCRATCH0);
    qtest_writeq(qts, PROBE, WORD0);
    qtest_writeq(qts, HUB_LED0, LED0);
    set_node_number(qts, 1, 1);
    qtest_writeq(qts, NODE(1) + PROBE, WORD1);
    qtest_writeq(qts, NODE(1) + HUB_LED0, LED1);
    set_node_number(qts, 1, 0);
    return qts;
}

/*
 * One eight-byte read as processor cpu sees it. Returns false when the
 * processor's view answers nothing there (the monitor cannot read it).
 */
static bool cpu_readq(QTestState *qts, unsigned int cpu, uint64_t address,
                      uint64_t *value)
{
    g_autofree char *command = g_strdup_printf("xp/1gx 0x%" PRIx64, address);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'human-monitor-command',"
        "  'arguments': { 'command-line': %s, 'cpu-index': %u } }",
        command, cpu);
    const char *text;
    const char *colon;

    g_assert(qdict_haskey(response, "return"));
    text = qdict_get_str(response, "return");
    if (strstr(text, "Cannot access memory")) {
        return false;
    }
    colon = strchr(text, ':');
    g_assert_nonnull(colon);
    g_assert_cmpint(sscanf(colon + 1, "%" SCNx64, value), ==, 1);
    return true;
}

static uint64_t cpu_read_ok(QTestState *qts, unsigned int cpu,
                            uint64_t address)
{
    uint64_t value = 0;

    g_assert_true(cpu_readq(qts, cpu, address, &value));
    return value;
}

static void assert_unreachable(QTestState *qts, unsigned int cpu,
                               uint64_t address)
{
    uint64_t value;

    g_assert_false(cpu_readq(qts, cpu, address, &value));
}

/*
 * From reset both local blocks carry 0, as the PROM's first store leaves
 * them: each node reaches only itself at node field zero, and nothing answers
 * at node 1's placement.
 */
static void test_equal_numbers_stay_local(void)
{
    QTestState *qts = machine_start();

    g_assert_cmphex(cpu_read_ok(qts, 0, PROBE), ==, WORD0);
    g_assert_cmphex(cpu_read_ok(qts, 2, PROBE), ==, WORD1);
    assert_unreachable(qts, 0, NODE(1) + PROBE);
    assert_unreachable(qts, 2, NODE(1) + PROBE);
    qtest_quit(qts);
}

/*
 * Once node 1 carries 1, node field zero names node 0 from either node, and
 * node field 1 names node 1 from either node.
 */
static void test_numbered_nodes_reach_each_other(void)
{
    QTestState *qts = machine_start();

    set_node_number(qts, 1, 1);
    g_assert_cmphex(cpu_read_ok(qts, 0, PROBE), ==, WORD0);
    g_assert_cmphex(cpu_read_ok(qts, 2, PROBE), ==, WORD0);
    g_assert_cmphex(cpu_read_ok(qts, 0, NODE(1) + PROBE), ==, WORD1);
    g_assert_cmphex(cpu_read_ok(qts, 3, NODE(1) + PROBE), ==, WORD1);
    g_assert_cmphex(cpu_read_ok(qts, 1, BEDROCK_NODE(1) + LB_SCRATCH2), ==,
                    SCRATCH1);
    g_assert_cmphex(cpu_read_ok(qts, 2, BEDROCK_NODE(0) + LB_SCRATCH2), ==,
                    SCRATCH0);
    qtest_quit(qts);
}

/* The claim follows the number: node 1 renumbered to 5 answers only at 5. */
static void test_renumbering_moves_the_claim(void)
{
    QTestState *qts = machine_start();

    set_node_number(qts, 1, 1);
    set_node_number(qts, 1, 5);
    g_assert_cmphex(cpu_read_ok(qts, 0, NODE(5) + PROBE), ==, WORD1);
    assert_unreachable(qts, 0, NODE(1) + PROBE);
    g_assert_cmphex(cpu_read_ok(qts, 2, NODE(5) + PROBE), ==, WORD1);
    qtest_quit(qts);
}

/*
 * The Bedrock's own register alias and the Hub special local register window
 * (read here through a processor LED) reach the reader's node, whatever
 * number it carries.
 */
static void test_local_forms_are_the_readers(void)
{
    QTestState *qts = machine_start();
    unsigned int round;

    for (round = 0; round < 2; round++) {
        g_assert_cmphex(cpu_read_ok(qts, 0, BEDROCK_LOCAL + LB_SCRATCH2), ==,
                        SCRATCH0);
        g_assert_cmphex(cpu_read_ok(qts, 3, BEDROCK_LOCAL + LB_SCRATCH2), ==,
                        SCRATCH1);
        g_assert_cmphex(cpu_read_ok(qts, 1, HUB_LED0), ==, LED0);
        g_assert_cmphex(cpu_read_ok(qts, 2, HUB_LED0), ==, LED1);
        /* The uncached alias of the reader's own memory. */
        g_assert_cmphex(cpu_read_ok(qts, 2, SPACE_HUB + PROBE), ==, WORD1);
        /* Again after node 1 takes a number of its own. */
        set_node_number(qts, 1, 1);
    }
    qtest_quit(qts);
}

/* A cable marked down leaves each node only itself. */
static void test_cable_down_makes_the_peer_unreachable(void)
{
    QTestState *qts = machine_start();

    set_node_number(qts, 1, 1);
    g_assert_cmphex(cpu_read_ok(qts, 0, NODE(1) + PROBE), ==, WORD1);
    qtest_qom_set_bool(qts, CABLE_PATH, "down", true);
    assert_unreachable(qts, 0, NODE(1) + PROBE);
    assert_unreachable(qts, 2, PROBE);
    g_assert_cmphex(cpu_read_ok(qts, 2, NODE(1) + PROBE), ==, WORD1);
    qtest_qom_set_bool(qts, CABLE_PATH, "down", false);
    g_assert_cmphex(cpu_read_ok(qts, 0, NODE(1) + PROBE), ==, WORD1);
    qtest_quit(qts);
}

#define PI_CACHED_ALIAS UINT64_C(0x28)
/* The PROM's early private data window, which forwards to node memory. */
#define PROMDATA_PROBE UINT64_C(0x100040)

/*
 * Before any number is assigned the PROM keeps its early data at physical
 * 0x100000 through a filtered window that forwards to the processor's own
 * node's memory. Node 1's processors must reach node 1's memory there, not
 * node 0's, which sits at the same node-field-zero address in the global
 * map's first window.
 */
static void test_prom_data_window_is_the_readers(void)
{
    QTestState *qts = machine_start();

    qtest_writeq(qts, PROMDATA_PROBE, WORD0);
    set_node_number(qts, 1, 1);
    qtest_writeq(qts, NODE(1) + PROMDATA_PROBE, WORD1);
    set_node_number(qts, 1, 0);
    g_assert_cmphex(cpu_read_ok(qts, 0, PROMDATA_PROBE), ==, WORD0);
    g_assert_cmphex(cpu_read_ok(qts, 2, PROMDATA_PROBE), ==, WORD1);
    qtest_quit(qts);
}

/*
 * The cached alias: below the size node 1's processor interface carries, node
 * field zero is node 1's own memory; at and above it, node 0's. The window
 * follows the register as soon as it is written.
 */
static void test_cached_alias_bounds_the_readers_memory(void)
{
    QTestState *qts = machine_start();

    set_node_number(qts, 1, 1);
    qtest_writeq(qts, NODE(1) + UINT64_C(0x800), WORD1);
    qtest_writeq(qts, UINT64_C(0x800), WORD0);
    g_assert_cmphex(cpu_read_ok(qts, 2, UINT64_C(0x800)), ==, WORD0);
    /* Size code 1, 4 KiB, on node 1's interface 0, where processor 2 is. */
    peer_access(qts, PI_CACHED_ALIAS, true, 1);
    g_assert_cmphex(cpu_read_ok(qts, 2, UINT64_C(0x800)), ==, WORD1);
    g_assert_cmphex(cpu_read_ok(qts, 2, PROBE), ==, WORD0);
    /* Node 0's processors keep reading node 0 there. */
    g_assert_cmphex(cpu_read_ok(qts, 0, UINT64_C(0x800)), ==, WORD0);
    peer_access(qts, PI_CACHED_ALIAS, true, 0);
    g_assert_cmphex(cpu_read_ok(qts, 2, UINT64_C(0x800)), ==, WORD0);
    qtest_quit(qts);
}

/* Block transfer engine 0 in node 0's I/O interface, reached locally. */
#define BTE0 (BEDROCK_LOCAL + UINT64_C(0x410000))
#define BTE_STATUS 0x00
#define BTE_DESTINATION 0x10
#define BTE_CONTROL 0x18
#define BTE_INTERRUPT 0x28
#define BTE_BUSY (UINT64_C(1) << 20)
#define BTE_ZERO_FILL 1
#define PI_PENDING0 UINT64_C(0x98)

/*
 * One zero-filled line to destination, then the completion interrupt the
 * interrupt-address register names: node in bits 8:1, level in 22:16,
 * processor interface 0.
 */
static void bte_zero_line(QTestState *qts, uint64_t destination,
                          unsigned int node, unsigned int level)
{
    qtest_writeq(qts, BTE0 + BTE_STATUS, 1 | BTE_BUSY);
    qtest_writeq(qts, BTE0 + BTE_DESTINATION, destination);
    qtest_writeq(qts, BTE0 + BTE_INTERRUPT,
                 ((uint64_t)node << 1) | ((uint64_t)level << 16));
    qtest_writeq(qts, BTE0 + BTE_CONTROL, BTE_ZERO_FILL);
    qtest_clock_step(qts, 1);
    qtest_clock_step(qts, 1);
}

static uint64_t pending0(QTestState *qts, unsigned int cpu)
{
    return cpu_read_ok(qts, cpu, BEDROCK_LOCAL + PI_PENDING0);
}

/*
 * A BTE completion interrupt goes to the node whose local block carries the
 * number it names, the engine's own node included, and nowhere when no
 * reachable node carries it.
 */
static void test_bte_completion_goes_by_number(void)
{
    QTestState *qts = machine_start();

    set_node_number(qts, 1, 1);
    bte_zero_line(qts, UINT64_C(0x300000), 1, 5);
    g_assert_cmphex(pending0(qts, 2) & (UINT64_C(1) << 5), !=, 0);
    g_assert_cmphex(pending0(qts, 0) & (UINT64_C(1) << 5), ==, 0);

    /* Node 0 renumbered to 3: 3 is itself, and 0 is nobody. */
    set_node_number(qts, 0, 3);
    bte_zero_line(qts, NODE(3) + UINT64_C(0x300000), 3, 6);
    g_assert_cmphex(pending0(qts, 0) & (UINT64_C(1) << 6), !=, 0);
    g_assert_cmphex(pending0(qts, 2) & (UINT64_C(1) << 6), ==, 0);
    bte_zero_line(qts, NODE(3) + UINT64_C(0x300000), 0, 7);
    g_assert_cmphex(pending0(qts, 0) & (UINT64_C(1) << 7), ==, 0);
    g_assert_cmphex(pending0(qts, 2) & (UINT64_C(1) << 7), ==, 0);
    qtest_quit(qts);
}


/*
 * An interrupt packet the IO-8's XBridge sends goes to the node whose local
 * block carries the number its node field names: the XBridge head at widget
 * 0xf on node 0 is programmed, as IRIX programs it, to post to the processor
 * interface's pending-modify register in the register space of a named node,
 * then forced.
 */
#define XBRIDGE_F (SPACE_IO + UINT64_C(0xf) * UINT64_C(0x1000000))
#define XBRIDGE_INT_ENABLE 0x108
#define XBRIDGE_INT_UPPER 0x30
#define XBRIDGE_INT_LOWER 0x38
#define XBRIDGE_INT_HOST(pin) (0x130 + (pin) * 8)
#define XBRIDGE_INT_FORCE_ALWAYS(pin) (0x180 + (pin) * 8)
#define XBRIDGE_HOST_PORT 0xa
#define XBRIDGE_HOST_REGISTER (UINT32_C(1) << 17)
#define XBRIDGE_HOST_NODE_SHIFT 9
#define PI_PENDING_MODIFY UINT64_C(0x90)
#define PI_REGISTERS UINT64_C(0x1800000)

static void xbridge_writel(QTestState *qts, uint64_t offset, uint32_t value)
{
    qtest_writel(qts, XBRIDGE_F + offset + 4, value);
}

static void xbridge_interrupt(QTestState *qts, unsigned int node,
                              unsigned int bit)
{
    const unsigned int pin = 3;

    xbridge_writel(qts, XBRIDGE_INT_UPPER, XBRIDGE_HOST_PORT << 16);
    xbridge_writel(qts, XBRIDGE_INT_LOWER, PI_REGISTERS + PI_PENDING_MODIFY);
    xbridge_writel(qts, XBRIDGE_INT_HOST(pin), XBRIDGE_HOST_REGISTER |
                   (node << XBRIDGE_HOST_NODE_SHIFT) | bit);
    xbridge_writel(qts, XBRIDGE_INT_ENABLE, 1U << pin);
    xbridge_writel(qts, XBRIDGE_INT_FORCE_ALWAYS(pin), 1);
}

static void test_remote_interrupt_reaches_the_peer(void)
{
    QTestState *qts = machine_start();

    set_node_number(qts, 1, 5);
    xbridge_interrupt(qts, 5, 20);
    g_assert_cmphex(pending0(qts, 2) & (UINT64_C(1) << 20), !=, 0);
    g_assert_cmphex(pending0(qts, 0) & (UINT64_C(1) << 20), ==, 0);
    xbridge_interrupt(qts, 0, 21);
    g_assert_cmphex(pending0(qts, 0) & (UINT64_C(1) << 21), !=, 0);
    g_assert_cmphex(pending0(qts, 2) & (UINT64_C(1) << 21), ==, 0);
    /* A number nobody reachable carries: the packet arrives nowhere. */
    xbridge_interrupt(qts, 1, 22);
    g_assert_cmphex(pending0(qts, 0) & (UINT64_C(1) << 22), ==, 0);
    g_assert_cmphex(pending0(qts, 2) & (UINT64_C(1) << 22), ==, 0);
    qtest_quit(qts);
}

/* Node 1's own block transfer engine, reached through the vector engine. */
#define BTE0_HUB UINT64_C(0x410000)

static void peer_bte_zero_line(QTestState *qts, uint64_t destination)
{
    peer_access(qts, BTE0_HUB + BTE_STATUS, true, 1 | BTE_BUSY);
    peer_access(qts, BTE0_HUB + BTE_DESTINATION, true, destination);
    peer_access(qts, BTE0_HUB + BTE_INTERRUPT, true, 0);
    peer_access(qts, BTE0_HUB + BTE_CONTROL, true, BTE_ZERO_FILL);
    qtest_clock_step(qts, 1);
    qtest_clock_step(qts, 1);
}

/*
 * The engines' data moves by the number a node carries, not by where the node
 * is placed in QEMU's global map, and without the reader-relative cached
 * alias. Node 1 renumbered to 5 is written at node field 5, where the global
 * map has nothing; node 1's own engine writing node field zero with node 1's
 * cached alias covering the line writes node 0's memory, as a processor's
 * cached alias does not apply to the engine.
 */
static void bte_cpu_writeq(QTestState *qts, unsigned cpu, uint64_t address,
                            uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, cpu, address, bytes, sizeof(bytes)));
}

static uint64_t bte_cpu_readq(QTestState *qts, unsigned cpu, uint64_t address)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, cpu, address, bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void test_routed_hub_special_decoding(void)
{
    g_autofree char *image = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    uint64_t pi_c0;
    int fd;

    fd = g_file_open_tmp("sn1-hub-special-storage-XXXXXX", &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf(
        "-M origin300,topology=origin300-numalink-pci-expansion-experiment,"
        "expansion-storage.0=expansion-enclosure=cfg "
        "-cpu R14000 -smp 4 -m 1G -bios %s "
        "-drive if=none,id=cfg,file=%s,format=raw",
        prom_path, image);
    bte_cpu_writeq(qts, 2, BEDROCK_LOCAL + LB_REVISION, UINT64_C(2) << 32);
    bte_cpu_writeq(qts, 0, BEDROCK_LOCAL + 0x681010, 5);
    bte_cpu_writeq(qts, 2, BEDROCK_LOCAL + 0x681000, 3);
    qtest_irq_intercept_out_named(qts, "/machine/node1/bedrock", "nmi");

    g_assert_cmphex(cpu_read_ok(qts, 0, HUB_REMOTE_REGISTERS), ==, 0);
    bte_cpu_writeq(qts, 0, HUB_REMOTE_REGISTERS + 0xc0, 0x5a);
    g_assert_cmphex(cpu_read_ok(qts, 0, HUB_REMOTE_REGISTERS + 0xc0), ==,
                    0x5a);
    bte_cpu_writeq(qts, 0, NODE(2) + HUB_REMOTE_REGISTERS + 0x70, 0x29);
    bte_cpu_writeq(qts, 0, NODE(2) + HUB_REMOTE_REGISTERS + 0x78, 0x29);
    for (unsigned line = 0; line < 4; line++) {
        g_assert_cmpuint(qtest_get_irq_raise_count(qts, line), ==, 0);
    }
    pi_c0 = bte_cpu_readq(qts, 0, BEDROCK_NODE(2) + 0xc0);
    bte_cpu_writeq(qts, 0, NODE(2) + HUB_REMOTE_REGISTERS + 0xc0, 0xa5);
    g_assert_cmphex(bte_cpu_readq(qts, 0, NODE(2) +
                                   HUB_REMOTE_REGISTERS + 0xc0), ==, 0xa5);
    g_assert_cmphex(bte_cpu_readq(qts, 0, BEDROCK_NODE(2) + 0xc0), ==,
                    pi_c0);
    assert_unreachable(qts, 0, NODE(2) + HUB_REMOTE_REGISTERS + 0xe0);

    bte_cpu_writeq(qts, 0, BEDROCK_NODE(2) + 0x70, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, 1);
    qtest_quit(qts);
    unlink(image);
}

static uint64_t bte_memory_transfer(QTestState *qts, unsigned node,
    unsigned engine, uint64_t source, uint64_t destination,
    uint64_t notification, bool zero, bool named)
{
    unsigned cpu = node * 2;
    uint64_t base = (named ? BEDROCK_NODE(node ? 2 : 0) + BTE0_HUB : BTE0) +
                    engine * 0x10000;

    bte_cpu_writeq(qts, cpu, base + BTE_STATUS, 1 | BTE_BUSY);
    bte_cpu_writeq(qts, cpu, base + 8, source);
    bte_cpu_writeq(qts, cpu, base + BTE_DESTINATION, destination);
    bte_cpu_writeq(qts, cpu, base + 0x20, notification);
    bte_cpu_writeq(qts, cpu, base + BTE_CONTROL, 0x10 | zero);
    return bte_cpu_readq(qts, cpu, base + BTE_STATUS);
}

#define PI1_OFFSET UINT64_C(0x200000)
#define PI_PENDING_MODIFY UINT64_C(0x90)
#define PI_MASK0_A UINT64_C(0xa8)
#define CPU_CAUSE_IRQ0 (1U << 10)

static uint32_t interrupt_cpu_cause(QTestState *qts, unsigned cpu)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers %u", cpu);
    const char *field = strstr(registers, "Cause   0x");
    uint32_t cause;

    g_assert_nonnull(field);
    g_assert_cmpint(sscanf(field + strlen("Cause   0x"), "%" SCNx32,
                           &cause), ==, 1);
    return cause;
}

static void check_remote_pi_interrupt(QTestState *qts, bool connected)
{
    uint8_t bytes[8];

    for (unsigned node = 0; node < 2; node++) {
        unsigned target = 1 - node;
        unsigned nasid = target ? 2 : 0;

        for (unsigned pi = 0; pi < 2; pi++) {
            uint64_t local = BEDROCK_LOCAL + pi * PI1_OFFSET;
            uint64_t named = BEDROCK_NODE(nasid) + pi * PI1_OFFSET;
            uint64_t bit = UINT64_C(1) << (20 + pi);
            uint64_t set = 0x100 | (20 + pi);
            unsigned cpu = target * 2 + pi;

            bte_cpu_writeq(qts, target * 2, local + PI_MASK0_A, bit);
            stq_be_p(bytes, set);
            g_assert_true(qtest_memwrite_cpu(qts, node * 2,
                named + PI_PENDING_MODIFY, bytes, sizeof(bytes)));
            g_assert_cmphex(bte_cpu_readq(qts, target * 2,
                local + PI_PENDING0), ==, connected ? bit : 0);
            g_assert_cmphex(interrupt_cpu_cause(qts, cpu) & CPU_CAUSE_IRQ0,
                            ==, connected ? CPU_CAUSE_IRQ0 : 0);
            g_assert_cmphex(bte_cpu_readq(qts, node * 2,
                local + PI_PENDING0), ==, 0);
            g_assert_cmphex(interrupt_cpu_cause(qts, node * 2 + pi) &
                            CPU_CAUSE_IRQ0, ==, 0);
            if (connected) {
                bte_cpu_writeq(qts, node * 2,
                               named + PI_PENDING_MODIFY, 20 + pi);
            }
            g_assert_cmphex(bte_cpu_readq(qts, target * 2,
                local + PI_PENDING0), ==, 0);
            g_assert_cmphex(interrupt_cpu_cause(qts, cpu) & CPU_CAUSE_IRQ0,
                            ==, 0);
            stq_be_p(bytes, set);
            g_assert_true(qtest_memwrite_cpu(qts, node * 2,
                BEDROCK_NODE(3) + pi * PI1_OFFSET + PI_PENDING_MODIFY,
                bytes, sizeof(bytes)));
            g_assert_cmphex(bte_cpu_readq(qts, target * 2,
                local + PI_PENDING0), ==, 0);
            bte_cpu_writeq(qts, target * 2, local + PI_MASK0_A, 0);
            g_test_message("CPU node%u -> node%u PI%u set/clear, "
                           "Cause and absent node: route %s",
                           node, target, pi, connected ? "up" : "down");
        }
    }
}

static void check_remote_bte_interrupt(QTestState *qts, bool connected)
{
    for (unsigned node = 0; node < 2; node++) {
        unsigned target = 1 - node;
        unsigned nasid = target ? 2 : 0;
        uint64_t own = NODE(node ? 2 : 0);

        for (unsigned engine = 0; engine < 2; engine++) {
            uint64_t base = BEDROCK_NODE(node ? 2 : 0) + BTE0_HUB +
                            engine * 0x10000;

            for (unsigned pi = 0; pi < 2; pi++) {
                uint64_t local = BEDROCK_LOCAL + pi * PI1_OFFSET;
                uint64_t bit = UINT64_C(1) << (24 + pi);
                unsigned cpu = target * 2 + pi;

                bte_cpu_writeq(qts, target * 2, local + PI_MASK0_A, bit);
                for (unsigned absent = 0; absent < 2; absent++) {
                    unsigned destination = absent ? 3 : nasid;
                    bool delivered = connected && !absent;

                    bte_cpu_writeq(qts, node * 2, base + BTE_STATUS,
                                   1 | BTE_BUSY);
                    bte_cpu_writeq(qts, node * 2, base + BTE_DESTINATION,
                                   own + 0x40000);
                    bte_cpu_writeq(qts, node * 2, base + BTE_INTERRUPT,
                        ((uint64_t)destination << 1) | pi |
                        ((uint64_t)(24 + pi) << 16));
                    bte_cpu_writeq(qts, node * 2, base + BTE_CONTROL,
                                   BTE_ZERO_FILL);
                    qtest_clock_step(qts, 1);
                    qtest_clock_step(qts, 1);
                    g_assert_cmphex(bte_cpu_readq(qts, node * 2,
                        base + BTE_STATUS), ==, 0);
                    g_assert_cmphex(bte_cpu_readq(qts, target * 2,
                        local + PI_PENDING0), ==, delivered ? bit : 0);
                    g_assert_cmphex(interrupt_cpu_cause(qts, cpu) &
                        CPU_CAUSE_IRQ0, ==, delivered ? CPU_CAUSE_IRQ0 : 0);
                    g_assert_cmphex(bte_cpu_readq(qts, node * 2,
                        local + PI_PENDING0), ==, 0);
                    bte_cpu_writeq(qts, target * 2,
                                   local + PI_PENDING_MODIFY, 24 + pi);
                    g_assert_cmphex(interrupt_cpu_cause(qts, cpu) &
                                    CPU_CAUSE_IRQ0, ==, 0);
                }
                bte_cpu_writeq(qts, target * 2, local + PI_MASK0_A, 0);
                g_test_message("BTE node%u engine%u -> node%u PI%u "
                               "completion, Cause and absent node: route %s",
                               node, engine, target, pi,
                               connected ? "up" : "down");
            }
        }
    }
}

static void test_bte_memory_route(const void *data)
{
    const char *topology = data;
    bool routed = strstr(topology, "expansion") != NULL;
    g_autofree char *image = NULL;
    g_autoptr(GError) error = NULL;
    uint8_t bytes[256], result[256];
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-bte-route-storage-XXXXXX", &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf(
        "-M origin300,topology=%s%s -cpu R14000 -smp 4 -m 1G -bios %s "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-nodefaults -display none -d guest_errors",
        topology, routed ? ",expansion-storage.0=expansion-enclosure=cfg" : "",
        prom_path, image);
    bte_cpu_writeq(qts, 2, BEDROCK_LOCAL + LB_REVISION, UINT64_C(2) << 32);
    for (unsigned node = 0; node < 2; node++) {
        bte_cpu_writeq(qts, node * 2, BEDROCK_LOCAL + MD_MEMORY_CONFIG,
                       MD_INSTALLED_SELECT2);
        /* BTE memory has no inbound widget/device identity. */
        bte_cpu_writeq(qts, node * 2, BEDROCK_LOCAL + 0x400118, 0);
    }
    if (routed) {
        bte_cpu_writeq(qts, 0, BEDROCK_LOCAL + 0x681010, 5);
        bte_cpu_writeq(qts, 2, BEDROCK_LOCAL + 0x681000, 3);
    }
    for (unsigned node = 0; node < 2; node++) {
        uint64_t own = NODE(node ? 2 : 0);
        uint64_t peer = NODE(node ? 0 : 2);
        uint8_t source[128];

        memset(source, 0x80 + node, sizeof(source));
        g_assert_true(qtest_memwrite_cpu(qts, node * 2, own + 0x10000,
                                         source, sizeof(source)));
        for (unsigned engine = 0; engine < 2; engine++) {
            for (unsigned named = 0; named < 2; named++) {
                for (unsigned zero = 0; zero < 2; zero++) {
                    memset(bytes, 0xa0 + node, sizeof(bytes));
                    g_assert_true(qtest_memwrite_cpu(qts, node * 2,
                        peer + 0x20000, bytes, sizeof(bytes)));
                    bte_cpu_writeq(qts, node * 2, peer + 0x30000, UINT64_MAX);
                    g_assert_cmphex(bte_memory_transfer(qts, node, engine,
                        own + 0x10000, peer + 0x20000, peer + 0x30000,
                        zero, named),
                        ==, 0);
                    g_assert_true(qtest_memread_cpu(qts, (1 - node) * 2,
                        peer + 0x20000, result, sizeof(result)));
                    if (zero) {
                        memset(bytes, 0, sizeof(source));
                    } else {
                        memcpy(bytes, source, sizeof(source));
                    }
                    g_assert_cmpmem(result, sizeof(result),
                                    bytes, sizeof(bytes));
                    g_assert_cmphex(bte_cpu_readq(qts, (1 - node) * 2,
                                                  peer + 0x30000), ==, 0);
                    g_assert_true(qtest_memread_cpu(qts, node * 2,
                        own + 0x10000, result, sizeof(source)));
                    g_assert_cmpmem(result, sizeof(source),
                                    source, sizeof(source));
                    g_test_message("%s node%u BTE%u %s %s "
                                   "remote data/notification OK",
                                   topology, node, engine,
                                   named ? "named" : "local",
                                   zero ? "zero" : "copy");
                }
            }
        }
    }
    /* SELECT2 places the second fitted bank in its own sparse window. */
    for (unsigned node = 0; node < 2; node++) {
        uint64_t own = NODE(node ? 2 : 0);
        uint64_t peer = NODE(node ? 0 : 2);
        uint8_t source[128], marker[128], untouched[128];
        const uint64_t notify_marker = UINT64_C(0x123456789abcdef0);

        for (unsigned i = 0; i < sizeof(source); i++) {
            source[i] = (i * 13) ^ (0x96 + node);
        }
        memset(marker, 0xc0 + node, sizeof(marker));
        memset(untouched, 0x32 + node, sizeof(untouched));
        g_assert_true(qtest_memwrite_cpu(qts, node * 2,
            own + MD_BANK1_BASE + 0x50000, source, sizeof(source)));
        g_assert_true(qtest_memwrite_cpu(qts, node * 2,
            own + 0x50000, untouched, sizeof(untouched)));
        g_assert_true(qtest_memwrite_cpu(qts, (1 - node) * 2,
            peer + 0x60000, untouched, sizeof(untouched)));
        bte_cpu_writeq(qts, (1 - node) * 2, peer + 0x70000,
                       notify_marker);
        for (unsigned engine = 0; engine < 2; engine++) {
            for (unsigned named = 0; named < 2; named++) {
                for (unsigned zero = 0; zero < 2; zero++) {
                    g_assert_true(qtest_memwrite_cpu(qts, (1 - node) * 2,
                        peer + MD_BANK1_BASE + 0x60000,
                        marker, sizeof(marker)));
                    bte_cpu_writeq(qts, (1 - node) * 2,
                        peer + MD_BANK1_BASE + 0x70000, UINT64_MAX);
                    g_assert_cmphex(bte_memory_transfer(qts, node, engine,
                        own + MD_BANK1_BASE + 0x50000,
                        peer + MD_BANK1_BASE + 0x60000,
                        peer + MD_BANK1_BASE + 0x70000, zero, named), ==, 0);
                    g_assert_true(qtest_memread_cpu(qts, (1 - node) * 2,
                        peer + MD_BANK1_BASE + 0x60000,
                        result, sizeof(source)));
                    if (zero) {
                        memset(bytes, 0, sizeof(source));
                    } else {
                        memcpy(bytes, source, sizeof(source));
                    }
                    g_assert_cmpmem(result, sizeof(source),
                                    bytes, sizeof(source));
                    g_assert_cmphex(bte_cpu_readq(qts, (1 - node) * 2,
                        peer + MD_BANK1_BASE + 0x70000), ==, 0);
                    g_assert_true(qtest_memread_cpu(qts, node * 2,
                        own + MD_BANK1_BASE + 0x50000, result, sizeof(source)));
                    g_assert_cmpmem(result, sizeof(source),
                                    source, sizeof(source));
                    g_assert_true(qtest_memread_cpu(qts, node * 2,
                        own + 0x50000, result, sizeof(untouched)));
                    g_assert_cmpmem(result, sizeof(untouched),
                                    untouched, sizeof(untouched));
                    g_assert_true(qtest_memread_cpu(qts, (1 - node) * 2,
                        peer + 0x60000, result, sizeof(untouched)));
                    g_assert_cmpmem(result, sizeof(untouched),
                                    untouched, sizeof(untouched));
                    g_assert_cmphex(bte_cpu_readq(qts, (1 - node) * 2,
                        peer + 0x70000), ==, notify_marker);
                    g_test_message("%s node%u BTE%u %s sparse bank1 %s OK",
                                   topology, node, engine,
                                   named ? "named" : "local",
                                   zero ? "zero" : "copy");
                }
            }
        }
    }
    /* Named registers enter the routed view before launching DMA. */
    for (unsigned node = 0; node < 2; node++) {
        uint64_t own = NODE(node ? 2 : 0);
        uint64_t status, notification;

        memset(bytes, 0x5a, sizeof(bytes));
        g_assert_true(qtest_memwrite_cpu(qts, node * 2, own + 0x20000,
                                         bytes, sizeof(bytes)));
        bte_cpu_writeq(qts, node * 2, own + 0x30000, UINT64_MAX);
        status = bte_memory_transfer(qts, node, 0, own + 0x10000,
                                    own + 0x20000, own + 0x30000,
                                    false, true);
        notification = bte_cpu_readq(qts, node * 2, own + 0x30000);
        g_test_message("%s node%u named-register local copy status 0x%"
                       PRIx64 " notification 0x%" PRIx64,
                       topology, node, status, notification);
        g_assert_cmphex(status, ==, 0);
        g_assert_cmphex(notification, ==, 0);
        g_assert_true(qtest_memread_cpu(qts, node * 2, own + 0x20000,
                                        result, sizeof(result)));
        memset(bytes, 0x80 + node, 128);
        g_assert_cmpmem(result, sizeof(result), bytes, sizeof(bytes));
    }
    check_remote_pi_interrupt(qts, true);
    check_remote_bte_interrupt(qts, true);
    /* A disabled bank and the fitted bank's hole both preserve the target. */
    for (unsigned node = 0; node < 2; node++) {
        uint64_t own = NODE(node ? 2 : 0);
        uint64_t peer = NODE(node ? 0 : 2);
        uint8_t marker[128];
        const uint64_t failed = (UINT64_C(1) << 16) | 1;

        memset(marker, 0x5c + node, sizeof(marker));
        g_assert_true(qtest_memwrite_cpu(qts, (1 - node) * 2,
            peer + MD_BANK1_BASE + 0x60000, marker, sizeof(marker)));
        bte_cpu_writeq(qts, (1 - node) * 2,
            BEDROCK_LOCAL + MD_MEMORY_CONFIG, MD_BANK0_SELECT2);
        memset(bytes, 0x32 + node, 128);
        g_assert_true(qtest_memread_cpu(qts, (1 - node) * 2,
            peer + 0x60000, result, 128));
        g_assert_cmpmem(result, 128, bytes, 128);
        for (unsigned named = 0; named < 2; named++) {
            bte_cpu_writeq(qts, node * 2,
                own + MD_BANK1_BASE + 0x70000, UINT64_MAX);
            g_assert_cmphex(bte_memory_transfer(qts, node, 0,
                own + MD_BANK1_BASE + 0x50000,
                peer + MD_BANK1_BASE + 0x60000,
                own + MD_BANK1_BASE + 0x70000, false, named), ==, failed);
            g_assert_cmphex(bte_cpu_readq(qts, node * 2,
                own + MD_BANK1_BASE + 0x70000), ==, failed);
        }
        bte_cpu_writeq(qts, (1 - node) * 2,
            BEDROCK_LOCAL + MD_MEMORY_CONFIG, MD_INSTALLED_SELECT2);
        g_assert_true(qtest_memread_cpu(qts, (1 - node) * 2,
            peer + MD_BANK1_BASE + 0x60000, result, sizeof(marker)));
        g_assert_cmpmem(result, sizeof(marker), marker, sizeof(marker));
        for (unsigned named = 0; named < 2; named++) {
            bte_cpu_writeq(qts, node * 2,
                own + MD_BANK1_BASE + 0x70000, UINT64_MAX);
            g_assert_cmphex(bte_memory_transfer(qts, node, 0,
                own + MD_BANK1_BASE + 0x10050000,
                peer + MD_BANK1_BASE + 0x60000,
                own + MD_BANK1_BASE + 0x70000, false, named), ==, failed);
            g_assert_cmphex(bte_cpu_readq(qts, node * 2,
                own + MD_BANK1_BASE + 0x70000), ==, failed);
            g_assert_true(qtest_memread_cpu(qts, (1 - node) * 2,
                peer + MD_BANK1_BASE + 0x60000, result, sizeof(marker)));
            g_assert_cmpmem(result, sizeof(marker), marker, sizeof(marker));
        }
    }
    /* A missing target bank must refuse data, preserving backing RAM. */
    memset(bytes, 0x5a, sizeof(bytes));
    g_assert_true(qtest_memwrite_cpu(qts, 2, NODE(2) + 0x20000,
                                     bytes, sizeof(bytes)));
    bte_cpu_writeq(qts, 2, BEDROCK_LOCAL + MD_MEMORY_CONFIG, 0);
    for (unsigned named = 0; named < 2; named++) {
        g_assert_cmphex(bte_memory_transfer(qts, 0, 0, 0x10000,
            NODE(2) + 0x20000, 0x30000, true, named),
            ==, (UINT64_C(1) << 16) | 1);
        g_assert_cmphex(bte_cpu_readq(qts, 0, 0x30000), ==,
                        (UINT64_C(1) << 16) | 1);
    }
    bte_cpu_writeq(qts, 2, BEDROCK_LOCAL + MD_MEMORY_CONFIG,
                   MD_INSTALLED_SELECT2);
    g_assert_true(qtest_memread_cpu(qts, 2, NODE(2) + 0x20000,
                                    result, sizeof(result)));
    g_assert_cmpmem(result, sizeof(result), bytes, sizeof(bytes));
    qtest_qmp_assert_success(qts, "{'execute':'qom-set','arguments':{"
        "'path':%s,'property':'down','value':true}}",
        routed ? "/machine/master-route" : CABLE_PATH);
    check_remote_pi_interrupt(qts, false);
    check_remote_bte_interrupt(qts, false);
    for (unsigned node = 0; node < 2; node++) {
        uint64_t own = NODE(node ? 2 : 0);
        uint64_t peer = NODE(node ? 0 : 2);
        uint8_t marker[128];
        const uint64_t failed = (UINT64_C(1) << 16) | 1;

        memset(marker, 0x5c + node, sizeof(marker));
        for (unsigned named = 0; named < 2; named++) {
            bte_cpu_writeq(qts, node * 2,
                own + MD_BANK1_BASE + 0x70000, UINT64_MAX);
            g_assert_cmphex(bte_memory_transfer(qts, node, 0,
                own + MD_BANK1_BASE + 0x50000,
                peer + MD_BANK1_BASE + 0x60000,
                own + MD_BANK1_BASE + 0x70000, false, named), ==, failed);
            g_assert_cmphex(bte_cpu_readq(qts, node * 2,
                own + MD_BANK1_BASE + 0x70000), ==, failed);
            g_assert_true(qtest_memread_cpu(qts, (1 - node) * 2,
                peer + MD_BANK1_BASE + 0x60000, result, sizeof(marker)));
            g_assert_cmpmem(result, sizeof(marker), marker, sizeof(marker));
        }
    }
    for (unsigned named = 0; named < 2; named++) {
        g_assert_cmphex(bte_memory_transfer(qts, 0, 0, 0x10000,
            NODE(2) + 0x20000, 0x30000, true, named),
            ==, (UINT64_C(1) << 16) | 1);
        g_assert_cmphex(bte_cpu_readq(qts, 0, 0x30000), ==,
                        (UINT64_C(1) << 16) | 1);
    }
    g_assert_true(qtest_memread_cpu(qts, 2, NODE(2) + 0x20000,
                                    result, sizeof(result)));
    g_assert_cmpmem(result, sizeof(result), bytes, sizeof(bytes));
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(image), ==, 0);
}

static void test_bte_data_goes_by_number(void)
{
    QTestState *qts = machine_start();

    set_node_number(qts, 1, 5);
    qtest_writeq(qts, NODE(5) + UINT64_C(0x3000), WORD1);
    bte_zero_line(qts, NODE(5) + UINT64_C(0x3000), 0, 8);
    g_assert_cmphex(cpu_read_ok(qts, 2, NODE(5) + UINT64_C(0x3000)), ==, 0);
    g_assert_cmphex(cpu_read_ok(qts, 0, PROBE), ==, WORD0);

    qtest_writeq(qts, NODE(5) + UINT64_C(0x800), WORD1);
    qtest_writeq(qts, UINT64_C(0x800), WORD0);
    /* Size code 1, 4 KiB, on node 1's interface 0. */
    peer_access(qts, PI_CACHED_ALIAS, true, 1);
    g_assert_cmphex(cpu_read_ok(qts, 2, UINT64_C(0x800)), ==, WORD1);
    peer_bte_zero_line(qts, UINT64_C(0x800));
    g_assert_cmphex(cpu_read_ok(qts, 0, UINT64_C(0x800)), ==, 0);
    g_assert_cmphex(cpu_read_ok(qts, 2, UINT64_C(0x800)), ==, WORD1);
    qtest_quit(qts);
}

/*
 * The reset view and the local boot view are reader-relative forms of the
 * reader's own node's flash. Each node gets its own 16 MiB boot flash; node
 * 1's carries a marker at position zero, node 0's is blank.
 */
#define PROM_RESET UINT64_C(0x1fc00000)
#define HUB_LOCAL_BOOT (SPACE_HUB + UINT64_C(0x18000000))
#define PROM_MARKER UINT64_C(0x5e7e0001c0de0001)
#define BOOT_FLASH_SIZE (16 * 1024 * 1024)

static char *create_flash(uint64_t marker)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(BOOT_FLASH_SIZE);
    char *path = NULL;
    int fd;

    stq_be_p(image, marker);
    fd = g_file_open_tmp("sn1-cross-node-flash-XXXXXX.raw", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, BOOT_FLASH_SIZE), ==, BOOT_FLASH_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

/* Both nodes boot from their own flash, so no -bios. */
static QTestState *machine_start_marked(char **flashes)
{
    flashes[0] = create_flash(0);
    flashes[1] = create_flash(PROM_MARKER);
    return qtest_initf(MACHINE_ARGS " "
                       "-drive if=pflash,index=0,format=raw,file=%s "
                       "-drive if=pflash,index=1,format=raw,file=%s",
                       flashes[0], flashes[1]);
}

static void machine_quit_marked(QTestState *qts, char **flashes)
{
    qtest_quit(qts);
    for (unsigned int i = 0; i < 2; i++) {
        g_unlink(flashes[i]);
        g_free(flashes[i]);
    }
}

static void test_reset_view_is_each_nodes_own(void)
{
    char *flashes[2];
    QTestState *qts = machine_start_marked(flashes);
    unsigned int round;

    /* Before and after node 1 takes a number other than node 0's. */
    for (round = 0; round < 2; round++) {
        g_assert_cmphex(cpu_read_ok(qts, 2, PROM_RESET), ==, PROM_MARKER);
        g_assert_cmphex(cpu_read_ok(qts, 3, PROM_RESET), ==, PROM_MARKER);
        g_assert_cmphex(cpu_read_ok(qts, 0, PROM_RESET), ==, 0);
        g_assert_cmphex(cpu_read_ok(qts, 1, PROM_RESET), ==, 0);
        set_node_number(qts, 1, 1);
    }
    machine_quit_marked(qts, flashes);
}

static void test_local_boot_view_is_the_readers(void)
{
    char *flashes[2];
    QTestState *qts = machine_start_marked(flashes);

    set_node_number(qts, 1, 1);
    g_assert_cmphex(cpu_read_ok(qts, 2, HUB_LOCAL_BOOT), ==, PROM_MARKER);
    g_assert_cmphex(cpu_read_ok(qts, 0, HUB_LOCAL_BOOT), ==, 0);
    machine_quit_marked(qts, flashes);
}

/*
 * The L1 UART sits in the local register window at +0x80, a reader-relative
 * form: each node's processors reach their own chassis's L1, processor
 * interface 1's included, whose own register overlay leaves that range to the
 * form beneath it. The UART's scratch register tells the two apart: node 0's
 * is written through processor 0's local form, node 1's through node field 1
 * once node 1 carries that number.
 */
#define L1_UART_SCRATCH (HUB_LOCAL_REGISTERS + UINT64_C(0x80) + UINT64_C(0x38))

static void test_l1_uart_local_form_is_the_readers(void)
{
    QTestState *qts = machine_start();

    set_node_number(qts, 1, 1);
    qtest_writeq(qts, L1_UART_SCRATCH, 0x11);
    qtest_writeq(qts, NODE(1) + L1_UART_SCRATCH, 0x22);
    g_assert_cmphex(cpu_read_ok(qts, 0, L1_UART_SCRATCH) & 0xff, ==, 0x11);
    g_assert_cmphex(cpu_read_ok(qts, 1, L1_UART_SCRATCH) & 0xff, ==, 0x11);
    g_assert_cmphex(cpu_read_ok(qts, 2, L1_UART_SCRATCH) & 0xff, ==, 0x22);
    g_assert_cmphex(cpu_read_ok(qts, 3, L1_UART_SCRATCH) & 0xff, ==, 0x22);
    qtest_quit(qts);
}

/*
 * The PROM's cache-as-RAM phase keeps the PROM's RAM-copy range and the data
 * window reader-local; it has to end at the OS handoff even for a processor
 * that never touches the data window again. Otherwise, once the nodes carry
 * different numbers, a processor on node 1 storing to node field zero in
 * either range still lands in its own node's memory instead of node 0's.
 *
 * Guest code does it: every processor reads a node tag from its own memory;
 * node 0's processors park, node 1's enter the phase exactly as the PROM does
 * (boot-status bit 1 in FPR 0 and a cached load in the data window, running
 * from the flash), withdraw both, and jump to a RAM loop that touches the
 * window only with its uncached marker store. There they report ready and
 * wait for the host to renumber node 1 and say go, then store a marker with
 * node field zero at the RAM-copy address and, uncached, in the data window.
 */
#define HANDOFF_CODE UINT64_C(0x200000)
#define HANDOFF_FLAGS UINT64_C(0x300000)
#define HANDOFF_NODE_TAG (HANDOFF_FLAGS + 0x0)
#define HANDOFF_GO (HANDOFF_FLAGS + 0x8)
#define HANDOFF_DONE (HANDOFF_FLAGS + 0x10)
#define HANDOFF_READY (HANDOFF_FLAGS + 0x18)
#define PROM_RAM_COPY UINT64_C(0x1a00000)
#define PROM_DATA_WINDOW UINT64_C(0x100000)
#define HANDOFF_MARKER UINT64_C(0x5a5a5a5a)

static char *create_handoff_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    stl_be_p(image + 0x004, 0x00000000); /* nop */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff); /* beq zero, zero, self */
    }
    /* Status: coprocessor 1 usable and 64-bit kernel addressing. */
    stl_be_p(image + 0x400, 0x400c6000); /* mfc0 t4, Status */
    stl_be_p(image + 0x404, 0x3c0d2000); /* lui t5, 0x2000 -- CU1 */
    stl_be_p(image + 0x408, 0x018d6025); /* or t4, t4, t5 */
    stl_be_p(image + 0x40c, 0x358c0080); /* ori t4, t4, Status.KX */
    stl_be_p(image + 0x410, 0x408c6000); /* mtc0 t4, Status */
    stl_be_p(image + 0x414, 0x00000000); /* nop */
    stl_be_p(image + 0x418, 0x00000000); /* nop */
    /* The node tag, uncached, from this processor's own node. */
    stl_be_p(image + 0x41c, 0x3c08a030); /* lui t0, 0xa030 */
    stl_be_p(image + 0x420, 0x8d090000); /* lw t1, 0(t0) */
    stl_be_p(image + 0x424, 0x11200015); /* beq t1, zero, 0x47c -- node 0 */
    stl_be_p(image + 0x428, 0x00000000); /* nop */
    /* Enter the phase: bit 1 in FPR 0, then a cached load in the window. */
    stl_be_p(image + 0x42c, 0x340a0002); /* ori t2, zero, 2 */
    stl_be_p(image + 0x430, 0x448a0000); /* mtc1 t2, $f0 */
    stl_be_p(image + 0x434, 0x3c0ba800); /* lui t3, 0xa800 -- XKPHYS cached */
    stl_be_p(image + 0x438, 0x000b583c); /* dsll32 t3, t3, 0 */
    stl_be_p(image + 0x43c, 0x3c0c0010); /* lui t4, 0x0010 -- the window */
    stl_be_p(image + 0x440, 0x016c5825); /* or t3, t3, t4 */
    stl_be_p(image + 0x444, 0xdd6d0000); /* ld t5, 0(t3) */
    stl_be_p(image + 0x448, 0x00000000); /* nop */
    /* Hand off: clear the bit and leave the flash for RAM. */
    stl_be_p(image + 0x44c, 0x44800000); /* mtc1 zero, $f0 */
    stl_be_p(image + 0x450, 0x3c08a020); /* lui t0, 0xa020 */
    stl_be_p(image + 0x454, 0x01000008); /* jr t0 */
    stl_be_p(image + 0x458, 0x00000000); /* nop */
    stl_be_p(image + 0x47c, 0x1000ffff); /* beq zero, zero, self */
    stl_be_p(image + 0x480, 0x00000000); /* nop */

    fd = g_file_open_tmp("sn1-cross-node-handoff-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

/* The RAM loop, uncached: report ready, wait for go, store, report done. */
static const uint32_t handoff_loop[] = {
    0x3c08a030, /* lui t0, 0xa030 */
    0x34090001, /* ori t1, zero, 1 */
    0xad090018, /* sw t1, 0x18(t0) -- ready */
    0x8d0a0008, /* lw t2, 8(t0) -- go */
    0x1140fffe, /* beq t2, zero, -2 */
    0x00000000, /* nop */
    0x3c0ba1a0, /* lui t3, 0xa1a0 -- the RAM-copy range */
    0x3c0c5a5a, /* lui t4, 0x5a5a */
    0x358c5a5a, /* ori t4, t4, 0x5a5a */
    0xad6c0000, /* sw t4, 0(t3) */
    0x3c0ba010, /* lui t3, 0xa010 -- the data window */
    0xad6c0000, /* sw t4, 0(t3) */
    0xad090010, /* sw t1, 0x10(t0) -- done */
    0x1000ffff, /* beq zero, zero, self */
    0x00000000, /* nop */
};

static void write_handoff_loop(QTestState *qts, uint64_t base)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(handoff_loop); i++) {
        qtest_writel(qts, base + HANDOFF_CODE + 4 * i, handoff_loop[i]);
    }
}

static void test_prom_copy_range_leaves_at_handoff(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    QTestState *qts;
    uint64_t value = 0;
    SgiWait w;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    prom = create_handoff_prom();
    quoted = g_shell_quote(prom);
    qts = qtest_initf(MACHINE_ARGS " -bios %s -accel tcg,thread=multi "
                      "-no-shutdown -S", quoted);
    qtest_writeq(qts, BEDROCK_LOCAL + MD_MEMORY_CONFIG, MD_INSTALLED_SELECT2);
    peer_access(qts, MD_MEMORY_CONFIG, true, MD_INSTALLED_SELECT2);
    write_handoff_loop(qts, 0);
    qtest_writeq(qts, HANDOFF_NODE_TAG, 0);
    qtest_writeq(qts, HANDOFF_GO, 0);
    qtest_writeq(qts, PROM_RAM_COPY, 0);
    qtest_writeq(qts, PROM_DATA_WINDOW, 0);
    set_node_number(qts, 1, 1);
    write_handoff_loop(qts, NODE(1));
    qtest_writeq(qts, NODE(1) + HANDOFF_NODE_TAG, UINT64_C(1) << 32);
    qtest_writeq(qts, NODE(1) + HANDOFF_GO, 0);
    qtest_writeq(qts, NODE(1) + HANDOFF_READY, 0);
    qtest_writeq(qts, NODE(1) + PROM_RAM_COPY, 0);
    qtest_writeq(qts, NODE(1) + PROM_DATA_WINDOW, 0);
    set_node_number(qts, 1, 0);

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&w, "handoff ready");
    for (;;) {
        if (cpu_readq(qts, 2, HANDOFF_READY, &value) && (value >> 32) == 1) {
            break;
        }
        sgi_wait_pause(&w, 10000);
    }
    g_assert_cmphex(value >> 32, ==, 1);
    /* Give the handoff time to be noticed: many of its poll periods. */
    g_usleep(500000);

    set_node_number(qts, 1, 1);
    qtest_writeq(qts, HANDOFF_GO, UINT64_C(1) << 32);
    sgi_wait_start(&w, "handoff done");
    for (;;) {
        if ((qtest_readq(qts, HANDOFF_DONE) >> 32) == 1) {
            break;
        }
        sgi_wait_pause(&w, 10000);
    }
    g_assert_cmphex(qtest_readq(qts, HANDOFF_DONE) >> 32, ==, 1);
    g_assert_cmphex(qtest_readq(qts, PROM_DATA_WINDOW) >> 32, ==,
                    HANDOFF_MARKER);
    g_assert_cmphex(qtest_readq(qts, NODE(1) + PROM_DATA_WINDOW), ==, 0);
    g_assert_cmphex(qtest_readq(qts, PROM_RAM_COPY) >> 32, ==, HANDOFF_MARKER);
    g_assert_cmphex(qtest_readq(qts, NODE(1) + PROM_RAM_COPY), ==, 0);
    qtest_quit(qts);
}


#define COPY_PROBE_MARKER UINT64_C(0x6c6c6c6c)
#define COPY_OTHER_MARKER UINT64_C(0x7d7d7d7d)
#define COPY_PROBE_READBACK (HANDOFF_FLAGS + 0x20)
#define COPY_PROBE_MAPPING (HANDOFF_FLAGS + 0x28)
#define COPY_PROBE_MASK (HANDOFF_FLAGS + 0x30)
#define COPY_PROBE_ASID (HANDOFF_FLAGS + 0x38)
#define COPY_PROBE_RELEASE (HANDOFF_FLAGS + 0x40)

/* Keep FPR 0 set until the mapped copy clears it itself. */
static char *create_copy_probe_prom(void)
{
    char *path = create_handoff_prom();
    uint8_t nop[4] = { 0 };
    uint8_t jump[4];
    const uint32_t select_cpu[] = {
        0x3c089200, 0x0008403c, 0x3c090100, 0x01094025,
        0xdd0a0020, /* local PI_CPU_NUMBER */
        0x15400005, 0x00000000, /* park every nonzero slot */
        0x3c08a030, 0x8d090000, 0x1000ffbf, 0x00000000,
        0x1000ffff, 0x00000000,
    };
    int fd = open(path, O_WRONLY);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pwrite(fd, nop, sizeof(nop), 0x44c), ==, sizeof(nop));
    stl_be_p(jump, 0x3c08a1a1); /* bootstrap in the reader-local copy */
    g_assert_cmpint(pwrite(fd, jump, sizeof(jump), 0x450), ==, sizeof(jump));
    stl_be_p(jump, 0x10000038); /* select a processor after enabling KX */
    g_assert_cmpint(pwrite(fd, jump, sizeof(jump), 0x41c), ==, sizeof(jump));
    g_assert_cmpint(pwrite(fd, nop, sizeof(nop), 0x420), ==, sizeof(nop));
    for (unsigned int i = 0; i < ARRAY_SIZE(select_cpu); i++) {
        stl_be_p(jump, select_cpu[i]);
        g_assert_cmpint(pwrite(fd, jump, sizeof(jump), 0x500 + 4 * i),
                        ==, sizeof(jump));
    }
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void write_copy_probe(QTestState *qts, uint64_t base, uint32_t marker)
{
    const uint32_t code[] = {
        0x44800000, /* mtc1 zero, f0 */
        0x3c0ba800, 0x000b583c, 0x3c0c0010, 0x016c5825,
        0x3c0d0000 | (marker >> 16),
        0x35ad0000 | (marker & 0xffff),
        0xad6d0000, /* sw t5, 0(t3): cached private window */
        marker == COPY_OTHER_MARKER ? 0x340e0000 : 0x8d6e0000,
        /* A released node-zero read can fault in the remote case. */
        0xad0e0020, /* report readback */
        0xad0d0010, /* report distinct code marker */
        0x8d090040, 0x1120fffe, 0x00000000, /* wait for release */
        0x01e00008, 0x00000000,
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(code); i++) {
        qtest_writel(qts, base + PROM_RAM_COPY + 4 * i, code[i]);
    }
}

static void test_prom_copy_live_node(gconstpointer opaque)
{
    unsigned int named = GPOINTER_TO_UINT(opaque);
    unsigned int own = named == 3 ? 2 : named;
    unsigned int other = named == 3 ? 3 : 0;
    uint32_t marker = named == 3 ? COPY_OTHER_MARKER : COPY_PROBE_MARKER;
    uint64_t entrylo = (NODE(named) + PROM_RAM_COPY) / 64 + 0x1e;
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    QTestState *qts;
    uint64_t value = 0;
    const uint32_t bootstrap[] = {
        0x3c08a030, 0x34090001, 0xad090018, /* ready, still own local RAM */
        0x3c08a1a1,
        0x8d0a1000, 0x1140fffe, 0x00000000, /* wait in local copy */
        0x3c0b0000 | (entrylo >> 16),
        0x356b0000 | (entrylo & 0xffff),
        0x408b1000, /* EntryLo0 */
        0x656c4000, 0x408c1800, /* EntryLo1: next 1 MiB page */
        0x3c0c001f, 0x358ce000, 0x408c2800, /* PageMask 0x1fe000 */
        0x3c0dc000, 0x000d683c, 0x3c0e1000, 0x01ae6825,
        0x65ae0004, 0x40ae5000, /* EntryHi, ASID 4 */
        0x40800000, 0x000000c0, 0x42000002, 0x000000c0,
        0x3c089600, 0x0008403c,
        0x34090000 | own, 0x0009487c, /* node at bit 33 */
        0x01094025, 0x3c090030, 0x01094025, /* own uncached flags */
        0xfd0b0028, 0xfd0c0030, 0xfd0e0038, /* expose TLB inputs */
        0x3c090010, 0x0109782f, 0x65ef0200, /* own release text */
        0x400a6000, 0x240bfffb, 0x014b5024, 0x408a6000, /* clear ERL */
        0x000000c0, 0x01a00008, 0x00000000,
    };
    const uint32_t release[] = {
        0x3c0ba800, 0x000b583c, 0x3c0c0010, 0x016c5825,
        0x340d0042, 0xad6d0000, /* outside copy: write reaches backing RAM */
        0x34090001, 0xad090048, 0x1000ffff, 0x00000000,
    };

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    prom = create_copy_probe_prom();
    quoted = g_shell_quote(prom);
    qts = qtest_initf(MACHINE_ARGS " -bios %s -accel tcg,thread=multi "
                      "-no-shutdown -S", quoted);
    qtest_writeq(qts, BEDROCK_LOCAL + MD_MEMORY_CONFIG, MD_INSTALLED_SELECT2);
    peer_access(qts, MD_MEMORY_CONFIG, true, MD_INSTALLED_SELECT2);
    qtest_writeq(qts, HANDOFF_NODE_TAG, 0);
    qtest_writeq(qts, PROM_DATA_WINDOW, WORD0);
    write_copy_probe(qts, 0, COPY_OTHER_MARKER);
    set_node_number(qts, 1, 1);
    qtest_writeq(qts, NODE(1) + HANDOFF_NODE_TAG, UINT64_C(1) << 32);
    qtest_writeq(qts, NODE(1) + PROM_DATA_WINDOW, WORD1);
    write_copy_probe(qts, NODE(1), COPY_PROBE_MARKER);
    for (unsigned int i = 0; i < ARRAY_SIZE(bootstrap); i++) {
        qtest_writel(qts, NODE(1) + PROM_RAM_COPY + 0x10000 + 4 * i,
                     bootstrap[i]);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(release); i++) {
        qtest_writel(qts, NODE(1) + HANDOFF_CODE + 0x200 + 4 * i, release[i]);
    }
    set_node_number(qts, 1, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    for (unsigned int tries = 0; tries < 500; tries++) {
        if (cpu_readq(qts, 2, HANDOFF_READY, &value) && value >> 32 == 1) {
            break;
        }
        g_usleep(10000);
    }
    g_assert_cmphex(value >> 32, ==, 1);
    {
        g_autoptr(QDict) sibling = qtest_qmp(qts,
            "{'execute':'human-monitor-command', 'arguments':"
            "{'command-line':'info registers', 'cpu-index':3}}");
        g_assert_nonnull(strstr(qdict_get_str(sibling, "return"),
                               "pc=0xffffffffbfc0052c"));
    }
    set_node_number(qts, 0, other);
    set_node_number(qts, 1, own);
    g_assert_cmphex(peer_access(qts, LB_REVISION, false, 0) &
                    REVISION_NODE_MASK,
                    ==, (uint64_t)own << REVISION_NODE_SHIFT);
    g_assert_cmphex(qtest_readl(qts, NODE(own) + PROM_RAM_COPY), ==,
                    0x44800000);
    qtest_writel(qts, NODE(own) + PROM_RAM_COPY + 0x11000, 1);
    for (unsigned int tries = 0; tries < 500; tries++) {
        if (qtest_readl(qts, NODE(own) + HANDOFF_DONE) == marker) {
            break;
        }
        g_usleep(10000);
    }
    if (qtest_readl(qts, NODE(own) + HANDOFF_DONE) != marker) {
        g_autoptr(QDict) regs = qtest_qmp(qts,
            "{'execute':'human-monitor-command', 'arguments':"
            "{'command-line':'info registers', 'cpu-index':2}}");
        g_test_message("%s", qdict_get_str(regs, "return"));
    }
    g_assert_cmphex(qtest_readl(qts, NODE(own) + HANDOFF_DONE), ==, marker);
    g_assert_cmphex(qtest_readq(qts, NODE(own) + COPY_PROBE_MAPPING), ==,
                    entrylo);
    g_assert_cmphex(qtest_readq(qts, NODE(own) + COPY_PROBE_MASK), ==,
                    0x1fe000);
    g_assert_cmphex(qtest_readq(qts, NODE(own) + COPY_PROBE_ASID), ==,
                    UINT64_C(0xc000000010000004));
    g_test_message("named=%u own=%u readback=0x%x backing-own=0x%" PRIx64
                   " backing-other=0x%" PRIx64, named, own,
                   qtest_readl(qts, NODE(own) + COPY_PROBE_READBACK),
                   qtest_readq(qts, NODE(own) + PROM_DATA_WINDOW),
                   qtest_readq(qts, NODE(other) + PROM_DATA_WINDOW));
    g_assert_cmphex(qtest_readl(qts, NODE(own) + COPY_PROBE_READBACK), ==,
                    named == 3 ? 0 : marker);
    g_assert_cmphex(qtest_readq(qts, NODE(other) + PROM_DATA_WINDOW), ==,
                    WORD0);
    g_assert_cmphex(qtest_readq(qts, NODE(own) + PROM_DATA_WINDOW), ==,
                    named == 3 ? (uint64_t)marker << 32 | WORD1 : WORD1);
    if (named == 3) {
        qtest_quit(qts);
        g_unlink(prom);
        return;
    }
    qtest_writeq(qts, NODE(own) + COPY_PROBE_RELEASE, UINT64_C(1) << 32);
    for (unsigned int tries = 0; tries < 500; tries++) {
        if (qtest_readl(qts, NODE(own) + HANDOFF_FLAGS + 0x48) == 1) {
            break;
        }
        g_usleep(10000);
    }
    g_assert_cmphex(qtest_readl(qts, NODE(own) + HANDOFF_FLAGS + 0x48), ==, 1);
    g_assert_cmphex(qtest_readq(qts, NODE(own) + PROM_DATA_WINDOW), ==,
                    UINT64_C(0x42) << 32 | WORD1);
    qtest_quit(qts);
    g_unlink(prom);
}

/*
 * The directory records a processor on another node by that node's number:
 * a pointer is the node number above three device bits (MD_PTR_NODE_SHFT in
 * the public SN1 header), so node 1's slice A reads as 8. Node 1's first
 * processor names a line in a cache instruction and reads node 0's
 * directory word for it, which has node 0's memory unit follow it, loads
 * the line cached and reads the word again.
 * Both local blocks leave reset carrying number 0, and the compatibility
 * space names memory by number, so node 1 first reads its tag and reports
 * ready in its own memory, then waits for the host to number it 1 and say
 * go; after that the same addresses reach node 0, where it publishes.
 */
#define DIRECTORY_FLAGS UINT64_C(0x300000)
#define DIRECTORY_TAG (DIRECTORY_FLAGS + 0x0)
#define DIRECTORY_BEFORE (DIRECTORY_FLAGS + 0x20)
#define DIRECTORY_AFTER (DIRECTORY_FLAGS + 0x28)
#define DIRECTORY_DONE (DIRECTORY_FLAGS + 0x30)
#define DIRECTORY_GO (DIRECTORY_FLAGS + 0x38)
#define DIRECTORY_READY (DIRECTORY_FLAGS + 0x40)

static char *create_remote_directory_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    const uint32_t code[] = {
        mips_mfc0(12, 12),                   /* mfc0 t4, Status */
        mips_ori(12, 12, 0x80),              /* ori t4, t4, Status.KX */
        mips_mtc0(12, 12),                   /* mtc0 t4, Status */
        mips_nop(),                          /* nop */
        mips_lui(8, 0xa030),                 /* lui t0, 0xa030 -- own flags */
        mips_lw(9, 8, 0),                    /* lw t1, 0(t0) -- node tag */
        mips_branch(MIPS_OPC_BEQ, 9, 0, 34), /* beq t1, zero, spin */
        mips_nop(),
        mips_lui(12, 0x9200),                /* lui t4, 0x9200 */
        mips_dsll32(12, 12),                 /* dsll32 t4, t4, 0 */
        mips_lui(11, 0x0100),                /* lui t3, 0x0100 */
        mips_daddu(12, 12, 11),              /* daddu t4, t4, t3 */
        mips_ld(12, 12, 0x20),               /* ld t4, 0x20(t4) -- CPU number */
        mips_andi(12, 12, 3),                /* andi t4, t4, 3 */
        mips_branch(MIPS_OPC_BNE, 12, 0, 26), /* bne t4, zero, spin */
        mips_nop(),
        mips_ori(11, 0, 1),                  /* ori t3, zero, 1 */
        mips_sd(11, 8, 0x40),                /* sd t3, 0x40(t0) -- ready */
        mips_lw(9, 8, 0x38),                 /* go: lw t1, 0x38(t0) */
        mips_branch(MIPS_OPC_BEQ, 9, 0, -2), /* beq t1, zero, go */
        mips_nop(),
        mips_lui(10, 0xa800),                /* lui t2, 0xa800 */
        mips_dsll32(10, 10),                 /* dsll32 t2, t2, 0 */
        mips_lui(11, 0x0058),                /* lui t3, 0x0058 -- the line */
        mips_daddu(10, 10, 11),              /* daddu t2, t2, t3 */
        mips_lui(13, 0x9000),                /* lui t5, 0x9000 */
        mips_ori(13, 13, 0x0001),            /* ori t5, t5, 1 */
        mips_dsll32(13, 13),                 /* dsll32 t5, t5, 0 */
        mips_lui(11, 0xc00b),                /* lui t3, 0xc00b -- its word */
        mips_dsll32(11, 11),                 /* dsll32 t3, t3, 0 */
        mips_dsrl32(11, 11),                 /* dsrl32 t3, t3, 0 */
        mips_daddu(13, 13, 11),              /* daddu t5, t5, t3 */
        mips_cache(0x17, 10, 0),             /* cache HitWBInv(S), 0(t2) */
        mips_ld(11, 13, 0),                  /* ld t3, 0(t5) */
        mips_sd(11, 8, 0x20),                /* sd t3, 0x20(t0) */
        mips_ld(11, 10, 0),                  /* ld t3, 0(t2) -- cached load */
        mips_sync(),                         /* sync */
        mips_ld(11, 13, 0),                  /* ld t3, 0(t5) */
        mips_sd(11, 8, 0x28),                /* sd t3, 0x28(t0) */
        mips_ori(11, 0, 1),                  /* ori t3, zero, 1 */
        mips_sd(11, 8, 0x30),                /* sd t3, 0x30(t0) */
        0x1000ffff,                          /* spin: beq zero, zero, spin */
        mips_nop(),
    };
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff); /* beq zero, zero, self */
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(code); i++) {
        stl_be_p(image + 0x400 + 4 * i, code[i]);
    }
    fd = g_file_open_tmp("sn1-cross-node-directory-XXXXXX.bin", &path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_remote_reader_in_the_directory(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    QTestState *qts;
    uint64_t value = 0;
    SgiWait w;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    prom = create_remote_directory_prom();
    quoted = g_shell_quote(prom);
    qts = qtest_initf(MACHINE_ARGS " -bios %s -accel tcg,thread=multi "
                      "-no-shutdown -S", quoted);
    qtest_writeq(qts, BEDROCK_LOCAL + MD_MEMORY_CONFIG, MD_INSTALLED_SELECT2);
    peer_access(qts, MD_MEMORY_CONFIG, true, MD_INSTALLED_SELECT2);
    qtest_writeq(qts, DIRECTORY_TAG, 0);
    qtest_writeq(qts, DIRECTORY_GO, 0);
    qtest_writeq(qts, DIRECTORY_DONE, 0);
    set_node_number(qts, 1, 1);
    qtest_writeq(qts, NODE(1) + DIRECTORY_TAG, UINT64_C(1) << 32);
    qtest_writeq(qts, NODE(1) + DIRECTORY_GO, 0);
    qtest_writeq(qts, NODE(1) + DIRECTORY_READY, 0);
    set_node_number(qts, 1, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&w, "remote directory ready");
    for (;;) {
        if (cpu_readq(qts, 2, DIRECTORY_READY, &value) && value == 1) {
            break;
        }
        sgi_wait_pause(&w, 10000);
    }
    g_assert_cmphex(value, ==, 1);
    set_node_number(qts, 1, 1);
    qtest_writeq(qts, DIRECTORY_GO, UINT64_C(1) << 32);
    sgi_wait_start(&w, "remote directory done");
    for (;;) {
        if (qtest_readq(qts, DIRECTORY_DONE) == 1) {
            break;
        }
        sgi_wait_pause(&w, 10000);
    }
    g_assert_cmphex(qtest_readq(qts, DIRECTORY_DONE), ==, 1);
    g_assert_cmphex(qtest_readq(qts, DIRECTORY_BEFORE) & 0xff1f, ==, 0);
    g_assert_cmphex(qtest_readq(qts, DIRECTORY_AFTER) & 0xff1f,
                    ==, 0x0801);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

#define OPTION_CONTROL UINT64_C(0x1f000000)

static void option_control_write(QTestState *qts, uint64_t address,
                                  uint64_t value)
{
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/opt',"
        "'property':'offset','value':%" PRIu64 "}}", address - OPTION_CONTROL);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/opt',"
        "'property':'value','value':%" PRIu64 "}}", value);
}

static uint64_t option_control_read(QTestState *qts, uint64_t address)
{
    g_autoptr(QDict) reply = NULL;

    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/opt',"
        "'property':'offset','value':%" PRIu64 "}}", address - OPTION_CONTROL);
    reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/opt',"
        "'property':'value'}}");
    return qdict_get_int(reply, "return");
}

static uint32_t option_head_pins(QTestState *qts, unsigned node)
{
    uint8_t bytes[4];
    uint64_t base = SPACE_IO + NODE(node ? 2 : 0) + (UINT64_C(0xe) << 24);

    g_assert_true(qtest_memread_cpu(qts, node * 2, base + 0x104,
                                    bytes, sizeof(bytes)));
    return ldl_be_p(bytes) & 0xff;
}

static void option_pin(QTestState *qts, unsigned node, unsigned device,
                       unsigned pin, bool level)
{
    option_control_write(qts, OPTION_CONTROL + node * 0x1000 + 0x400 +
                 (device - 1) * 0x100 + pin * 0x40 + 32, level);
}

static QTestState *option_start(bool selected, char **image, const char *extra)
{
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("sn1-option-storage-XXXXXX", image, &error);
    QTestState *qts;

    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf(
        "-M origin300,topology=%s%s -cpu R14000 -smp 4 -m 1G -bios %s "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-device sgi-compute-option-intx-fixture,id=opt%s "
        "-nodefaults -display none %s",
        selected ? "origin300-numalink-pci-expansion-experiment" :
                   "origin300-two-chassis",
        selected ? ",expansion-storage.0=expansion-enclosure=cfg" : "",
        prom_path, *image, extra[0] == ',' ? extra : "",
        extra[0] == '-' ? extra : "");
    bte_cpu_writeq(qts, 2, BEDROCK_LOCAL + LB_REVISION, UINT64_C(2) << 32);
    return qts;
}

static void test_option_intx_pins(const void *data)
{
    static const unsigned map[2][4] = { { 1, 5, 1, 5 }, { 2, 6, 2, 6 } };
    bool selected = GPOINTER_TO_INT(data);
    g_autofree char *image = NULL;
    QTestState *qts = option_start(selected, &image, "");

    for (unsigned node = 0; node < 2; node++) {
        g_assert_cmpuint(option_control_read(qts,
                         OPTION_CONTROL + node * 0x1000),
                         ==, selected);
        for (unsigned device = 1; device <= 3; device++) {
            for (unsigned pin = 0; pin < 4; pin++) {
                uint32_t expected = selected && device <= 2 ?
                    1U << map[device - 1][pin] : 0;

                option_pin(qts, node, device, pin, true);
                g_assert_cmphex(option_head_pins(qts, node), ==, expected);
                g_assert_cmphex(option_head_pins(qts, node ^ 1), ==, 0);
                option_pin(qts, node, device, pin, false);
                g_assert_cmphex(option_head_pins(qts, node), ==, 0);
            }
        }
    }
    qtest_quit(qts);
    g_unlink(image);
}

static void test_option_intx_shared(void)
{
    g_autofree char *image = NULL;
    QTestState *qts = option_start(true, &image, "");

    for (unsigned node = 0; node < 2; node++) {
        for (unsigned device = 1; device <= 2; device++) {
            for (unsigned first = 0; first < 4; first++) {
                unsigned other = first ^ 2;
                unsigned input = device + ((first & 1) ? 4 : 0);

                option_pin(qts, node, device, first, true);
                option_pin(qts, node, device, other, true);
                option_pin(qts, node, device, first, false);
                g_assert_cmphex(option_head_pins(qts, node), ==, 1U << input);
                option_pin(qts, node, device, other, false);
                g_assert_cmphex(option_head_pins(qts, node), ==, 0);
            }
        }
    }
    qtest_quit(qts);
    g_unlink(image);
}

static void test_option_intx_restore(void)
{
    g_autofree char *image = NULL;
    QTestState *qts = option_start(true, &image, "");

    for (unsigned node = 0; node < 2; node++) {
        option_pin(qts, node, 1, 0, true);
        option_pin(qts, node, 1, 2, true);
        option_control_write(qts, OPTION_CONTROL + node * 0x1000 + 8, 0);
        g_assert_cmpuint(option_control_read(qts,
                         OPTION_CONTROL + node * 0x1000 + 8),
                         ==, 1);
        g_assert_cmphex(option_head_pins(qts, node), ==, 1U << 1);
        for (unsigned mismatch = 1; mismatch <= 3; mismatch++) {
            option_control_write(qts, OPTION_CONTROL + node * 0x1000 + 8,
                                 mismatch);
            g_assert_cmpuint(option_control_read(qts,
                OPTION_CONTROL + node * 0x1000 + 8), ==, 0);
        }
        option_pin(qts, node, 1, 0, false);
        g_assert_cmphex(option_head_pins(qts, node), ==, 1U << 1);
        option_pin(qts, node, 1, 2, false);
        g_assert_cmphex(option_head_pins(qts, node), ==, 0);
    }
    option_pin(qts, 0, 2, 1, true);
    option_pin(qts, 1, 2, 1, true);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_qmp_eventwait(qts, "RESET");
    bte_cpu_writeq(qts, 2, BEDROCK_LOCAL + LB_REVISION, UINT64_C(2) << 32);
    g_assert_cmphex(option_head_pins(qts, 0), ==, 0);
    g_assert_cmphex(option_head_pins(qts, 1), ==, 0);
    option_pin(qts, 0, 2, 1, true);
    g_assert_cmphex(option_head_pins(qts, 0), ==, 1U << 6);
    qtest_quit(qts);
    g_unlink(image);
}

static void test_option_intx_changed_map(void)
{
    g_autofree char *image = NULL;
    QTestState *qts = option_start(true, &image, ",alternate-map=true");

    option_pin(qts, 0, 1, 0, true);
    g_assert_cmphex(option_head_pins(qts, 0), ==, 1U << 3);
    option_pin(qts, 0, 1, 2, true);
    g_assert_cmphex(option_head_pins(qts, 0), ==, (1U << 1) | (1U << 3));
    option_pin(qts, 1, 1, 0, true);
    g_assert_cmphex(option_head_pins(qts, 1), ==, 1U << 1);
    option_pin(qts, 0, 1, 0, false);
    g_assert_cmphex(option_head_pins(qts, 0), ==, 1U << 1);
    option_pin(qts, 0, 1, 2, false);
    g_assert_cmphex(option_head_pins(qts, 0), ==, 0);
    qtest_quit(qts);
    g_unlink(image);
}

static void option_head_write(QTestState *qts, unsigned node, uint64_t offset,
                              uint32_t value)
{
    uint8_t bytes[4];
    uint64_t base = SPACE_IO + NODE(node ? 2 : 0) + (UINT64_C(0xe) << 24);

    stl_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, node * 2, base + offset + 4,
                                     bytes, sizeof(bytes)));
}

static void test_option_intx_masked(void)
{
    const uint64_t bit = UINT64_C(1) << 20;
    g_autofree char *image = NULL;
    QTestState *qts = option_start(true, &image, "");

    bte_cpu_writeq(qts, 0, BEDROCK_LOCAL + 0x681010, 5);
    bte_cpu_writeq(qts, 2, BEDROCK_LOCAL + 0x681000, 3);
    for (unsigned source = 0; source < 2; source++) {
        for (unsigned destination = 0; destination < 2; destination++) {
            option_head_write(qts, source, XBRIDGE_INT_UPPER,
                              XBRIDGE_HOST_PORT << 16);
            option_head_write(qts, source, XBRIDGE_INT_LOWER,
                              PI_REGISTERS + PI_PENDING_MODIFY);
            option_head_write(qts, source, XBRIDGE_INT_HOST(1),
                XBRIDGE_HOST_REGISTER |
                ((destination ? 2 : 0) << XBRIDGE_HOST_NODE_SHIFT) | 20);
            option_head_write(qts, source, 0x118, 2);
            option_head_write(qts, source, XBRIDGE_INT_ENABLE, 0);
            bte_cpu_writeq(qts, destination * 2, BEDROCK_LOCAL + PI_MASK0_A,
                           bit);
            option_pin(qts, source, 1, 0, true);
            g_assert_cmphex(option_head_pins(qts, source), ==, 2);
            g_assert_cmphex(pending0(qts, destination * 2) & bit, ==, 0);
            option_head_write(qts, source, XBRIDGE_INT_ENABLE, 2);
            g_assert_cmphex(pending0(qts, destination * 2) & bit, ==, 0);
            option_head_write(qts, source, 0x1c8, 1);
            g_assert_cmphex(pending0(qts, destination * 2) & bit, ==, bit);
            g_assert_cmphex(pending0(qts, (destination ^ 1) * 2) & bit, ==, 0);
            g_assert_cmphex(interrupt_cpu_cause(qts, destination * 2) &
                            CPU_CAUSE_IRQ0, ==, CPU_CAUSE_IRQ0);
            option_pin(qts, source, 1, 0, false);
            g_assert_cmphex(pending0(qts, destination * 2) & bit, ==, 0);
            g_assert_cmphex(interrupt_cpu_cause(qts, destination * 2) &
                            CPU_CAUSE_IRQ0, ==, 0);
            bte_cpu_writeq(qts, destination * 2, BEDROCK_LOCAL + PI_MASK0_A, 0);
        }
    }
    qtest_quit(qts);
    g_unlink(image);
}

static void test_option_intx_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-option-migration-XXXXXX",
                                         &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *from_image = NULL;
    g_autofree char *to_image = NULL;
    QTestState *from = option_start(true, &from_image, "");
    QTestState *to = option_start(true, &to_image, "-incoming defer");

    g_assert_no_error(error);
    for (unsigned node = 0; node < 2; node++) {
        option_pin(from, node, 1, 0, true);
        option_pin(from, node, 1, 2, true);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    for (unsigned node = 0; node < 2; node++) {
        g_assert_cmphex(option_head_pins(to, node), ==, 1U << 1);
        option_pin(to, node, 1, 0, false);
        g_assert_cmphex(option_head_pins(to, node), ==, 1U << 1);
        option_pin(to, node, 1, 2, false);
        g_assert_cmphex(option_head_pins(to, node), ==, 0);
    }
    qtest_quit(to);
    qtest_quit(from);
    g_unlink(from_image);
    g_unlink(to_image);
    g_unlink(socket);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    prom_path = create_prom();
    qtest_add_func("sn1/cross-node/remote-reader-in-the-directory",
                   test_remote_reader_in_the_directory);
    qtest_add_func("sn1/cross-node/equal-numbers-stay-local",
                   test_equal_numbers_stay_local);
    qtest_add_func("sn1/cross-node/numbered-nodes-reach-each-other",
                   test_numbered_nodes_reach_each_other);
    qtest_add_func("sn1/cross-node/renumbering-moves-the-claim",
                   test_renumbering_moves_the_claim);
    qtest_add_func("sn1/cross-node/local-forms-are-the-readers",
                   test_local_forms_are_the_readers);
    qtest_add_func("sn1/cross-node/cable-down-makes-the-peer-unreachable",
                   test_cable_down_makes_the_peer_unreachable);
    qtest_add_func("sn1/cross-node/prom-data-window-is-the-readers",
                   test_prom_data_window_is_the_readers);
    qtest_add_func("sn1/cross-node/cached-alias-bounds-the-readers-memory",
                   test_cached_alias_bounds_the_readers_memory);
    qtest_add_func("sn1/cross-node/bte-completion-goes-by-number",
                   test_bte_completion_goes_by_number);
    qtest_add_func("sn1/cross-node/reset-view-is-each-nodes-own",
                   test_reset_view_is_each_nodes_own);
    qtest_add_func("sn1/cross-node/local-boot-view-is-the-readers",
                   test_local_boot_view_is_the_readers);
    qtest_add_func("sn1/cross-node/l1-uart-local-form-is-the-readers",
                   test_l1_uart_local_form_is_the_readers);
    qtest_add_func("sn1/cross-node/prom-copy-range-leaves-at-handoff",
                   test_prom_copy_range_leaves_at_handoff);
    qtest_add_data_func("sn1/cross-node/prom-copy-placement-number",
                       GUINT_TO_POINTER(1), test_prom_copy_live_node);
    qtest_add_data_func("sn1/cross-node/prom-copy-live-number",
                       GUINT_TO_POINTER(2), test_prom_copy_live_node);
    qtest_add_data_func("sn1/cross-node/prom-copy-other-number",
                       GUINT_TO_POINTER(3), test_prom_copy_live_node);
    qtest_add_func("sn1/cross-node/remote-interrupt-reaches-the-peer",
                   test_remote_interrupt_reaches_the_peer);
    qtest_add_func("sn1/cross-node/routed-hub-special-decoding",
                   test_routed_hub_special_decoding);
    qtest_add_data_func("sn1/cross-node/bte-memory/direct",
                        "origin300-two-chassis", test_bte_memory_route);
    qtest_add_data_func("sn1/cross-node/bte-memory/routed",
                        "origin300-numalink-pci-expansion-experiment",
                        test_bte_memory_route);
    qtest_add_func("sn1/cross-node/bte-data-goes-by-number",
                   test_bte_data_goes_by_number);
    if (qtest_has_device("sgi-compute-option-intx-fixture")) {
        qtest_add_data_func("sn1/option-intx/selected-pins",
                            GINT_TO_POINTER(1), test_option_intx_pins);
        qtest_add_data_func("sn1/option-intx/unselected-pins",
                            GINT_TO_POINTER(0), test_option_intx_pins);
        qtest_add_func("sn1/option-intx/shared-pins", test_option_intx_shared);
        qtest_add_func("sn1/option-intx/restore", test_option_intx_restore);
        qtest_add_func("sn1/option-intx/migration", test_option_intx_migration);
        qtest_add_func("sn1/option-intx/masked", test_option_intx_masked);
        qtest_add_func("sn1/option-intx/changed-map",
                       test_option_intx_changed_map);
    }
    ret = g_test_run();
    g_unlink(prom_path);
    g_free(prom_path);
    return ret;
}
