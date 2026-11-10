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
#include <poll.h>
#include <sys/socket.h>
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qobject/qdict.h"
#include "sgi-test.h"
#include "sgi-wait.h"
#include "sgi-mips-emit.h"
#include "sgi-migrate.h"
#include "sgi-l1-test.h"

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

/*
 * The cross-partition probe: a kernel reads another node's directory slot with
 * a fault-tolerant copy and treats a fault as "not present". A read of a node
 * no number claims, or whose cable is down, must fail as a bus error at the
 * reader and leave every other access working; it must not stop the machine.
 */
static void test_remote_probe_faults_and_machine_runs_on(void)
{
    QTestState *qts = machine_start();
    g_autoptr(QDict) status = NULL;

    /* Node 1 holds number 0 until it is numbered: nothing is at NODE(1). */
    assert_unreachable(qts, 0, NODE(1) + PROBE);
    assert_unreachable(qts, 0, NODE(1) + PROBE + 8);
    g_assert_cmphex(cpu_read_ok(qts, 0, PROBE), ==, WORD0);
    set_node_number(qts, 1, 1);
    g_assert_cmphex(cpu_read_ok(qts, 0, NODE(1) + PROBE), ==, WORD1);
    qtest_qom_set_bool(qts, CABLE_PATH, "down", true);
    assert_unreachable(qts, 0, NODE(1) + PROBE);
    g_assert_cmphex(cpu_read_ok(qts, 0, PROBE), ==, WORD0);
    qtest_qom_set_bool(qts, CABLE_PATH, "down", false);
    g_assert_cmphex(cpu_read_ok(qts, 0, NODE(1) + PROBE), ==, WORD1);
    status = qtest_qmp(qts, "{'execute':'query-status'}");
    g_assert_true(qdict_haskey(status, "return"));
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
        "-M origin300,topology=origin300-numalink-pci-expansion,"
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
        g_assert_true(qtest_memwrite_cpu(qts, (1 - node) * 2,
            peer + 0x40000, source, sizeof(source)));
        memset(bytes, 0xa0 + node, sizeof(source));
        g_assert_true(qtest_memwrite_cpu(qts, node * 2,
            own + 0x50000, bytes, sizeof(source)));
        bte_cpu_writeq(qts, node * 2, own + 0x60000, UINT64_MAX);
        g_assert_cmphex(bte_memory_transfer(qts, node, 0,
            peer + 0x40000, own + 0x50000, own + 0x60000, false, false),
            ==, 0);
        g_assert_true(qtest_memread_cpu(qts, node * 2,
            own + 0x50000, result, sizeof(source)));
        g_assert_cmpmem(result, sizeof(source), source, sizeof(source));
        g_assert_cmphex(bte_cpu_readq(qts, node * 2,
            own + 0x60000), ==, 0);
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
 * A node without main power leaves every other node's map, whichever way
 * the fabric reaches it: processor reads, and block-copy engine reads and
 * writes, which have a low-priority alias of the whole system map under the
 * fabric's claims and would reach its RAM through it. The RAM keeps its
 * bytes, and power returning brings the node back unchanged. A model that
 * only refuses register accesses fails here.
 */
/*
 * What the PROM does for a node that starts cold: configure its memory unit
 * and, behind a router, its route to the other node. A node whose supply
 * returned starts cold and needs it again; its bytes do not (the model keeps
 * them, a stated approximation).
 */
static void unpowered_program_node(QTestState *qts, unsigned int node,
                                   bool routed)
{
    bte_cpu_writeq(qts, node * 2, BEDROCK_LOCAL + MD_MEMORY_CONFIG,
                   MD_INSTALLED_SELECT2);
    bte_cpu_writeq(qts, node * 2, BEDROCK_LOCAL + 0x400118, 0);
    if (routed) {
        bte_cpu_writeq(qts, node * 2,
                       BEDROCK_LOCAL + (node ? 0x681000 : 0x681010),
                       node ? 3 : 5);
    }
}

/*
 * Both nodes numbered (node 1 is 2) with their memory configured, one
 * pattern resident in node 1 and another at the base of node 0. The
 * expansion topology puts a router between them.
 */
static QTestState *unpowered_start(const char *topology, char **image,
                                   const char *extra)
{
    bool routed = strstr(topology, "expansion") != NULL;
    g_autoptr(GError) error = NULL;
    uint8_t source[128], resident[128];
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-unpowered-storage-XXXXXX", image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf(
        "-M origin300,topology=%s%s -cpu R14000 -smp 4 -m 1G -bios %s "
        "-drive if=none,id=cfg,file=%s,format=raw "
        "-nodefaults -display none -d guest_errors %s",
        topology, routed ? ",expansion-storage.0=expansion-enclosure=cfg" : "",
        prom_path, *image, extra);
    bte_cpu_writeq(qts, 2, BEDROCK_LOCAL + LB_REVISION, UINT64_C(2) << 32);
    unpowered_program_node(qts, 0, routed);
    unpowered_program_node(qts, 1, routed);
    memset(source, 0x80, sizeof(source));
    memset(resident, 0xa1, sizeof(resident));
    g_assert_true(qtest_memwrite_cpu(qts, 0, 0x10000, source,
                                     sizeof(source)));
    g_assert_true(qtest_memwrite_cpu(qts, 2, NODE(2) + 0x20000, resident,
                                     sizeof(resident)));
    g_assert_true(qtest_memwrite_cpu(qts, 2, NODE(2) + 0x40000, resident,
                                     sizeof(resident)));
    return qts;
}

/* What node 0 sees of node 1's memory, whichever way the fabric reaches. */
static void check_unpowered_memory(QTestState *qts)
{
    const uint64_t failed = (UINT64_C(1) << 16) | 1;
    const uint64_t peer = NODE(2);
    uint8_t resident[128], result[128];
    uint8_t untouched[128] = { 0 };

    memset(resident, 0xa1, sizeof(resident));
    assert_unreachable(qts, 0, peer + 0x20000);
    for (unsigned named = 0; named < 2; named++) {
        for (unsigned zero = 0; zero < 2; zero++) {
            /* The copy into the off node, and its zero fill, both fail. */
            bte_cpu_writeq(qts, 0, 0x30000, UINT64_MAX);
            g_assert_cmphex(bte_memory_transfer(qts, 0, 0, 0x10000,
                peer + 0x20000, 0x30000, zero, named), ==, failed);
            g_assert_true(qtest_memread_cpu(qts, 2, peer + 0x20000, result,
                                            sizeof(result)));
            g_assert_cmpmem(result, sizeof(result), resident,
                            sizeof(resident));
        }
        /* A copy out of the off node leaves its reader's buffer alone. */
        bte_cpu_writeq(qts, 0, 0x30000, UINT64_MAX);
        g_assert_cmphex(bte_memory_transfer(qts, 0, 0, peer + 0x40000,
            0x50000, 0x30000, false, named), ==, failed);
        g_assert_true(qtest_memread_cpu(qts, 0, 0x50000, result,
                                        sizeof(result)));
        g_assert_cmpmem(result, sizeof(result), untouched,
                        sizeof(untouched));
    }
}

/* Power back: reachable again, with the bytes it kept. */
static void check_repowered_memory(QTestState *qts)
{
    const uint64_t peer = NODE(2);
    uint8_t source[128], result[128];

    memset(source, 0x80, sizeof(source));
    g_assert_cmphex(cpu_read_ok(qts, 0, peer + 0x20000), ==,
                    UINT64_C(0xa1a1a1a1a1a1a1a1));
    bte_cpu_writeq(qts, 0, 0x30000, UINT64_MAX);
    g_assert_cmphex(bte_memory_transfer(qts, 0, 0, 0x10000, peer + 0x20000,
                                        0x30000, false, false), ==, 0);
    g_assert_true(qtest_memread_cpu(qts, 2, peer + 0x20000, result,
                                    sizeof(result)));
    g_assert_cmpmem(result, sizeof(result), source, sizeof(source));
}

static void test_unpowered_node_memory_is_absent(const void *data)
{
    g_autofree char *image = NULL;
    QTestState *qts = unpowered_start(data, &image, "");

    g_assert_cmphex(cpu_read_ok(qts, 0, NODE(2) + 0x20000), ==,
                    UINT64_C(0xa1a1a1a1a1a1a1a1));
    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 0);
    check_unpowered_memory(qts);
    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 1);
    /* The node starts cold: its number and memory unit are the PROM's. */
    bte_cpu_writeq(qts, 2, BEDROCK_LOCAL + LB_REVISION, UINT64_C(2) << 32);
    unpowered_program_node(qts, 1, strstr(data, "expansion") != NULL);
    check_repowered_memory(qts);
    qtest_quit(qts);
    g_assert_cmpint(unlink(image), ==, 0);
}

/*
 * An off node stays off across a migration: the destination derives the
 * unreachable memory from the loaded level, and power returning there
 * brings the node back.
 */
static void test_unpowered_node_survives_migration(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-unpowered-migration-XXXXXX",
                                         &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *from_image = NULL;
    g_autofree char *to_image = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    from = unpowered_start(data, &from_image, "");
    to = unpowered_start(data, &to_image, "-incoming defer");
    qtest_set_irq_in(from, "/machine/node1", "power", 0, 0);
    check_unpowered_memory(from);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    check_unpowered_memory(to);
    qtest_set_irq_in(to, "/machine/node1", "power", 0, 1);
    bte_cpu_writeq(to, 2, BEDROCK_LOCAL + LB_REVISION, UINT64_C(2) << 32);
    unpowered_program_node(to, 1, strstr(data, "expansion") != NULL);
    check_repowered_memory(to);
    qtest_quit(to);
    qtest_quit(from);
    g_unlink(from_image);
    g_unlink(to_image);
    g_unlink(socket);
    g_rmdir(dir);
}

/*
 * Migrate while the first node is off. This guards that path: the stream
 * must load and leave the first node's memory absent from both nodes' views.
 * It does not cover device load order. The re-derive that
 * bedrock_power_rederive() does in post_load is defensive; load order follows
 * realize order, so no qtest can make the off Bedrock load after its peer.
 */
static void test_unpowered_first_node_survives_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-unpowered-first-XXXXXX",
                                         &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *from_image = NULL;
    g_autofree char *to_image = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    from = unpowered_start("origin300-two-chassis", &from_image, "");
    to = unpowered_start("origin300-two-chassis", &to_image,
                         "-incoming defer");
    g_assert_cmphex(cpu_read_ok(from, 2, NODE(0) + 0x10000), ==,
                    UINT64_C(0x8080808080808080));
    qtest_set_irq_in(from, "/machine/node0", "power", 0, 0);
    assert_unreachable(from, 2, NODE(0) + 0x10000);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    assert_unreachable(to, 2, NODE(0) + 0x10000);
    qtest_set_irq_in(to, "/machine/node0", "power", 0, 1);
    unpowered_program_node(to, 0, false);
    g_assert_cmphex(cpu_read_ok(to, 2, NODE(0) + 0x10000), ==,
                    UINT64_C(0x8080808080808080));
    qtest_quit(to);
    qtest_quit(from);
    g_unlink(from_image);
    g_unlink(to_image);
    g_unlink(socket);
    g_rmdir(dir);
}

/*
 * The local boot view is a reader-relative form of the reader's own node's
 * flash, and so is the reset view until the nodes carry different numbers.
 * Each node gets its own 16 MiB boot flash; node 1's carries a marker at
 * position zero, node 0's is blank.
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

/*
 * A monitor read is a memory reference, not a firmware one. Once node 1
 * carries 1 its processors' reference at node field zero names node 0, whose
 * reset view (no bank enabled, so its blank flash) answers.
 */
static void test_reset_view_is_each_nodes_own(void)
{
    char *flashes[2];
    QTestState *qts = machine_start_marked(flashes);

    g_assert_cmphex(cpu_read_ok(qts, 2, PROM_RESET), ==, PROM_MARKER);
    g_assert_cmphex(cpu_read_ok(qts, 3, PROM_RESET), ==, PROM_MARKER);
    g_assert_cmphex(cpu_read_ok(qts, 0, PROM_RESET), ==, 0);
    g_assert_cmphex(cpu_read_ok(qts, 1, PROM_RESET), ==, 0);
    set_node_number(qts, 1, 1);
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        g_assert_cmphex(cpu_read_ok(qts, cpu, PROM_RESET), ==, 0);
    }
    g_assert_cmphex(cpu_read_ok(qts, 2, NODE(1) + PROM_RESET), ==,
                    PROM_MARKER);
    machine_quit_marked(qts, flashes);
}

/*
 * The reset view's memory page at node field zero is node 0's page from
 * every node once node 1 carries another number: a coherent reference names
 * memory by number, and only the firmware's references are reader-relative.
 * The Origin 3200's 512 MiB bank 0 covers the reset address, so the page is
 * RAM on both nodes; each node's copy carries its own marker.
 */
#define ORIGIN3200_ARGS "-M origin3000,topology=origin3200 " \
                        "-cpu R12000 -smp 8 -m 8192"
/* Bank 0 alone, 512 MiB (size code 4), no bank swap. */
#define MD_BANK0_512MB UINT64_C(0x41)
#define RESET_PAGE (PROM_RESET + UINT64_C(0x8000))
#define RESET_PAGE_NODE0 UINT64_C(0x00c0ffee00000000)
#define RESET_PAGE_NODE1 UINT64_C(0x00c0ffee00000001)

static void test_reset_page_is_node_zeros(void)
{
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(ORIGIN3200_ARGS " -bios %s", quoted);

    qtest_writeq(qts, BEDROCK_LOCAL + MD_MEMORY_CONFIG, MD_BANK0_512MB);
    peer_access(qts, MD_MEMORY_CONFIG, true, MD_BANK0_512MB);
    qtest_writeq(qts, RESET_PAGE, RESET_PAGE_NODE0);
    set_node_number(qts, 1, 1);
    qtest_writeq(qts, NODE(1) + RESET_PAGE, RESET_PAGE_NODE1);

    for (unsigned int cpu = 0; cpu < 8; cpu++) {
        g_assert_cmphex(cpu_read_ok(qts, cpu, RESET_PAGE), ==,
                        RESET_PAGE_NODE0);
        g_assert_cmphex(cpu_read_ok(qts, cpu, NODE(1) + RESET_PAGE), ==,
                        RESET_PAGE_NODE1);
    }
    qtest_quit(qts);
}

/*
 * On a routed machine the reset page at node field zero follows the
 * reader's route to number 0, like any other node-field-zero address: out
 * of reach it is a decode error, and once the NI route table names the
 * router port of node 0, node 0's page answers. An Origin 3400's node 1 is
 * on router port 3 and node 0 on port 2, so its route to number 0 is turn 7.
 */
#define ORIGIN3400_ARGS "-M origin3000,topology=origin3400 " \
                        "-cpu R12000 -smp 16 -m 16384"
#define NI_LOCAL_ROUTE(number) (UINT64_C(0x681000) + (number) * 8)
#define ROUTED_PROBE UINT64_C(0x10000)

static void test_routed_reset_page_follows_the_route(void)
{
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(ORIGIN3400_ARGS " -bios %s", quoted);

    for (unsigned int node = 0; node < 4; node++) {
        bte_cpu_writeq(qts, node * 4, BEDROCK_LOCAL + MD_MEMORY_CONFIG,
                       MD_BANK0_512MB);
    }
    bte_cpu_writeq(qts, 0, RESET_PAGE, RESET_PAGE_NODE0);
    bte_cpu_writeq(qts, 0, ROUTED_PROBE, WORD0);
    bte_cpu_writeq(qts, 4, RESET_PAGE, RESET_PAGE_NODE1);
    for (unsigned int node = 1; node < 4; node++) {
        bte_cpu_writeq(qts, node * 4, BEDROCK_LOCAL + LB_REVISION,
                       (uint64_t)node << REVISION_NODE_SHIFT);
    }

    assert_unreachable(qts, 4, ROUTED_PROBE);
    assert_unreachable(qts, 4, RESET_PAGE);
    bte_cpu_writeq(qts, 4, BEDROCK_LOCAL + NI_LOCAL_ROUTE(0), 7);
    g_assert_cmphex(cpu_read_ok(qts, 4, ROUTED_PROBE), ==, WORD0);
    g_assert_cmphex(cpu_read_ok(qts, 4, RESET_PAGE), ==, RESET_PAGE_NODE0);
    g_assert_cmphex(cpu_read_ok(qts, 0, RESET_PAGE), ==, RESET_PAGE_NODE0);
    qtest_quit(qts);
}

/* The path of the first router in the machine's modules. */
static char *first_router_path(QTestState *qts)
{
    for (unsigned int module = 0; module < 16; module++) {
        g_autofree char *path = g_strdup_printf("/machine/module%u/router",
                                                module);
        QDict *resp = qtest_qmp(qts, "{'execute':'qom-list',"
                                "'arguments':{'path':%s}}", path);
        bool found = qdict_haskey(resp, "return");

        qobject_unref(resp);
        if (found) {
            return g_steal_pointer(&path);
        }
    }
    g_assert_not_reached();
}

/*
 * A router that loses its supply or is held in reset takes the memory behind
 * it off every other node's map, because the links through it read down. It
 * returns when the line does.
 */
/* Give node 0 a probe word and node 1's processor the route 7 to it. */
static void routed_probe_prepare(QTestState *qts)
{
    for (unsigned int node = 0; node < 4; node++) {
        bte_cpu_writeq(qts, node * 4, BEDROCK_LOCAL + MD_MEMORY_CONFIG,
                       MD_BANK0_512MB);
    }
    bte_cpu_writeq(qts, 0, ROUTED_PROBE, WORD0);
    for (unsigned int node = 1; node < 4; node++) {
        bte_cpu_writeq(qts, node * 4, BEDROCK_LOCAL + LB_REVISION,
                       (uint64_t)node << REVISION_NODE_SHIFT);
    }
    bte_cpu_writeq(qts, 4, BEDROCK_LOCAL + NI_LOCAL_ROUTE(0), 7);
}

/* Node 0's probe word is reachable from node 1's processor over route 7. */
static void routed_probe_setup(QTestState *qts)
{
    routed_probe_prepare(qts);
    g_assert_cmphex(cpu_read_ok(qts, 4, ROUTED_PROBE), ==, WORD0);
}

static void test_dead_router_removes_the_memory_behind_it(const void *data)
{
    const char *line = data;
    bool power = !strcmp(line, "power");
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(ORIGIN3400_ARGS " -bios %s", quoted);
    g_autofree char *router = first_router_path(qts);

    routed_probe_setup(qts);
    qtest_set_irq_in(qts, router, line, 0, power ? 0 : 1);
    assert_unreachable(qts, 4, ROUTED_PROBE);
    qtest_set_irq_in(qts, router, line, 0, power ? 1 : 0);
    g_assert_cmphex(cpu_read_ok(qts, 4, ROUTED_PROBE), ==, WORD0);
    qtest_quit(qts);
}

/* The L1 that shares the router's module: the R-brick's own controller. */
static char *router_l1_path(const char *router)
{
    g_autofree char *module = g_strndup(router, strlen(router) -
                                        strlen("/router"));

    return g_strdup_printf("%s/l1", module);
}

/*
 * With the L1's start-up power on, a routed machine boots as before: the
 * R-brick's L1 comes up with its brick powered and the router released, and
 * the probe word crosses the router. The L1's lines reach the router and
 * move with it.
 */
static void test_router_l1_starts_powered_and_released(void)
{
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(ORIGIN3400_ARGS " -bios %s", quoted);
    g_autofree char *router = first_router_path(qts);
    g_autofree char *l1 = router_l1_path(router);

    g_assert_false(qtest_qom_get_bool(qts, l1, "reset-hold"));
    routed_probe_setup(qts);
    qtest_quit(qts);
}

/*
 * An R-brick whose L1 starts it off, as with auto-power off, starts its
 * router offline: the L1 settled both lines before the router existed, and
 * the machine hands them over when it builds the router. The memory behind
 * the router is out of every map until the front switch powers the brick up.
 */
static void test_router_l1_starts_off_when_its_l1_does(void)
{
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(ORIGIN3400_ARGS
                                  " -global sgi-l1.auto-power=off -bios %s",
                                  quoted);
    g_autofree char *router = first_router_path(qts);
    g_autofree char *l1 = router_l1_path(router);

    /*
     * The property reaches every L1, so the C-bricks start off too and
     * their processors answer nothing. Switch them on, as their L1s would.
     */
    for (unsigned int node = 0; node < 4; node++) {
        g_autofree char *path = g_strdup_printf("/machine/node%u", node);

        qtest_set_irq_in(qts, path, "power", 0, 1);
    }
    g_assert_true(qtest_qom_get_bool(qts, l1, "reset-hold"));
    routed_probe_prepare(qts);
    assert_unreachable(qts, 4, ROUTED_PROBE);
    qtest_set_irq_in(qts, l1, "power-button", 0, 1);
    qtest_set_irq_in(qts, l1, "power-button", 0, 0);
    g_assert_false(qtest_qom_get_bool(qts, l1, "reset-hold"));
    g_assert_cmphex(cpu_read_ok(qts, 4, ROUTED_PROBE), ==, WORD0);
    qtest_quit(qts);
}

/*
 * The L1's power-down raises the hold, then drops node-power: the router
 * goes dark and the memory behind it leaves every map. Each line alone
 * keeps it dark: the other, forced high at the router's input, does not
 * bring it back. Power-up releases both.
 */
static void test_router_l1_power_follows_the_switch(void)
{
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(ORIGIN3400_ARGS " -bios %s", quoted);
    g_autofree char *router = first_router_path(qts);
    g_autofree char *l1 = router_l1_path(router);

    routed_probe_setup(qts);
    qtest_set_irq_in(qts, l1, "power-button", 0, 1);
    qtest_set_irq_in(qts, l1, "power-button", 0, 0);
    g_assert_true(qtest_qom_get_bool(qts, l1, "reset-hold"));
    assert_unreachable(qts, 4, ROUTED_PROBE);
    /* The hold alone keeps the router dark, and so does the lost supply. */
    qtest_set_irq_in(qts, router, "power", 0, 1);
    assert_unreachable(qts, 4, ROUTED_PROBE);
    qtest_set_irq_in(qts, router, "power", 0, 0);
    qtest_set_irq_in(qts, router, "reset-hold", 0, 0);
    assert_unreachable(qts, 4, ROUTED_PROBE);
    qtest_set_irq_in(qts, router, "reset-hold", 0, 1);

    qtest_set_irq_in(qts, l1, "power-button", 0, 1);
    qtest_set_irq_in(qts, l1, "power-button", 0, 0);
    g_assert_false(qtest_qom_get_bool(qts, l1, "reset-hold"));
    g_assert_cmphex(cpu_read_ok(qts, 4, ROUTED_PROBE), ==, WORD0);
    qtest_quit(qts);
}

/*
 * Across two routers the hub's route table names the turn at the first
 * router, and each router's table, read on the link the packet entered by,
 * names the turn at the next. These are the values the stock PROM writes on
 * the Origin 3400 with two R-bricks: C-bricks 0 and 1 on ports 2 and 3 of
 * router 0, C-bricks 2 and 3 on ports 2 and 3 of router 1, and the routers
 * joined port 1 to port 1 and port 6 to port 6. Router 1 programs number 0
 * on links 2 and 3 with turn 1 (router 0, in on port 1, out on port 2) and
 * number 1 with turn 5 (in on port 6, out on port 3); its exit links' fields
 * for those numbers stay unwritten.
 */
#define TWO_ROUTER_ARGS "-M origin3000,topology=origin3400-two-routers " \
                        "-cpu R12000 -smp 16 -m 16384"
#define ROUTER_LOCAL_TABLE(link, number) \
    (router_link_bank[(link) - 1] + UINT64_C(0x200) + (number) * 8)

static const uint64_t router_link_bank[] = {
    0x10000, 0x10800, 0x20000, 0x20800, 0x30000, 0x30800, 0x40000, 0x40800,
};

/* A vector write from processor cpu's own node along route. */
static void cpu_vector_write(QTestState *qts, unsigned int cpu,
                             uint64_t route, uint64_t offset, uint64_t data)
{
    uint64_t status;

    (void)bte_cpu_readq(qts, cpu, BEDROCK_LOCAL + LB_VECTOR_STATUS_CLEAR);
    bte_cpu_writeq(qts, cpu, BEDROCK_LOCAL + LB_VECTOR_ROUTE, route);
    bte_cpu_writeq(qts, cpu, BEDROCK_LOCAL + LB_VECTOR_DATA, data);
    bte_cpu_writeq(qts, cpu, BEDROCK_LOCAL + LB_VECTOR_PARAMS,
                   (offset & UINT64_C(0xfffff8)) | VECTOR_REQUEST(0x1c8) |
                   VECTOR_WRITE);
    status = bte_cpu_readq(qts, cpu, BEDROCK_LOCAL + LB_VECTOR_STATUS);
    g_assert_cmphex(status & VECTOR_VALID, ==, VECTOR_VALID);
    g_assert_cmphex(status & 7, ==, VECTOR_WRITE);
}

static void test_two_routers_route_by_entry_link(void)
{
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(TWO_ROUTER_ARGS " -bios %s", quoted);

    for (unsigned int node = 0; node < 4; node++) {
        bte_cpu_writeq(qts, node * 4, BEDROCK_LOCAL + MD_MEMORY_CONFIG,
                       MD_BANK0_512MB);
    }
    /* Each node writes its own memory while every node still carries 0. */
    bte_cpu_writeq(qts, 0, ROUTED_PROBE, WORD0);
    bte_cpu_writeq(qts, 4, ROUTED_PROBE, WORD1);
    for (unsigned int node = 1; node < 4; node++) {
        bte_cpu_writeq(qts, node * 4, BEDROCK_LOCAL + LB_REVISION,
                       (uint64_t)node << REVISION_NODE_SHIFT);
    }

    /* Node 2 to number 0: out of router 1 by port 1, turn 7 from port 2. */
    bte_cpu_writeq(qts, 8, BEDROCK_LOCAL + NI_LOCAL_ROUTE(0), 7);
    /* Node 3 to number 1: out of router 1 by port 6, turn 3 from port 3. */
    bte_cpu_writeq(qts, 12, BEDROCK_LOCAL + NI_LOCAL_ROUTE(1), 3);
    assert_unreachable(qts, 8, ROUTED_PROBE);
    assert_unreachable(qts, 12, NODE(1) + ROUTED_PROBE);

    /* Route 0 ends at the processor's own router: router 1. */
    for (unsigned int link = 2; link <= 3; link++) {
        cpu_vector_write(qts, 8, 0, ROUTER_LOCAL_TABLE(link, 0), 1);
        cpu_vector_write(qts, 8, 0, ROUTER_LOCAL_TABLE(link, 1), 5);
    }
    g_assert_cmphex(cpu_read_ok(qts, 8, ROUTED_PROBE), ==, WORD0);
    g_assert_cmphex(cpu_read_ok(qts, 12, NODE(1) + ROUTED_PROBE), ==, WORD1);
    qtest_quit(qts);
}

/*
 * The high-bandwidth Origin 3800 joins its two routers on ports 1, 6, 7
 * and 8, port to equal port, with C-bricks 0 to 3 on ports 2 to 5 of router
 * 0 and C-bricks 4 to 7 on ports 2 to 5 of router 1. The look-ahead rule
 * holds on the cables the two-router Origin 3400 does not have. Node 4, on
 * router 1 port 2, reaches number 0 by port 8: hub turn (8 - 2) mod 8 = 6,
 * and router 1's link-2 field, the turn router 0 applies, is (2 - 8) mod 8 =
 * 2. Node 5, on router 1 port 3, reaches number 1 by port 7: hub turn 4, and
 * router 1's link-3 field (3 - 7) mod 8 = 4.
 */
#define ORIGIN3800_ARGS "-M origin3000,topology=origin3800-high-bandwidth " \
                        "-cpu R12000 -smp 32 " SGI_UNRESERVED_RAM("32768M")

static void test_origin3800_routes_over_ports_7_and_8(void)
{
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(ORIGIN3800_ARGS " -bios %s", quoted);

    for (unsigned int node = 0; node < 8; node++) {
        bte_cpu_writeq(qts, node * 4, BEDROCK_LOCAL + MD_MEMORY_CONFIG,
                       MD_BANK0_512MB);
    }
    bte_cpu_writeq(qts, 0, ROUTED_PROBE, WORD0);
    bte_cpu_writeq(qts, 4, ROUTED_PROBE, WORD1);
    for (unsigned int node = 1; node < 8; node++) {
        bte_cpu_writeq(qts, node * 4, BEDROCK_LOCAL + LB_REVISION,
                       (uint64_t)node << REVISION_NODE_SHIFT);
    }

    bte_cpu_writeq(qts, 16, BEDROCK_LOCAL + NI_LOCAL_ROUTE(0), 6);
    bte_cpu_writeq(qts, 20, BEDROCK_LOCAL + NI_LOCAL_ROUTE(1), 4);
    assert_unreachable(qts, 16, ROUTED_PROBE);
    assert_unreachable(qts, 20, NODE(1) + ROUTED_PROBE);

    cpu_vector_write(qts, 16, 0, ROUTER_LOCAL_TABLE(2, 0), 2);
    cpu_vector_write(qts, 20, 0, ROUTER_LOCAL_TABLE(3, 1), 4);
    g_assert_cmphex(cpu_read_ok(qts, 16, ROUTED_PROBE), ==, WORD0);
    g_assert_cmphex(cpu_read_ok(qts, 20, NODE(1) + ROUTED_PROBE), ==, WORD1);
    qtest_quit(qts);
}

/* A vector read from processor cpu's own node along route. */
static uint64_t cpu_vector_read(QTestState *qts, unsigned int cpu,
                                uint64_t route, uint64_t offset)
{
    uint64_t status;

    (void)bte_cpu_readq(qts, cpu, BEDROCK_LOCAL + LB_VECTOR_STATUS_CLEAR);
    bte_cpu_writeq(qts, cpu, BEDROCK_LOCAL + LB_VECTOR_ROUTE, route);
    bte_cpu_writeq(qts, cpu, BEDROCK_LOCAL + LB_VECTOR_PARAMS,
                   (offset & UINT64_C(0xfffff8)) | VECTOR_REQUEST(0x1c8));
    status = bte_cpu_readq(qts, cpu, BEDROCK_LOCAL + LB_VECTOR_STATUS);
    g_assert_cmphex(status & VECTOR_VALID, ==, VECTOR_VALID);
    g_assert_cmphex(status & 7, ==, 0);
    return bte_cpu_readq(qts, cpu, BEDROCK_LOCAL + LB_VECTOR_READ);
}

/*
 * Every router leaves reset with scratch 0 to 4 clear. Scratch 0 holds the
 * router's serial in bits 47:0 beside a booted flag in bit 63 and a local
 * identifier in bits 62:56, which the stock PROM sets only when it programs
 * a router; a router that powers up already flagged as booted makes the
 * PROM skip the router's identity and numbering. A route is the port the
 * first router sends out of, and route 0 ends at the first router.
 */
typedef struct RouterResetCase {
    const char *args;
    unsigned int routers;
    uint64_t route[2];
} RouterResetCase;

static void test_routers_reset_unprogrammed(gconstpointer data)
{
    const RouterResetCase *c = data;
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf("%s -bios %s", c->args, quoted);

    for (unsigned int i = 0; i < c->routers; i++) {
        for (unsigned int word = 0; word < 5; word++) {
            g_assert_cmphex(cpu_vector_read(qts, 0, c->route[i],
                                            UINT64_C(0x100) + 8 * word),
                            ==, 0);
        }
    }
    qtest_quit(qts);
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
    SgiWait w;
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
        0x340e0000 | own, 0x000e707c, 0x016e5825, /* own node field */
        0x340d0042, 0xad6d0000, /* outside copy: write to own RAM */
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
    sgi_wait_start(&w, "sibling PROM park");
    for (;;) {
        g_autoptr(QDict) sibling = qtest_qmp(qts,
            "{'execute':'human-monitor-command', 'arguments':"
            "{'command-line':'info registers', 'cpu-index':3}}");

        if (strstr(qdict_get_str(sibling, "return"),
                   "pc=0xffffffffbfc0052c")) {
            break;
        }
        sgi_wait_pause(&w, 10000);
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
    g_assert_cmphex(qtest_readq(qts, PROM_DATA_WINDOW), ==, WORD0);
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
        selected ? "origin300-numalink-pci-expansion" :
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
        for (unsigned mismatch = 1; mismatch <= 2; mismatch++) {
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

/*
 * Every processor of both nodes starts at its reset vector at once, each on
 * its own vCPU thread, and so each first fetches through its node's reset
 * router together with its sibling. TCG adds a processor's notifier to a
 * router on its first fill there, without the BQL, so the machine adds them
 * all before any vCPU runs; the router refuses one added later, which ends
 * the run before a processor can publish. Each processor publishes its
 * slice into its own real-time compare register through the local form,
 * which resolves to the requester's own processor interface.
 */
#define START_PI_RT_COMPARE_A UINT64_C(0x108)
#define START_PI1 UINT64_C(0x200000)
#define START_MARK_BASE 0xa0

static char *create_start_prom(void)
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
    stl_be_p(image + 0x400, 0x400c6000); /* mfc0 t4, Status */
    stl_be_p(image + 0x404, 0x358c0080); /* ori t4, t4, Status.KX */
    stl_be_p(image + 0x408, 0x408c6000); /* mtc0 t4, Status */
    stl_be_p(image + 0x40c, 0x00000000); /* nop */
    stl_be_p(image + 0x410, 0x3c089200); /* lui t0, 0x9200 */
    stl_be_p(image + 0x414, 0x0008403c); /* dsll32 t0, t0, 0 */
    stl_be_p(image + 0x418, 0x3c090100); /* lui t1, 0x0100 */
    stl_be_p(image + 0x41c, 0x01094025); /* or t0, t0, t1 */
    stl_be_p(image + 0x420, 0xdd0a0020); /* ld t2, 0x20(t0) -- CPU number */
    stl_be_p(image + 0x424, 0x314a0003); /* andi t2, t2, 3 -- the slice */
    stl_be_p(image + 0x428, 0x354b00a0); /* ori t3, t2, START_MARK_BASE */
    stl_be_p(image + 0x42c, 0x314c0001); /* andi t4, t2, 1 -- slot in PI */
    stl_be_p(image + 0x430, 0x000c60f8); /* dsll t4, t4, 3 -- compare stride */
    stl_be_p(image + 0x434, 0x010c602d); /* daddu t4, t0, t4 */
    stl_be_p(image + 0x438, 0xfd8b0108); /* sd t3, 0x108(t4) -- own compare */
    stl_be_p(image + 0x43c, 0x00000000); /* nop */
    stl_be_p(image + 0x440, 0x1000ffff); /* beq zero, zero, self */
    stl_be_p(image + 0x444, 0x00000000); /* nop */

    fd = g_file_open_tmp("sn1-cross-node-start-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_every_processor_starts_through_its_router(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    QTestState *qts;
    SgiWait w;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    prom = create_start_prom();
    quoted = g_shell_quote(prom);
    qts = qtest_initf(MACHINE_ARGS " -bios %s -accel tcg,thread=multi "
                      "-no-shutdown -S", quoted);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");

    /* Processors 0 and 2 read their own node's registers. */
    for (unsigned int node = 0; node < 2; node++) {
        unsigned int reader = node * 2;
        unsigned int published = 0;

        sgi_wait_start(&w, "node %u's processors' marks", node);
        for (;;) {
            published = 0;
            for (unsigned int slice = 0; slice < 4; slice++) {
                uint64_t address = BEDROCK_LOCAL + START_PI_RT_COMPARE_A +
                                   (slice >> 1) * START_PI1 + (slice & 1) * 8;
                uint64_t value = cpu_read_ok(qts, reader, address);

                if (value) {
                    g_assert_cmphex(value, ==, START_MARK_BASE | slice);
                    published++;
                }
            }
            if (published == 2) {
                break;
            }
            sgi_wait_pause(&w, 10000);
        }
        g_assert_cmpuint(published, ==, 2);
    }

    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * Per-node power. A node's supply stops its own processors and nothing
 * else: the other node keeps running, and the node comes back from a cold
 * start. The processors run code that counts into the node's own
 * real-time compare register, so a stopped node shows as a frozen count
 * and a running one as an advancing count.
 */
#define COUNT_SLICE_A UINT64_C(0x108)

static char *create_counter_prom(unsigned int size)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(size);
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    stl_be_p(image + 0x004, 0x00000000); /* nop */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff); /* beq zero, zero, self */
    }
    stl_be_p(image + 0x400, 0x400c6000); /* mfc0 t4, Status */
    stl_be_p(image + 0x404, 0x358c0080); /* ori t4, t4, Status.KX */
    stl_be_p(image + 0x408, 0x408c6000); /* mtc0 t4, Status */
    stl_be_p(image + 0x40c, 0x00000000); /* nop */
    stl_be_p(image + 0x410, 0x3c089200); /* lui t0, 0x9200 */
    stl_be_p(image + 0x414, 0x0008403c); /* dsll32 t0, t0, 0 */
    stl_be_p(image + 0x418, 0x3c090100); /* lui t1, 0x0100 */
    stl_be_p(image + 0x41c, 0x01094025); /* or t0, t0, t1 */
    stl_be_p(image + 0x420, 0xdd0a0020); /* ld t2, 0x20(t0) -- CPU number */
    stl_be_p(image + 0x424, 0x314a0003); /* andi t2, t2, 3 -- the slice */
    stl_be_p(image + 0x428, 0x1540000a); /* bne t2, zero, 0x454 -- not A */
    stl_be_p(image + 0x42c, 0x00000000); /* nop */
    /* Slice A counts into its own compare register, for ever. */
    stl_be_p(image + 0x430, 0x340b0000); /* ori t3, zero, 0 */
    stl_be_p(image + 0x434, 0x256b0001); /* addiu t3, t3, 1 */
    stl_be_p(image + 0x438, 0xfd0b0108); /* sd t3, 0x108(t0) */
    stl_be_p(image + 0x43c, 0x1000fffd); /* beq zero, zero, 0x434 */
    stl_be_p(image + 0x440, 0x00000000); /* nop */
    /* The others wait: a processor that is not counting costs nothing. */
    stl_be_p(image + 0x454, 0x42000020); /* wait */
    stl_be_p(image + 0x458, 0x1000fffe); /* beq zero, zero, 0x454 */
    stl_be_p(image + 0x45c, 0x00000000); /* nop */

    fd = g_file_open_tmp("sn1-cross-node-counter-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, size), ==, (ssize_t)size);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

/* Which of node @node's processors are halted, by number within the node. */
static unsigned int halted_cpus(QTestState *qts, unsigned int node)
{
    g_autofree char *path = g_strdup_printf("/machine/node%u", node);
    QDict *reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'halted-cpus'}}", path);
    unsigned int mask;

    g_assert_true(qdict_haskey(reply, "return"));
    mask = qdict_get_int(reply, "return");
    qobject_unref(reply);
    return mask;
}

/* Whether processor @cpu is halted; the two-chassis nodes have two each. */
static bool cpu_halted(QTestState *qts, unsigned int cpu)
{
    return halted_cpus(qts, cpu / 2) & (1U << (cpu % 2));
}

static void wait_halted(QTestState *qts, unsigned int cpu, bool halted)
{
    SgiWait w;

    sgi_wait_start(&w, "processor %u %s", cpu,
                   halted ? "halted" : "running");
    while (cpu_halted(qts, cpu) != halted) {
        g_assert_true(sgi_wait_continue(&w, 10000));
    }
}

static void wait_halted_mask(QTestState *qts, unsigned int node,
                             unsigned int mask)
{
    SgiWait w;

    sgi_wait_start(&w, "node %u's halted mask to be 0x%x", node, mask);
    while (halted_cpus(qts, node) != mask) {
        g_assert_true(sgi_wait_continue(&w, 10000));
    }
}

/* The count of the node whose first processor is @cpu; it must be readable. */
static uint64_t node_count(QTestState *qts, unsigned int cpu)
{
    return cpu_read_ok(qts, cpu, BEDROCK_LOCAL + COUNT_SLICE_A);
}

/* Wait for the count to move past the value it had, and return the new one. */
static uint64_t wait_count_advances(QTestState *qts, unsigned int cpu)
{
    uint64_t before = node_count(qts, cpu), now;
    SgiWait w;

    sgi_wait_start(&w, "the count of processor %u to advance", cpu);
    while ((now = node_count(qts, cpu)) == before) {
        g_assert_true(sgi_wait_continue(&w, 10000));
    }
    return now;
}

/* The count of an off node must stay where it is, however long we look. */
static void assert_count_frozen(QTestState *qts, unsigned int cpu)
{
    uint64_t before = node_count(qts, cpu);

    g_usleep(300 * 1000);
    g_assert_cmphex(node_count(qts, cpu), ==, before);
}

static QTestState *counter_start(char **prom, const char *machine,
                                 unsigned int size)
{
    g_autofree char *quoted = NULL;

    *prom = create_counter_prom(size);
    quoted = g_shell_quote(*prom);
    return qtest_initf("%s -bios %s -accel tcg,thread=multi -no-shutdown -S",
                       machine, quoted);
}

/*
 * Node 1 loses its supply: its processors stop while node 0 keeps counting,
 * the machine stays up, no interrupt and no NMI line wakes the stopped
 * node, and neither does a stop and continue. When the supply returns the
 * node starts over at the reset vector and counts again from zero.
 */
static void test_node_power_stops_one_node(void)
{
    g_autofree char *prom = NULL;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = counter_start(&prom, MACHINE_ARGS, PROM_SIZE);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 0);
    wait_count_advances(qts, 2);

    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 0);
    wait_halted(qts, 2, true);
    wait_halted(qts, 3, true);
    g_assert_false(cpu_halted(qts, 0));
    wait_count_advances(qts, 0);
    assert_count_frozen(qts, 2);

    /* The Bedrock's own NMI input is refused while the node is off. */
    qtest_set_irq_in(qts, "/machine/node1/bedrock", "pi-nmi", 0, 1);
    qtest_set_irq_in(qts, "/machine/node1/bedrock", "pi-nmi", 0, 0);
    g_usleep(300 * 1000);
    g_assert_true(cpu_halted(qts, 2));
    assert_count_frozen(qts, 2);

    /* A stop and continue is not a power-up. */
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 0);
    g_assert_true(cpu_halted(qts, 2));
    assert_count_frozen(qts, 2);

    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 1);
    wait_halted(qts, 2, false);
    wait_count_advances(qts, 2);
    wait_count_advances(qts, 0);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A start a power-up owes is performed once. The returning node's starts
 * are left owed and unqueued for two seconds (x-test-start-delay-ms); a stop
 * and continue in that time finds them owed and starts the processors at
 * once, as a migration's destination does, and the delayed start that
 * arrives afterwards must find nothing left to do. A second start would
 * reset the counting processor after it had been running, and its count
 * would fall back to zero.
 */
static void test_owed_start_runs_once(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *machine = g_strdup_printf("%s -global "
        "sgi-sn1-node.x-test-start-delay-ms=2000", MACHINE_ARGS);
    QTestState *qts;
    int64_t until;
    uint64_t last;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = counter_start(&prom, machine, PROM_SIZE);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 2);

    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 0);
    wait_halted(qts, 2, true);
    wait_halted(qts, 3, true);
    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 1);
    /* Nothing is queued yet: the processors stay halted. */
    g_assert_true(cpu_halted(qts, 2));

    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_halted(qts, 2, false);
    wait_halted(qts, 3, false);
    last = wait_count_advances(qts, 2);

    /* The delayed start arrives within this window; the count only grows. */
    until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < until) {
        uint64_t now = node_count(qts, 2);

        g_assert_cmpuint(now, >=, last);
        last = now;
        g_usleep(5000);
    }
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A system reset ends an owed start. The returning node's starts are left
 * owed for two seconds; a reset in that time starts every processor itself,
 * and the delayed start that arrives afterwards must find nothing owed. It
 * would otherwise reset a processor that is running, and its count would
 * fall back to zero.
 */
static void test_reset_clears_an_owed_start(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *machine = g_strdup_printf("%s -global "
        "sgi-sn1-node.x-test-start-delay-ms=2000", MACHINE_ARGS);
    QTestState *qts;
    int64_t until;
    uint64_t last;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = counter_start(&prom, machine, PROM_SIZE);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 2);

    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 0);
    wait_halted(qts, 2, true);
    wait_halted(qts, 3, true);
    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 1);
    g_assert_true(cpu_halted(qts, 2));

    qtest_system_reset(qts);
    wait_halted(qts, 2, false);
    last = wait_count_advances(qts, 2);

    /* The delayed start would arrive within this window. */
    until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < until) {
        uint64_t now = node_count(qts, 2);

        g_assert_cmpuint(now, >=, last);
        last = now;
        g_usleep(5000);
    }
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A stream that carries no owed start leaves none on the destination. The
 * destination's node 1 is power-cycled before the stream arrives, which owes
 * its starts for two seconds; the source's processors are running, and the
 * delayed start must not reset them once the stream has loaded.
 */
static void test_load_clears_an_owed_start(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-owed-load-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *delayed = g_strdup_printf("%s -global "
        "sgi-sn1-node.x-test-start-delay-ms=2000", MACHINE_ARGS);
    QTestState *from, *to;
    int64_t until;
    uint64_t last;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    g_assert_no_error(error);
    from = counter_start(&prom, MACHINE_ARGS, PROM_SIZE);
    quoted = g_shell_quote(prom);
    to = qtest_initf("%s -bios %s -accel tcg,thread=multi -no-shutdown -S "
                     "-incoming defer", delayed, quoted);
    qtest_qmp_assert_success(from, "{'execute':'cont'}");
    /* Far enough on that a restart from zero is plain to see. */
    while (node_count(from, 2) < 3000000) {
        g_usleep(10000);
    }

    qtest_set_irq_in(to, "/machine/node1", "power", 0, 0);
    qtest_set_irq_in(to, "/machine/node1", "power", 0, 1);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    last = node_count(from, 2);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    /* An owed start would have reset the counting processor by now. */
    g_usleep(200 * 1000);
    g_assert_cmpuint(node_count(to, 2), >=, last);
    last = wait_count_advances(to, 2);

    until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < until) {
        uint64_t now = node_count(to, 2);

        g_assert_cmpuint(now, >=, last);
        last = now;
        g_usleep(5000);
    }
    qtest_quit(to);
    qtest_quit(from);
    g_unlink(socket);
    g_rmdir(dir);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * The first node loses and regains its supply with the second running: only
 * the first is touched. Run on the accelerator qtest uses, where nothing
 * executes, to pin the halted state alone.
 */
static void test_node_power_halts_only_its_processors(void)
{
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(MACHINE_ARGS " -bios %s", quoted);

    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        g_assert_false(cpu_halted(qts, cpu));
    }
    qtest_set_irq_in(qts, "/machine/node0", "power", 0, 0);
    g_assert_true(cpu_halted(qts, 0));
    g_assert_true(cpu_halted(qts, 1));
    g_assert_false(cpu_halted(qts, 2));
    g_assert_false(cpu_halted(qts, 3));
    qtest_set_irq_in(qts, "/machine/node0", "power", 0, 1);
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        g_assert_false(cpu_halted(qts, cpu));
    }
    qtest_quit(qts);
}

/*
 * An off node stays off across a migration, and the node that kept running
 * is not touched by it: the destination holds the stopped processors from
 * the loaded level and the memory stays out of the other node's view.
 */
static void test_off_node_survives_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-off-node-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *from, *to;

    g_assert_no_error(error);
    from = qtest_initf(MACHINE_ARGS " -bios %s", quoted);
    to = qtest_initf(MACHINE_ARGS " -bios %s -incoming defer", quoted);
    qtest_set_irq_in(from, "/machine/node1", "power", 0, 0);
    g_assert_true(cpu_halted(from, 2));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    g_assert_true(cpu_halted(to, 2));
    g_assert_true(cpu_halted(to, 3));
    g_assert_false(cpu_halted(to, 0));
    g_assert_false(cpu_halted(to, 1));
    qtest_set_irq_in(to, "/machine/node1", "power", 0, 1);
    g_assert_false(cpu_halted(to, 2));
    g_assert_false(cpu_halted(to, 3));
    qtest_quit(to);
    qtest_quit(from);
    g_unlink(socket);
    g_rmdir(dir);
}

/*
 * The same on an Origin 3200, driven the way a person drives it: each
 * C-brick's L1 owns its node's supply. A power-down on one brick stops that
 * node alone and the machine goes on; the machine ends only when the last
 * compute node is off, and a power-up after that resumes it with only the
 * brick that was switched on running. Each brick has four processors.
 */
#define BRICK_CPUS 0xfU

/* Virtual time moves only under the qtest accelerator, not under TCG. */
static bool console_steps_clock = true;

/*
 * Type @text at the L1 console and read back until @until shows: the L1 runs
 * lines in order, so the last line of a batch having answered means the
 * earlier ones have run. ^T escapes to the L1 for one line.
 */
static void l1_console(QTestState *qts, int fd, const char *text,
                       const char *until)
{
    g_autoptr(GString) seen = g_string_new(NULL);
    SgiWait w;

    g_assert_cmpint(write(fd, text, strlen(text)), ==, strlen(text));
    sgi_wait_start(&w, "the L1 console to show '%s'", until);
    while (!strstr(seen->str, until)) {
        struct pollfd wait = { .fd = fd, .events = POLLIN };
        char chunk[256];
        ssize_t n;

        if (console_steps_clock) {
            qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 10);
        }
        if (poll(&wait, 1, 100) == 1) {
            n = read(fd, chunk, sizeof(chunk));
            g_assert_cmpint(n, >, 0);
            g_string_append_len(seen, chunk, n);
        } else if (!sgi_wait_continue(&w, 1000)) {
            g_test_message("L1 console showed: %s", seen->str);
            g_assert_not_reached();
        }
    }
}

static void l1_line(QTestState *qts, int fd, const char *command,
                    const char *until)
{
    g_autofree char *text = g_strdup_printf("\024%s\r", command);

    l1_console(qts, fd, text, until);
}

static QTestState *origin3200_console_start(int *console, const char *bios,
                                            const char *extra)
{
    g_autofree char *quoted = g_shell_quote(bios);
    int fds[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), ==, 0);
    qts = qtest_initf(ORIGIN3200_ARGS " -bios %s -nodefaults -display none "
                      "-no-shutdown -chardev socket,id=cb,fd=%d "
                      "-serial chardev:cb %s", quoted, fds[1], extra);
    close(fds[1]);
    *console = fds[0];
    return qts;
}

static bool machine_running(QTestState *qts)
{
    QDict *status = qtest_qmp(qts, "{'execute':'query-status'}");
    bool running = !strcmp(qdict_get_str(qdict_get_qdict(status, "return"),
                                         "status"), "running");

    qobject_unref(status);
    return running;
}

static void wait_machine_running(QTestState *qts)
{
    SgiWait w;

    sgi_wait_start(&w, "the machine to run");
    while (!machine_running(qts)) {
        g_assert_true(sgi_wait_continue(&w, 10000));
    }
}

/* The machine stays up for one brick and ends with the last. */
static void test_brick_power_ends_the_machine_last(void)
{
    g_autofree char *prom = create_counter_prom(PROM_SIZE);
    QTestState *qts;
    int console;

    qts = origin3200_console_start(&console, prom, "");
    g_assert_cmphex(halted_cpus(qts, 0), ==, 0);
    g_assert_cmphex(halted_cpus(qts, 1), ==, 0);

    /* A controller whose node is off stays at its prompt. */
    l1_console(qts, console, "\024power down\rpower check\r",
               "power appears off");
    g_assert_cmphex(halted_cpus(qts, 0), ==, BRICK_CPUS);
    g_assert_cmphex(halted_cpus(qts, 1), ==, 0);
    g_assert_true(machine_running(qts));

    /* The peer's L1 runs its own node alone, then the last one ends it. */
    l1_console(qts, console, "ctc power down\rctc power check\r",
               "power appears off");
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    g_assert_cmphex(halted_cpus(qts, 0), ==, BRICK_CPUS);
    g_assert_cmphex(halted_cpus(qts, 1), ==, BRICK_CPUS);
    g_assert_false(machine_running(qts));

    /* Power-up after the end resumes the machine with only that brick on. */
    l1_console(qts, console, "power up\r", "");
    qtest_qmp_eventwait(qts, "RESET");
    wait_machine_running(qts);
    g_assert_cmphex(halted_cpus(qts, 0), ==, 0);
    g_assert_cmphex(halted_cpus(qts, 1), ==, BRICK_CPUS);

    /* The other brick then starts alone, with no machine reset. */
    l1_console(qts, console, "\024ctc power up\r\024ctc power check\r",
               "power appears on");
    g_assert_cmphex(halted_cpus(qts, 1), ==, 0);
    g_assert_true(machine_running(qts));
    close(console);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/* A broadcast reaches both bricks, so it takes the last node with it. */
static void test_broadcast_power_down_ends_the_machine(void)
{
    g_autofree char *prom = create_counter_prom(PROM_SIZE);
    QTestState *qts;
    int console;

    qts = origin3200_console_start(&console, prom, "");
    l1_console(qts, console, "\024* power down\r", "");
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    g_assert_cmphex(halted_cpus(qts, 0), ==, BRICK_CPUS);
    g_assert_cmphex(halted_cpus(qts, 1), ==, BRICK_CPUS);

    /* A system reset is a mains cycle: each L1 powers its node on again. */
    qtest_system_reset(qts);
    g_assert_cmphex(halted_cpus(qts, 0), ==, 0);
    g_assert_cmphex(halted_cpus(qts, 1), ==, 0);
    l1_line(qts, console, "power check", "power appears on");
    close(console);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * An Origin 3200 running the counting program on threaded TCG, paused, with
 * the L1 console on a socket whose far end is returned in @console.
 */
static QTestState *origin3200_counter_start(int *console, const char *prom,
                                            const char *extra)
{
    g_autofree char *quoted = g_shell_quote(prom);
    int fds[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), ==, 0);
    qts = qtest_initf(ORIGIN3200_ARGS " -bios %s -nodefaults -display none "
                      "-no-shutdown -S -accel tcg,thread=multi "
                      "-chardev socket,id=cb,fd=%d -serial chardev:cb %s",
                      quoted, fds[1], extra);
    close(fds[1]);
    *console = fds[0];
    return qts;
}

/*
 * One brick's power-down with counting code on both nodes: the other node
 * keeps counting, the stopped node's count freezes, and power-up restarts it.
 */
static void test_brick_power_stops_one_counter(void)
{
    g_autofree char *prom = NULL;
    QTestState *qts;
    int console;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    prom = create_counter_prom(PROM_SIZE);
    qts = origin3200_counter_start(&console, prom, "");
    console_steps_clock = false;
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 0);
    wait_count_advances(qts, 4);

    l1_console(qts, console, "\024ctc power down\r\024ctc power check\r",
               "power appears off");
    wait_halted_mask(qts, 1, BRICK_CPUS);
    g_assert_true(machine_running(qts));
    wait_count_advances(qts, 0);
    assert_count_frozen(qts, 4);

    l1_console(qts, console, "\024ctc power up\r\024ctc power check\r",
               "power appears on");
    wait_halted_mask(qts, 1, 0);
    wait_count_advances(qts, 4);
    wait_count_advances(qts, 0);
    close(console);
    console_steps_clock = true;
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * The machine hears that the last node is off only once that node's
 * processors have all stopped work: by the time the shutdown arrives the
 * node's processors are halted. The processors run on their own threads and
 * the settle is a race that closes in microseconds, so each processor's stop
 * is delayed here (x-test-stop-delay-ms, which lets the main loop run
 * meanwhile); a machine that ended on the supply loss alone would announce
 * its shutdown with the processors still running.
 */
static void test_last_brick_power_down_announces_after_the_stop(void)
{
    g_autofree char *prom = NULL;
    QTestState *qts;
    QDict *event, *stamp;
    int64_t cut, raised;
    int console;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    prom = create_counter_prom(PROM_SIZE);
    qts = origin3200_counter_start(&console, prom,
                                   "-global sgi-sn1-node.x-test-stop-delay-ms=1500");
    console_steps_clock = false;
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 0);
    wait_count_advances(qts, 4);

    l1_console(qts, console, "\024power down\rpower check\r",
               "power appears off");
    wait_halted_mask(qts, 0, BRICK_CPUS);
    g_assert_true(machine_running(qts));
    /*
     * The last node's processors are still counting when it is cut. The
     * shutdown carries the time it was raised: it must be after the stops,
     * which each take the delay, not at the supply loss.
     */
    cut = g_get_real_time();
    l1_console(qts, console, "ctc power down\rctc power check\r",
               "power appears off");
    event = qtest_qmp_eventwait_ref(qts, "SHUTDOWN");
    stamp = qdict_get_qdict(event, "timestamp");
    raised = qdict_get_int(stamp, "seconds") * G_USEC_PER_SEC +
             qdict_get_int(stamp, "microseconds");
    qobject_unref(event);
    g_assert_cmpint(raised - cut, >=, 1000 * 1000);
    g_assert_cmphex(halted_cpus(qts, 1), ==, BRICK_CPUS);
    g_assert_cmphex(halted_cpus(qts, 0), ==, BRICK_CPUS);
    close(console);
    console_steps_clock = true;
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A stop and continue while the last node's stops are still owed must not
 * lose the announcement of the loss: resuming holds the off node's
 * processors again, and the machine must still hear, once they have all
 * stopped, that its last node is off. The stops of a loss are left unqueued
 * for 30 seconds (x-test-stop-queue-delay-ms), so the resume finds them
 * owed, and the shutdown must come from the resume's own stops.
 */
static void test_stop_cont_keeps_the_pending_announcement(void)
{
    g_autofree char *prom = NULL;
    QTestState *qts;
    QDict *event;
    SgiWait w;
    int console;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    prom = create_counter_prom(PROM_SIZE);
    qts = origin3200_counter_start(&console, prom,
                                   "-global sgi-sn1-node."
                                   "x-test-stop-queue-delay-ms=30000");
    console_steps_clock = false;
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 0);
    wait_count_advances(qts, 4);

    l1_console(qts, console, "\024power down\rpower check\r",
               "power appears off");
    l1_console(qts, console, "ctc power down\rctc power check\r",
               "power appears off");
    g_assert_true(machine_running(qts));
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");

    sgi_wait_start(&w, "the machine to shut down");
    while (machine_running(qts)) {
        g_assert_true(sgi_wait_continue(&w, 10000));
    }
    event = qtest_qmp_eventwait_ref(qts, "SHUTDOWN");
    qobject_unref(event);
    g_assert_cmphex(halted_cpus(qts, 1), ==, BRICK_CPUS);
    g_assert_cmphex(halted_cpus(qts, 0), ==, BRICK_CPUS);
    close(console);
    console_steps_clock = true;
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A node that loses its supply, regains it and loses it again before its
 * processors have drained the first stop: the stop work of the first loss
 * must not count towards the second, or the machine would be told early.
 * Each stop is held for a second (x-test-stop-delay-ms), so the second
 * loss's stops finish two seconds after the first loss, and the shutdown
 * must not be raised before.
 */
static void test_repeated_loss_announces_when_the_last_stop_ran(void)
{
    g_autofree char *prom = NULL;
    QTestState *qts;
    QDict *event, *stamp;
    int64_t cut, raised;
    int console;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    prom = create_counter_prom(PROM_SIZE);
    qts = origin3200_counter_start(&console, prom,
                                   "-global sgi-sn1-node.x-test-stop-delay-ms="
                                   "1000");
    console_steps_clock = false;
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 0);
    wait_count_advances(qts, 4);

    qtest_set_irq_in(qts, "/machine/node0", "power", 0, 0);
    wait_halted_mask(qts, 0, BRICK_CPUS);
    cut = g_get_real_time();
    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 0);
    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 1);
    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 0);
    event = qtest_qmp_eventwait_ref(qts, "SHUTDOWN");
    stamp = qdict_get_qdict(event, "timestamp");
    raised = qdict_get_int(stamp, "seconds") * G_USEC_PER_SEC +
             qdict_get_int(stamp, "microseconds");
    qobject_unref(event);
    g_assert_cmpint(raised - cut, >=, 1700 * 1000);
    g_assert_cmphex(halted_cpus(qts, 1), ==, BRICK_CPUS);
    close(console);
    console_steps_clock = true;
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A load must not run a node's supply cycle. The L1 drives its node's supply
 * again as it loads, while the Bedrock, the XBridge and the memory unit may
 * already have loaded the source's registers; a cold reset then would wipe
 * them. Each node's scratch register and memory configuration, written
 * before the migration, must read back after it.
 */
#define LOAD_SCRATCH0 UINT64_C(0x5ca1ab1e00000000)
#define LOAD_SCRATCH1 UINT64_C(0x5ca1ab1e00000001)
#define LOAD_ARGS ORIGIN3200_ARGS

static void load_registers_write(QTestState *qts)
{
    bte_cpu_writeq(qts, 0, BEDROCK_LOCAL + LB_SCRATCH2, LOAD_SCRATCH0);
    bte_cpu_writeq(qts, 4, BEDROCK_LOCAL + LB_SCRATCH2, LOAD_SCRATCH1);
    bte_cpu_writeq(qts, 0, BEDROCK_LOCAL + MD_MEMORY_CONFIG, MD_BANK0_512MB);
    bte_cpu_writeq(qts, 4, BEDROCK_LOCAL + MD_MEMORY_CONFIG, MD_BANK0_512MB);
}

static void load_registers_check(QTestState *qts, unsigned int node)
{
    unsigned int cpu = node * 4;

    g_assert_cmphex(bte_cpu_readq(qts, cpu, BEDROCK_LOCAL + LB_SCRATCH2), ==,
                    node ? LOAD_SCRATCH1 : LOAD_SCRATCH0);
    g_assert_cmphex(bte_cpu_readq(qts, cpu, BEDROCK_LOCAL + MD_MEMORY_CONFIG),
                    ==, MD_BANK0_512MB);
}

/* Both nodes on at the source, off at the destination's start. */
static void test_load_into_off_nodes(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-load-off-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *from, *to;

    g_assert_no_error(error);
    from = qtest_initf(LOAD_ARGS " -bios %s", quoted);
    to = qtest_initf(LOAD_ARGS " -global sgi-l1.auto-power=off -bios %s "
                     "-incoming defer", quoted);
    g_assert_cmphex(halted_cpus(to, 0), ==, BRICK_CPUS);
    g_assert_cmphex(halted_cpus(to, 1), ==, BRICK_CPUS);
    load_registers_write(from);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    load_registers_check(to, 0);
    load_registers_check(to, 1);
    sgi_migrate_resume(to);
    g_assert_cmphex(halted_cpus(to, 0), ==, 0);
    g_assert_cmphex(halted_cpus(to, 1), ==, 0);
    load_registers_check(to, 0);
    load_registers_check(to, 1);
    qtest_quit(to);
    qtest_quit(from);
    g_unlink(socket);
    g_rmdir(dir);
}

/* The second node off at the source, on at the destination's start. */
static void test_load_into_on_nodes(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-load-on-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *prom = create_counter_prom(PROM_SIZE);
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *from, *to;
    int console, fds[2];

    g_assert_no_error(error);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), ==, 0);
    from = qtest_initf(LOAD_ARGS " -bios %s -nodefaults -display none "
                       "-no-shutdown -chardev socket,id=cb,fd=%d "
                       "-serial chardev:cb", quoted, fds[1]);
    close(fds[1]);
    console = fds[0];
    to = qtest_initf(LOAD_ARGS " -bios %s -nodefaults -display none "
                     "-no-shutdown -incoming defer", quoted);
    load_registers_write(from);
    l1_console(from, console, "\024ctc power down\r\024ctc power check\r",
               "power appears off");
    g_assert_cmphex(halted_cpus(from, 1), ==, BRICK_CPUS);
    g_assert_cmphex(halted_cpus(to, 1), ==, 0);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    load_registers_check(to, 0);
    g_assert_cmphex(halted_cpus(to, 0), ==, 0);
    g_assert_cmphex(halted_cpus(to, 1), ==, BRICK_CPUS);
    sgi_migrate_resume(to);
    load_registers_check(to, 0);
    g_assert_cmphex(halted_cpus(to, 0), ==, 0);
    g_assert_cmphex(halted_cpus(to, 1), ==, BRICK_CPUS);
    close(console);
    qtest_quit(to);
    qtest_quit(from);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_unlink(socket);
    g_rmdir(dir);
}

/* The program counter of processor @cpu, from the monitor's register dump. */
static uint64_t cpu_pc(QTestState *qts, unsigned int cpu)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'human-monitor-command','arguments':"
        "{'command-line':'info registers','cpu-index':%u}}", cpu);
    const char *text, *pc;
    uint64_t value;

    g_assert_true(qdict_haskey(reply, "return"));
    text = qdict_get_str(reply, "return");
    pc = strstr(text, "pc=0x");
    g_assert_nonnull(pc);
    g_assert_cmpint(sscanf(pc + 5, "%" SCNx64, &value), ==, 1);
    return value;
}

/*
 * The processors are part of the stream too. A destination whose node starts
 * off, loading a source whose node ran, must not restart them at the reset
 * vector as its L1 drives the supply level during the load: they carry on
 * where the source stopped them.
 */
static void test_load_keeps_running_processors(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-load-cpus-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *from_prom = NULL, *to_prom = NULL;
    QTestState *from, *to;
    uint64_t pc;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    g_assert_no_error(error);
    from = counter_start(&from_prom, LOAD_ARGS, PROM_SIZE);
    to = counter_start(&to_prom, LOAD_ARGS " -global sgi-l1.auto-power=off "
                       "-incoming defer", PROM_SIZE);
    qtest_qmp_assert_success(from, "{'execute':'cont'}");
    wait_count_advances(from, 0);
    wait_count_advances(from, 4);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    /* The counting loop, not the reset vector. */
    pc = cpu_pc(to, 0);
    g_assert_cmphex(pc & 0xfff, >=, 0x430);
    g_assert_cmphex(pc & 0xfff, <=, 0x440);
    pc = cpu_pc(to, 4);
    g_assert_cmphex(pc & 0xfff, >=, 0x430);
    g_assert_cmphex(pc & 0xfff, <=, 0x440);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    wait_count_advances(to, 0);
    wait_count_advances(to, 4);
    qtest_quit(to);
    qtest_quit(from);
    g_assert_cmpint(unlink(from_prom), ==, 0);
    g_assert_cmpint(unlink(to_prom), ==, 0);
    g_unlink(socket);
    g_rmdir(dir);
}

/*
 * Node restart. A reset acts on one node: its processors stop, its parts
 * are reset and the processors start again at the reset vector, while the
 * other node goes on. The restarted node's starts are left owed for
 * RESTART_DELAY_MS (x-test-start-delay-ms) so that the window between the
 * reset and the start can be seen: the counting processor is held, and its
 * count, which lives in the Bedrock the reset cleared, reads zero.
 */
#define RESTART_DELAY_MS 2000
#define LB_SCRATCH0 UINT64_C(0x608000)
#define LB_SCRATCH1 UINT64_C(0x608008)

static char *restart_machine(const char *base)
{
    return g_strdup_printf("%s -global sgi-sn1-node.x-test-start-delay-ms=%d",
                           base, RESTART_DELAY_MS);
}

/* Whether processor @cpu is halted, when each node has @per_node of them. */
static bool cpu_halted_in(QTestState *qts, unsigned int cpu,
                          unsigned int per_node)
{
    return halted_cpus(qts, cpu / per_node) & (1U << (cpu % per_node));
}

/* Wait until processor @cpu is held with the count its reset cleared. */
static void wait_restart_window(QTestState *qts, unsigned int cpu,
                                unsigned int per_node)
{
    SgiWait w;

    sgi_wait_start(&w, "processor %u to be held with its count cleared", cpu);
    while (!cpu_halted_in(qts, cpu, per_node) || node_count(qts, cpu) != 0) {
        g_assert_true(sgi_wait_continue(&w, 10000));
    }
}

/* The restarted processor leaves its hold and counts up from the vector. */
static void wait_restart_runs(QTestState *qts, unsigned int cpu,
                              unsigned int per_node)
{
    SgiWait w;

    sgi_wait_start(&w, "processor %u to start", cpu);
    while (cpu_halted_in(qts, cpu, per_node)) {
        g_assert_true(sgi_wait_continue(&w, 10000));
    }
    wait_count_advances(qts, cpu);
}

/*
 * One node of the two-chassis Origin 300 restarts while the other counts on.
 * The restarted node's processor is held with a cleared count, the other
 * node is never halted and its count keeps rising, and the held node then
 * counts again from its reset vector.
 */
static void test_restart_one_node(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *machine = restart_machine(MACHINE_ARGS);
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = counter_start(&prom, machine, PROM_SIZE);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 0);
    wait_count_advances(qts, 2);

    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 1);
    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 0);
    wait_restart_window(qts, 2, 2);
    g_assert_false(cpu_halted(qts, 0));
    wait_count_advances(qts, 0);
    g_assert_true(machine_running(qts));
    wait_restart_runs(qts, 2, 2);

    /* And the other way round. */
    qtest_set_irq_in(qts, "/machine/node0", "reset", 0, 1);
    qtest_set_irq_in(qts, "/machine/node0", "reset", 0, 0);
    wait_restart_window(qts, 0, 2);
    g_assert_false(cpu_halted(qts, 2));
    wait_count_advances(qts, 2);
    wait_restart_runs(qts, 0, 2);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A node whose supply is off is not reached: neither its own reset nor the
 * other node's starts it, and it starts normally when the supply returns.
 * The powered node's reset reaches every powered compute node, so the
 * machine resets as a whole and keeps the off node off.
 */
static void test_restart_does_not_start_an_off_node(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *machine = restart_machine(MACHINE_ARGS);
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = counter_start(&prom, machine, PROM_SIZE);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 0);
    wait_count_advances(qts, 2);

    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 0);
    wait_halted(qts, 2, true);
    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 1);
    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 0);
    g_assert_true(cpu_halted(qts, 2));
    qtest_set_irq_in(qts, "/machine/node0", "reset", 0, 1);
    qtest_set_irq_in(qts, "/machine/node0", "reset", 0, 0);
    qtest_qmp_eventwait(qts, "RESET");
    g_usleep((RESTART_DELAY_MS + 500) * 1000);
    g_assert_true(cpu_halted(qts, 2));
    assert_count_frozen(qts, 2);
    wait_restart_runs(qts, 0, 2);

    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 1);
    wait_restart_runs(qts, 2, 2);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A restart whose processors are still stopping is overtaken by a loss of
 * the supply: the node ends off and the stale restart neither resets its
 * parts nor starts it. Each stop is held for a second
 * (x-test-stop-delay-ms), long enough to cut in.
 */
static void test_loss_overtakes_a_restart(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *machine = g_strdup_printf("%s -global "
        "sgi-sn1-node.x-test-stop-delay-ms=1000 -global "
        "sgi-sn1-node.x-test-start-delay-ms=%d", MACHINE_ARGS,
        RESTART_DELAY_MS);
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = counter_start(&prom, machine, PROM_SIZE);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 2);

    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 1);
    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 0);
    /* The stops are asleep: the node still counts. */
    wait_count_advances(qts, 2);
    wait_count_advances(qts, 0);
    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 0);
    wait_halted(qts, 2, true);
    wait_halted(qts, 3, true);
    g_usleep((RESTART_DELAY_MS + 2500) * 1000);
    g_assert_true(cpu_halted(qts, 2));
    assert_count_frozen(qts, 2);
    wait_count_advances(qts, 0);

    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 1);
    wait_restart_runs(qts, 2, 2);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A second reset while the first is still stopping the processors takes
 * over: the node restarts once, and a processor that has started is not
 * reset again by the first reset's work, which would drop its count.
 */
static void test_restart_in_flight_restarts_once(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *machine = g_strdup_printf("%s -global "
        "sgi-sn1-node.x-test-stop-delay-ms=500 -global "
        "sgi-sn1-node.x-test-start-delay-ms=%d", MACHINE_ARGS,
        RESTART_DELAY_MS);
    QTestState *qts;
    int64_t until;
    uint64_t last;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = counter_start(&prom, machine, PROM_SIZE);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 2);

    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 1);
    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 0);
    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 1);
    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 0);
    wait_restart_window(qts, 2, 2);
    wait_restart_runs(qts, 2, 2);
    last = node_count(qts, 2);

    /* The first restart's stops and starts arrive within this window. */
    until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < until) {
        uint64_t now = node_count(qts, 2);

        g_assert_cmpuint(now, >=, last);
        last = now;
        g_usleep(5000);
    }
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A node migrated while its restart is still stopping the processors is
 * restarted by the destination when it runs: the stops queued on the source
 * do not travel, so the stream says the restart is owed. The source holds the
 * stops back (x-test-stop-queue-delay-ms) so that its processors are still
 * counting when the stream is taken; the destination holds the starts back so
 * that the restart it runs can be seen as a held processor with a cleared
 * count.
 */
static void test_restart_survives_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-restart-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *held = g_strdup_printf("%s -global "
        "sgi-sn1-node.x-test-stop-queue-delay-ms=60000", MACHINE_ARGS);
    g_autofree char *delayed = restart_machine(MACHINE_ARGS);
    QTestState *from, *to;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    g_assert_no_error(error);
    from = counter_start(&prom, held, PROM_SIZE);
    quoted = g_shell_quote(prom);
    to = qtest_initf("%s -bios %s -accel tcg,thread=multi -no-shutdown -S "
                     "-incoming defer", delayed, quoted);
    qtest_qmp_assert_success(from, "{'execute':'cont'}");
    while (node_count(from, 2) < 3000000) {
        g_usleep(10000);
    }

    qtest_set_irq_in(from, "/machine/node1", "reset", 0, 1);
    qtest_set_irq_in(from, "/machine/node1", "reset", 0, 0);
    /* Nothing is queued yet: the processor goes on counting. */
    wait_count_advances(from, 2);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");

    wait_restart_window(to, 2, 2);
    wait_restart_runs(to, 2, 2);
    wait_count_advances(to, 0);
    qtest_quit(to);
    qtest_quit(from);
    g_unlink(socket);
    g_rmdir(dir);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * What a restart keeps, on the accelerator qtest uses, where it runs at
 * once. The node's parts are reset as a supply cycle resets them, so local
 * block scratch 0 and the memory unit's configuration are cleared, while
 * scratch 1 and 2 (the kernel's restart word) and RAM keep what they held.
 * The other node is not touched at all, identity word included: a machine
 * reset would show as its memory configuration falling back to the default.
 */
static void test_restart_keeps_the_survivors(void)
{
    QTestState *qts = machine_start();
    uint64_t revision0 = qtest_readq(qts, BEDROCK_LOCAL + LB_REVISION);
    uint64_t revision1 = peer_access(qts, LB_REVISION, false, 0);

    peer_access(qts, LB_SCRATCH0, true, 0x77);
    peer_access(qts, LB_SCRATCH1, true, 0x88);
    qtest_writeq(qts, BEDROCK_LOCAL + LB_SCRATCH0, 0x55);
    g_assert_cmphex(peer_access(qts, LB_SCRATCH0, false, 0), ==, 0x77);

    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 1);
    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 0);

    g_assert_cmphex(peer_access(qts, LB_SCRATCH0, false, 0), ==, 0);
    g_assert_cmphex(peer_access(qts, LB_SCRATCH1, false, 0), ==, 0x88);
    g_assert_cmphex(peer_access(qts, LB_SCRATCH2, false, 0), ==, SCRATCH1);
    g_assert_cmphex(peer_access(qts, MD_MEMORY_CONFIG, false, 0), !=,
                    MD_INSTALLED_SELECT2);
    g_assert_cmphex(peer_access(qts, LB_REVISION, false, 0), ==, revision1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + LB_REVISION), ==,
                    revision0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + LB_SCRATCH0), ==, 0x55);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + LB_SCRATCH2), ==,
                    SCRATCH0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + MD_MEMORY_CONFIG), ==,
                    MD_INSTALLED_SELECT2);
    g_assert_cmphex(qtest_readq(qts, PROBE), ==, WORD0);
    qtest_quit(qts);
}

/*
 * The same reset leaves a neighbour whose supply is off alone, on the
 * accelerator qtest uses.
 */
static void test_restart_ignores_an_off_node(void)
{
    QTestState *qts = machine_start();

    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 0);
    g_assert_true(cpu_halted(qts, 2));
    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 1);
    qtest_set_irq_in(qts, "/machine/node1", "reset", 0, 0);
    g_assert_true(cpu_halted(qts, 2));
    g_assert_true(cpu_halted(qts, 3));
    qtest_set_irq_in(qts, "/machine/node0", "reset", 0, 1);
    qtest_set_irq_in(qts, "/machine/node0", "reset", 0, 0);
    g_assert_true(cpu_halted(qts, 2));
    g_assert_false(cpu_halted(qts, 0));
    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 1);
    g_assert_false(cpu_halted(qts, 2));
    qtest_quit(qts);
}

/*
 * The kernel's reset request reaches the node's own L1 through its UART
 * window (destination 0x0ffff006, the general local requests), from every
 * processor of the node and repeatedly: one request
 * restarts the node, more of them while it is stopped or after it has
 * started restart it again, and the other node is never touched.
 */
#define L1_UART_WINDOW (HUB_LOCAL_REGISTERS + UINT64_C(0x80))
#define L1_REQUEST_RESET 0x2001

static void send_reset_request(QTestState *qts, uint64_t window)
{
    uint8_t request[8] = { 0x10, 0x0f, 0xff, 0xf0, 0x06,
                           L1_REQUEST_RESET >> 8, L1_REQUEST_RESET & 0xff, 0 };

    sgi_l1_test_send(qts, window, request, sizeof(request), false);
}

static void test_l1_reset_request_restarts_its_node(void)
{
    g_autofree char *quoted = g_shell_quote(prom_path);
    g_autofree char *machine = restart_machine(MACHINE_ARGS);
    QTestState *qts = qtest_initf("%s -bios %s", machine, quoted);

    g_assert_cmphex(halted_cpus(qts, 0), ==, 0);
    send_reset_request(qts, L1_UART_WINDOW);
    g_assert_cmphex(halted_cpus(qts, 0), ==, 3);
    g_assert_cmphex(halted_cpus(qts, 1), ==, 0);
    /* More requests while the processors are stopped are harmless. */
    send_reset_request(qts, L1_UART_WINDOW);
    send_reset_request(qts, L1_UART_WINDOW);
    g_assert_cmphex(halted_cpus(qts, 0), ==, 3);
    g_assert_cmphex(halted_cpus(qts, 1), ==, 0);
    wait_halted_mask(qts, 0, 0);

    /* A request after the node has started restarts it again. */
    send_reset_request(qts, L1_UART_WINDOW);
    g_assert_cmphex(halted_cpus(qts, 0), ==, 3);
    wait_halted_mask(qts, 0, 0);
    g_assert_cmphex(halted_cpus(qts, 1), ==, 0);
    qtest_quit(qts);
}

/*
 * The L1 of each Origin 3200 brick restarts its own node. "reset" acts on
 * the brick typed at, "ctc reset" on the peer over the L1 link, and "*
 * reset" on both. Nothing else is touched. Run on the accelerator qtest
 * uses, where a restarted node's processors stay halted while the starts
 * are owed (x-test-start-delay-ms) and a node that was not restarted shows
 * none halted: a brick that is not named stays as it is and the machine
 * does not reset.
 */
typedef struct L1ResetCase {
    const char *command;
    bool node0;
    bool node1;
} L1ResetCase;

static const L1ResetCase l1_reset_own = { "reset", true, false };
static const L1ResetCase l1_reset_peer = { "ctc reset", false, true };
static const L1ResetCase l1_reset_both = { "* reset", true, true };

static void test_l1_reset_restarts_its_node(const void *data)
{
    const L1ResetCase *c = data;
    g_autofree char *prom = create_counter_prom(PROM_SIZE);
    g_autofree char *extra = g_strdup_printf("-global "
        "sgi-sn1-node.x-test-start-delay-ms=%d", RESTART_DELAY_MS);
    g_autofree char *line = g_strdup_printf("\024%s\r\024power check\r",
                                            c->command);
    QTestState *qts;
    int console;

    qts = origin3200_console_start(&console, prom, extra);
    g_assert_cmphex(halted_cpus(qts, 0), ==, 0);
    g_assert_cmphex(halted_cpus(qts, 1), ==, 0);
    l1_console(qts, console, line, "power appears on");
    g_assert_cmphex(halted_cpus(qts, 0), ==, c->node0 ? BRICK_CPUS : 0);
    g_assert_cmphex(halted_cpus(qts, 1), ==, c->node1 ? BRICK_CPUS : 0);
    /* The starts arrive once the delay has passed. */
    if (c->node0) {
        wait_halted_mask(qts, 0, 0);
    }
    if (c->node1) {
        wait_halted_mask(qts, 1, 0);
    }
    close(console);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * An L1 does not restart a node whose brick is off: its reset is refused
 * with the supply down, and the brick that is on can still be reset without
 * switching the other on. That brick's reset reaches every powered compute
 * node, so the machine resets as a whole and keeps the off brick off.
 */
static void test_l1_reset_leaves_an_off_brick_off(void)
{
    g_autofree char *prom = create_counter_prom(PROM_SIZE);
    g_autofree char *extra = g_strdup_printf("-global "
        "sgi-sn1-node.x-test-start-delay-ms=%d", RESTART_DELAY_MS);
    QTestState *qts;
    int console;

    qts = origin3200_console_start(&console, prom, extra);
    l1_console(qts, console, "\024ctc power down\r\024ctc power check\r",
               "power appears off");
    g_assert_cmphex(halted_cpus(qts, 1), ==, BRICK_CPUS);
    l1_console(qts, console, "\024ctc reset\r\024* reset\r\024power check\r",
               "power appears on");
    qtest_qmp_eventwait(qts, "RESET");
    wait_halted_mask(qts, 0, 0);
    /* The reset has run; the off brick was not started by it. */
    g_assert_cmphex(halted_cpus(qts, 1), ==, BRICK_CPUS);

    l1_console(qts, console, "\024ctc power up\r\024ctc power check\r",
               "power appears on");
    wait_halted_mask(qts, 1, 0);
    close(console);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * Bedrock resets reach nodes by the registers the PROM programs. Processor
 * @cpu writes its own hub: the network interface's reset enable, then port
 * reset (bit 1, follow the link) with or without local reset (bit 2). The
 * peer takes the warm reset only while its own enable is set; otherwise it
 * keeps running and latches port errors bit 52. Two nodes with their enables
 * set are every powered compute node, so the machine resets as a whole
 * (RESET event), keeping each L1's power state.
 */
#define NI_PORT_RESET UINT64_C(0x680008)
#define NI_RESET_ENABLE UINT64_C(0x680010)
#define NI_PORT_ERRORS UINT64_C(0x680100)
#define NI_LINK_RESET_IN (UINT64_C(1) << 52)
#define PARTITION_MARK UINT64_C(0x4000000fff)
#define RESTART_WORD UINT64_C(0x0ead0030)
#define SENTINEL0 UINT64_C(0x00000000a5a5a5a5)

static void hub_write(QTestState *qts, unsigned int cpu, uint64_t offset,
                      uint64_t value)
{
    bte_cpu_writeq(qts, cpu, BEDROCK_LOCAL + offset, value);
}

static uint64_t hub_read(QTestState *qts, unsigned int cpu, uint64_t offset)
{
    return bte_cpu_readq(qts, cpu, BEDROCK_LOCAL + offset);
}

/* Both nodes counting, the survivors and a clearable word set as a kernel's. */
static QTestState *bedrock_reset_start(char **prom, const char *machine)
{
    QTestState *qts = counter_start(prom, machine, PROM_SIZE);

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 0);
    wait_count_advances(qts, 2);
    for (unsigned int node = 0; node < 2; node++) {
        hub_write(qts, node * 2, LB_SCRATCH0, SENTINEL0 + node);
        hub_write(qts, node * 2, LB_SCRATCH1, PARTITION_MARK);
        hub_write(qts, node * 2, LB_SCRATCH2, RESTART_WORD + node);
    }
    return qts;
}

static void assert_no_machine_reset(QTestState *qts)
{
    g_autoptr(QDict) reset = qtest_qmp_event_ref(qts, "RESET");

    g_assert_null(reset);
}

static void test_bedrock_reset_reaches_every_node(const void *data)
{
    unsigned int writer = GPOINTER_TO_UINT(data);
    g_autofree char *prom = NULL;
    g_autofree char *machine = restart_machine(MACHINE_ARGS);
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = bedrock_reset_start(&prom, machine);
    /* The PROM's end state for an unpartitioned pair: both enables set. */
    hub_write(qts, 0, NI_RESET_ENABLE, 1);
    hub_write(qts, 2, NI_RESET_ENABLE, 1);

    hub_write(qts, writer, NI_RESET_ENABLE, 1);
    hub_write(qts, writer, NI_PORT_RESET, 6);
    qtest_qmp_eventwait(qts, "RESET");
    wait_restart_runs(qts, 0, 2);
    wait_restart_runs(qts, 2, 2);
    for (unsigned int node = 0; node < 2; node++) {
        /* The local block's scratch 0 cleared; 1 and 2 are kept. */
        g_assert_cmphex(hub_read(qts, node * 2, LB_SCRATCH0), ==, 0);
        g_assert_cmphex(hub_read(qts, node * 2, LB_SCRATCH1), ==,
                        PARTITION_MARK);
        g_assert_cmphex(hub_read(qts, node * 2, LB_SCRATCH2), ==,
                        RESTART_WORD + node);
        g_assert_cmphex(hub_read(qts, node * 2, NI_PORT_ERRORS), ==, 0);
    }
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * The peer's enable is clear: only the writer's node restarts, the peer keeps
 * counting with its registers and identity as they were, latches bit 52 and
 * nothing else, and the machine does not reset. Its link stays up.
 */
static void test_bedrock_reset_leaves_a_disabled_peer_running(const void *data)
{
    unsigned int writer = GPOINTER_TO_UINT(data);
    unsigned int peer = writer ? 0 : 2;
    g_autofree char *prom = NULL;
    g_autofree char *machine = restart_machine(MACHINE_ARGS);
    QTestState *qts;
    uint64_t identity, status;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = bedrock_reset_start(&prom, machine);
    hub_write(qts, peer, NI_RESET_ENABLE, 0);
    identity = hub_read(qts, peer, LB_REVISION);
    status = hub_read(qts, peer, 0);
    g_assert_cmphex(status & 2, ==, 2);

    hub_write(qts, writer, NI_RESET_ENABLE, 1);
    hub_write(qts, writer, NI_PORT_RESET, 6);
    wait_restart_window(qts, writer, 2);
    g_assert_false(cpu_halted(qts, peer));
    wait_count_advances(qts, peer);
    g_assert_true(machine_running(qts));
    g_assert_cmphex(hub_read(qts, peer, NI_PORT_ERRORS), ==, NI_LINK_RESET_IN);
    g_assert_cmphex(hub_read(qts, peer, LB_SCRATCH0), ==,
                    SENTINEL0 + (writer ? 0 : 1));
    g_assert_cmphex(hub_read(qts, peer, LB_SCRATCH1), ==, PARTITION_MARK);
    g_assert_cmphex(hub_read(qts, peer, LB_SCRATCH2), ==,
                    RESTART_WORD + (writer ? 0 : 1));
    g_assert_cmphex(hub_read(qts, peer, NI_RESET_ENABLE), ==, 0);
    g_assert_cmphex(hub_read(qts, peer, LB_REVISION), ==, identity);
    /* The link reads up at the survivor, as it must for a later bus error. */
    g_assert_cmphex(hub_read(qts, peer, 0), ==, status);
    g_assert_cmphex(hub_read(qts, writer, NI_PORT_ERRORS), ==, 0);
    wait_restart_runs(qts, writer, 2);
    g_assert_cmphex(hub_read(qts, writer, LB_SCRATCH0), ==, 0);
    g_assert_cmphex(hub_read(qts, writer, LB_SCRATCH2), ==,
                    RESTART_WORD + (writer ? 1 : 0));
    wait_count_advances(qts, peer);
    assert_no_machine_reset(qts);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A local reset restarts the writer's node whatever its reset enable says,
 * and has no link effect: the peer is not marked or restarted.
 */
static void test_bedrock_local_reset_ignores_the_enable(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *machine = restart_machine(MACHINE_ARGS);
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = bedrock_reset_start(&prom, machine);
    hub_write(qts, 0, NI_RESET_ENABLE, 0);
    hub_write(qts, 2, NI_RESET_ENABLE, 1);
    hub_write(qts, 0, NI_PORT_RESET, 4);
    wait_restart_window(qts, 0, 2);
    g_assert_false(cpu_halted(qts, 2));
    wait_count_advances(qts, 2);
    g_assert_cmphex(hub_read(qts, 2, NI_PORT_ERRORS), ==, 0);
    g_assert_cmphex(hub_read(qts, 2, LB_SCRATCH0), ==, SENTINEL0 + 1);
    wait_restart_runs(qts, 0, 2);
    assert_no_machine_reset(qts);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A port reset alone follows the link and does not reset the writer: the
 * peer with its enable set restarts, as does everything that is powered, so
 * the reach is one node of two and only that node restarts.
 */
static void test_bedrock_port_reset_alone_restarts_the_peer(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *machine = restart_machine(MACHINE_ARGS);
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = bedrock_reset_start(&prom, machine);
    hub_write(qts, 2, NI_RESET_ENABLE, 1);
    hub_write(qts, 0, NI_PORT_RESET, 2);
    wait_restart_window(qts, 2, 2);
    g_assert_false(cpu_halted(qts, 0));
    wait_count_advances(qts, 0);
    g_assert_cmphex(hub_read(qts, 0, LB_SCRATCH0), ==, SENTINEL0);
    wait_restart_runs(qts, 2, 2);
    assert_no_machine_reset(qts);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A node whose supply is off is never reached. The writer's node is then
 * every powered compute node, so the machine resets as a whole and the off
 * node is not started by it: the machine keeps each L1's power state.
 */
static void test_bedrock_reset_never_reaches_an_off_node(void)
{
    g_autofree char *prom = create_counter_prom(PROM_SIZE);
    QTestState *qts;
    int console;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = origin3200_counter_start(&console, prom, "");
    console_steps_clock = false;
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 0);
    wait_count_advances(qts, 4);
    l1_console(qts, console, "\024ctc power down\r\024ctc power check\r",
               "power appears off");
    wait_halted_mask(qts, 1, BRICK_CPUS);

    hub_write(qts, 0, NI_RESET_ENABLE, 1);
    hub_write(qts, 0, NI_PORT_RESET, 6);
    qtest_qmp_eventwait(qts, "RESET");
    wait_halted_mask(qts, 0, 0);
    wait_count_advances(qts, 0);
    g_usleep(500 * 1000);
    g_assert_cmphex(halted_cpus(qts, 1), ==, BRICK_CPUS);
    l1_console(qts, console, "\024ctc power check\r", "power appears off");
    l1_console(qts, console, "\024power check\r", "power appears on");

    l1_console(qts, console, "\024ctc power up\r\024ctc power check\r",
               "power appears on");
    wait_halted_mask(qts, 1, 0);
    close(console);
    console_steps_clock = true;
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * The L1's restart reaches a running node: the processors of the brick
 * count again from the reset vector while the other brick's count never
 * stops.
 */
static void test_l1_reset_restarts_a_counting_node(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *extra = g_strdup_printf("-global "
        "sgi-sn1-node.x-test-start-delay-ms=%d", RESTART_DELAY_MS);
    QTestState *qts;
    int console;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    prom = create_counter_prom(PROM_SIZE);
    qts = origin3200_counter_start(&console, prom, extra);
    console_steps_clock = false;
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_count_advances(qts, 0);
    wait_count_advances(qts, 4);

    l1_console(qts, console, "\024reset\r\024power check\r",
               "power appears on");
    wait_restart_window(qts, 0, 4);
    g_assert_false(cpu_halted_in(qts, 4, 4));
    wait_count_advances(qts, 4);
    g_assert_true(machine_running(qts));
    wait_restart_runs(qts, 0, 4);
    close(console);
    console_steps_clock = true;
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A node cycled while its processors run outside the PROM's cache-as-RAM
 * phase must serve that phase again when they start. The phase gives each
 * processor a private block at physical 0x100000 for its stack
 * (hw/mips/sgi/sn-promdata.c) before the memory unit has any bank enabled.
 * Whether the window answers is state of the processor's block, not of the
 * memory unit: a machine reset re-arms it, and a node's own cold reset must
 * as well. Otherwise a processor that left the phase before the cycle (the
 * PROM monitor and the kernel both run outside it) finds the window gone
 * after it, and its first stack access reaches the memory unit that the
 * cycle has just reset.
 *
 * The image's processor A of each node enters the phase from the PROM and
 * stores into the window, then leaves it by clearing the phase bit and
 * spinning in its node's memory. Once the node's poll has seen it leave, the
 * test marks local block scratch 2, which the cycle keeps, and cycles the
 * node.
 * The second pass enters the phase again, stores 0x2222 into the window
 * and loads it back into the count register. A load that faults takes the
 * general exception vector, which stores 0xbad there instead.
 */
#define CAR_PHASE_BIT 2
#define CAR_LATCHED 1
#define CAR_STORED UINT64_C(0x2222)
#define CAR_FAULTED UINT64_C(0xbad)
#define CAR_SPIN UINT64_C(0x01000000)

static char *create_cache_as_ram_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    unsigned int pc;
    int fd;

#define EMIT(word) do { stl_be_p(image + pc, (word)); pc += 4; } while (0)
#define BRANCH(op, rs, rt, target) \
    EMIT(mips_branch((op), (rs), (rt), ((int)(target) - (int)pc - 4) / 4))
    stl_be_p(image + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    for (pc = 0x200; pc <= 0x300; pc += 0x80) {
        stl_be_p(image + pc, 0x1000ffff); /* beq zero, zero, self */
    }
    /* General exception: report the fault through the count register. */
    pc = 0x380;
    EMIT(mips_ori(11, 0, CAR_FAULTED));     /* ori t3, zero, 0xbad */
    EMIT(mips_sd(11, 8, COUNT_SLICE_A));    /* sd t3, 0x108(t0) */
    EMIT(0x1000ffff);                       /* beq zero, zero, self */
    EMIT(mips_nop());

    pc = 0x400;
    EMIT(mips_mfc0(12, 12));                /* mfc0 t4, Status */
    EMIT(mips_ori(12, 12, 0x0080));         /* Status.KX */
    EMIT(mips_lui(13, 0x2000));             /* Status.CU1 */
    EMIT(mips_rtype(12, 13, 12, 0, 0x25));  /* or t4, t4, t5 */
    EMIT(mips_mtc0(12, 12));                /* mtc0 t4, Status */
    EMIT(mips_nop());
    EMIT(mips_nop());
    EMIT(mips_lui(8, 0x9200));              /* t0: the local Bedrock */
    EMIT(mips_dsll32(8, 8));
    EMIT(mips_lui(9, 0x0100));
    EMIT(mips_rtype(8, 9, 8, 0, 0x25));     /* or t0, t0, t1 */
    EMIT(mips_ld(10, 8, 0x20));             /* ld t2, CPU number */
    EMIT(mips_andi(10, 10, 3));
    BRANCH(MIPS_OPC_BNE, 10, 0, 0x500);
    EMIT(mips_nop());
    /* Processor A: local block scratch 2 says which pass this is. */
    EMIT(mips_lui(14, 0x0060));
    EMIT(mips_ori(14, 14, 0x8010));
    EMIT(mips_daddu(14, 14, 8));
    EMIT(mips_ld(13, 14, 0));               /* ld t5, scratch 2 */
    EMIT(mips_ori(10, 0, CAR_PHASE_BIT));
    EMIT(0x44aa0000);                       /* dmtc1 t2, f0: enter phase */
    EMIT(mips_lui(9, 0x8010));              /* t1: cached window */
    BRANCH(MIPS_OPC_BNE, 13, 0, 0x480);
    EMIT(mips_nop());
    /* First pass: latch the window, then leave the phase outside the PROM. */
    EMIT(mips_ori(10, 0, 0x1111));
    EMIT(mips_sw(10, 9, 0));
    EMIT(mips_ori(11, 0, CAR_LATCHED));
    EMIT(mips_sd(11, 8, COUNT_SLICE_A));
    EMIT(0x44a00000);                       /* dmtc1 zero, f0 */
    EMIT(mips_lui(9, 0xa100));              /* uncached CAR_SPIN */
    EMIT(mips_rtype(9, 0, 0, 0, 0x08));     /* jr t1 */
    EMIT(mips_nop());
    g_assert_cmpuint(pc, <=, 0x480);
    /* Second pass: store into the window and read it back. */
    pc = 0x480;
    EMIT(mips_ori(10, 0, CAR_STORED));
    EMIT(mips_sw(10, 9, 0));
    EMIT(mips_lw(11, 9, 0));
    EMIT(mips_nop());
    EMIT(mips_sd(11, 8, COUNT_SLICE_A));
    EMIT(0x1000ffff);                       /* beq zero, zero, self */
    EMIT(mips_nop());
    /* The other processors wait. */
    pc = 0x500;
    EMIT(0x42000020);                       /* wait */
    EMIT(0x1000fffe);                       /* beq zero, zero, 0x500 */
    EMIT(mips_nop());
#undef BRANCH
#undef EMIT

    fd = g_file_open_tmp("sn1-cross-node-car-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static bool car_left_traced(const char *path, unsigned int cpu)
{
    g_autofree char *contents = NULL;
    g_autofree char *needle = g_strdup_printf("sn_promdata_phase cpu %u leave",
                                              cpu);

    return g_file_get_contents(path, &contents, NULL, NULL) &&
           strstr(contents, needle) != NULL;
}

typedef void (*CarCycle)(QTestState *qts, unsigned int node);

static void car_power_cycle(QTestState *qts, unsigned int node)
{
    g_autofree char *path = g_strdup_printf("/machine/node%u", node);

    qtest_set_irq_in(qts, path, "power", 0, 0);
    qtest_set_irq_in(qts, path, "power", 0, 1);
}

static void car_restart(QTestState *qts, unsigned int node)
{
    g_autofree char *path = g_strdup_printf("/machine/node%u", node);

    qtest_set_irq_in(qts, path, "reset", 0, 1);
    qtest_set_irq_in(qts, path, "reset", 0, 0);
}

static void check_cycle_rearms_the_cache_as_ram_window(unsigned int node,
                                                       CarCycle cycle)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *prom = create_cache_as_ram_prom();
    g_autofree char *quoted = g_shell_quote(prom);
    g_autofree char *dir = g_dir_make_tmp("sn1-car-cycle-XXXXXX", &error);
    g_autofree char *trace = NULL;
    unsigned int cpu = node * 2;
    g_autoptr(QDict) reset = NULL;
    QTestState *qts;
    uint64_t count;
    SgiWait w;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    g_assert_no_error(error);
    trace = g_build_filename(dir, "trace", NULL);
    qts = qtest_initf("%s -bios %s -accel tcg,thread=multi -no-shutdown -S "
                      "-trace enable=sn_promdata_phase,file=%s",
                      MACHINE_ARGS, quoted, trace);
    /* The spin loop of the node's first pass: beq zero, zero, self; nop. */
    bte_cpu_writeq(qts, cpu, BEDROCK_LOCAL + MD_MEMORY_CONFIG,
                   MD_INSTALLED_SELECT2);
    bte_cpu_writeq(qts, cpu, CAR_SPIN, UINT64_C(0x1000ffff00000000));
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");

    sgi_wait_start(&w, "processor %u to leave the cache-as-RAM phase", cpu);
    while (node_count(qts, cpu) != CAR_LATCHED ||
           !car_left_traced(trace, cpu)) {
        g_assert_true(sgi_wait_continue(&w, 10000));
    }

    bte_cpu_writeq(qts, cpu, BEDROCK_LOCAL + LB_SCRATCH2, 1);
    cycle(qts, node);

    sgi_wait_start(&w, "processor %u's second pass to report", cpu);
    while ((count = node_count(qts, cpu)) != CAR_STORED &&
           count != CAR_FAULTED) {
        g_assert_true(sgi_wait_continue(&w, 10000));
    }
    g_assert_cmphex(count, ==, CAR_STORED);
    reset = qtest_qmp_event_ref(qts, "RESET");
    g_assert_null(reset);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_unlink(trace);
    g_rmdir(dir);
}

static void test_restart_rearms_the_cache_as_ram_window(void)
{
    check_cycle_rearms_the_cache_as_ram_window(0, car_restart);
}

static void test_power_return_rearms_the_cache_as_ram_window(void)
{
    check_cycle_rearms_the_cache_as_ram_window(1, car_power_cycle);
}

/*
 * A node board with no processor (the Origin 3900 memory node, node 3 here)
 * has nothing to stop, so a restart or a supply return of it must finish
 * from the call itself: the processors' last stop is what finishes one
 * otherwise, and with none it never comes. Nothing on the node runs, so
 * qtest reaches it as the PROM would, over the router with a vector access
 * from node 0.
 */
#define MEMORY_NODE_ARGS "-M origin3000,topology=origin3900-memory-node " \
                         "-cpu R16000 -smp 12 -m 8192"
#define MEMORY_NODE 3
#define MEMORY_NODE_ROUTE UINT64_C(5)
#define LB_SOFT_RESET UINT64_C(0x604018)

static uint64_t memory_node_access(QTestState *qts, uint64_t hub_offset,
                                   bool write, uint64_t data)
{
    (void)qtest_readq(qts, BEDROCK_LOCAL + LB_VECTOR_STATUS_CLEAR);
    qtest_writeq(qts, BEDROCK_LOCAL + LB_VECTOR_ROUTE, MEMORY_NODE_ROUTE);
    qtest_writeq(qts, BEDROCK_LOCAL + LB_VECTOR_DATA, data);
    qtest_writeq(qts, BEDROCK_LOCAL + LB_VECTOR_PARAMS,
                 (hub_offset & UINT64_C(0xfffff8)) | VECTOR_REQUEST(0x1c8) |
                 (write ? VECTOR_WRITE : 0));
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + LB_VECTOR_STATUS) &
                    VECTOR_VALID, ==, VECTOR_VALID);
    return qtest_readq(qts, BEDROCK_LOCAL + LB_VECTOR_READ);
}

/*
 * The sections the migration stream of @qts names. The stream is the
 * machine's whole RAM, so only its tail is kept, saved to @path: the
 * description of every section the stream carries ends it. The stream is
 * checked for @control, which the node always writes, so that a tail that
 * lost the description cannot pass for one that lacks a section. Returns
 * the tail, to search with stream_names().
 */
static char *stream_tail(QTestState *qts, const char *path, gsize *length)
{
    g_autofree char *uri = g_strdup_printf("exec:tail -c 4000000 >%s", path);
    char *contents = NULL;
    SgiWait w;

    qtest_qmp_assert_success(qts, "{'execute':'migrate','arguments':"
                             "{'uri':%s}}", uri);
    sgi_wait_migration(qts, "completed");
    /* The tail may still be writing: the description ends in "]}". */
    sgi_wait_start(&w, "the stream's description to be saved");
    for (;;) {
        g_free(contents);
        contents = NULL;
        if (g_file_get_contents(path, &contents, length, NULL) &&
            *length >= 2 && !memcmp(contents + *length - 2, "]}", 2)) {
            break;
        }
        g_assert_true(sgi_wait_continue(&w, 10000));
    }
    g_assert_true(memmem(contents, *length, "sn1-node", 8));
    return contents;
}

/* Whether @tail names @section; it holds binary state, so no strstr(). */
static bool stream_names(const char *tail, gsize length, const char *section)
{
    return memmem(tail, length, section, strlen(section)) != NULL;
}

static QTestState *memory_node_start(char **prom, char **stream)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-memory-node-XXXXXX", &error);
    g_autofree char *quoted = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    int fd = g_file_open_tmp("sn1-memory-node-prom-XXXXXX", prom, &error);

    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    *stream = g_build_filename(dir, "stream", NULL);
    quoted = g_shell_quote(*prom);
    return qtest_initf(MEMORY_NODE_ARGS " -bios %s -nodefaults -display none",
                       quoted);
}

static void memory_node_quit(QTestState *qts, char *prom, char *stream)
{
    g_autofree char *dir = g_path_get_dirname(stream);

    qtest_quit(qts);
    g_unlink(stream);
    g_rmdir(dir);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_free(prom);
    g_free(stream);
}

/*
 * A reset that reaches the memory node alone restarts it: its parts are
 * reset as a supply cycle resets them, the compute nodes keep running and
 * the machine does not reset, and no restart stays owed to a migration.
 */
static void test_processorless_node_restart_finishes(void)
{
    char *prom, *stream, *tail;
    QTestState *qts = memory_node_start(&prom, &stream);
    gsize length;
    uint64_t before = memory_node_access(qts, MD_MEMORY_CONFIG, false, 0);
    QDict *reply = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                             "{'path':'/machine/node3','property':'nr-cpus'}}");

    g_assert_cmpint(qdict_get_int(reply, "return"), ==, 0);
    qobject_unref(reply);
    memory_node_access(qts, MD_MEMORY_CONFIG, true, MD_BANK0_512MB);
    g_assert_cmphex(memory_node_access(qts, MD_MEMORY_CONFIG, false, 0), ==,
                    MD_BANK0_512MB);

    memory_node_access(qts, LB_SOFT_RESET, true, 1);
    g_assert_cmphex(memory_node_access(qts, MD_MEMORY_CONFIG, false, 0), ==,
                    before);
    assert_no_machine_reset(qts);
    for (unsigned int node = 0; node < MEMORY_NODE; node++) {
        g_assert_cmphex(halted_cpus(qts, node), ==, 0);
    }
    tail = stream_tail(qts, stream, &length);
    g_assert_false(stream_names(tail, length, "sn1-node/restart"));
    g_free(tail);
    memory_node_quit(qts, prom, stream);
}

/* The same node's supply returning owes no start either. */
static void test_processorless_node_power_up_owes_no_start(void)
{
    char *prom, *stream, *tail;
    QTestState *qts = memory_node_start(&prom, &stream);
    gsize length;

    qtest_set_irq_in(qts, "/machine/node3", "power", 0, 0);
    qtest_set_irq_in(qts, "/machine/node3", "power", 0, 1);
    tail = stream_tail(qts, stream, &length);
    g_assert_false(stream_names(tail, length, "sn1-node/start"));
    g_free(tail);
    memory_node_quit(qts, prom, stream);
}

/*
 * A node restart pulses the reset input of the XBridge cabled to its I/O
 * interface, and a bridge that is not held restarts cold: on the two
 * partition Origin 3400 each C-brick's I-brick bridge sits on widget 15 of
 * its own node. Restarting node 0 clears what was programmed into its
 * bridge, and leaves node 1's bridge, its node and the machine alone. The
 * bridge's interrupt enable stands for the state the PROM finds again.
 */
#define TWO_PARTITIONS_ARGS \
    "-M origin3000,topology=origin3400-two-partitions " \
    "-cpu R12000 -smp 8 -m 8192"

/* Each node's own processor 0 reaches its bridge on widget 15, locally. */
#define CABLED_BRIDGE_ENABLE (XBRIDGE_F + XBRIDGE_INT_ENABLE + 4)

static uint32_t cabled_bridge_enable(QTestState *qts, unsigned int node)
{
    uint8_t bytes[4];

    g_assert_true(qtest_memread_cpu(qts, node * 4, CABLED_BRIDGE_ENABLE,
                                    bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

static void cabled_bridge_set_enable(QTestState *qts, unsigned int node,
                                     uint32_t value)
{
    uint8_t bytes[4];

    stl_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, node * 4, CABLED_BRIDGE_ENABLE,
                                     bytes, sizeof(bytes)));
}

static void test_restart_resets_the_cabled_bridge(void)
{
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(TWO_PARTITIONS_ARGS " -bios %s", quoted);

    for (unsigned int node = 0; node < 2; node++) {
        cabled_bridge_set_enable(qts, node, 0x5a);
        g_assert_cmphex(cabled_bridge_enable(qts, node), ==, 0x5a);
    }
    qtest_set_irq_in(qts, "/machine/node0", "reset", 0, 1);
    qtest_set_irq_in(qts, "/machine/node0", "reset", 0, 0);
    g_assert_cmphex(cabled_bridge_enable(qts, 0), ==, 0);
    g_assert_cmphex(cabled_bridge_enable(qts, 1), ==, 0x5a);
    g_assert_cmphex(halted_cpus(qts, 1), ==, 0);
    assert_no_machine_reset(qts);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    prom_path = create_prom();
    qtest_add_func("sn1/cross-node/restart-one-node", test_restart_one_node);
    qtest_add_func("sn1/cross-node/restart-rearms-the-cache-as-ram-window",
                   test_restart_rearms_the_cache_as_ram_window);
    qtest_add_func("sn1/cross-node/restart-does-not-start-an-off-node",
                   test_restart_does_not_start_an_off_node);
    qtest_add_func("sn1/cross-node/loss-overtakes-a-restart",
                   test_loss_overtakes_a_restart);
    qtest_add_func("sn1/cross-node/restart-in-flight-restarts-once",
                   test_restart_in_flight_restarts_once);
    qtest_add_func("sn1/cross-node/restart-survives-migration",
                   test_restart_survives_migration);
    qtest_add_func("sn1/cross-node/restart-keeps-the-survivors",
                   test_restart_keeps_the_survivors);
    qtest_add_func("sn1/cross-node/processorless-node-restart-finishes",
                   test_processorless_node_restart_finishes);
    qtest_add_func("sn1/cross-node/processorless-node-power-up-owes-no-start",
                   test_processorless_node_power_up_owes_no_start);
    qtest_add_func("sn1/cross-node/restart-resets-the-cabled-bridge",
                   test_restart_resets_the_cabled_bridge);
    qtest_add_func("sn1/cross-node/restart-ignores-an-off-node",
                   test_restart_ignores_an_off_node);
    qtest_add_data_func("sn1/cross-node/l1-reset-restarts-its-node/own",
                        &l1_reset_own, test_l1_reset_restarts_its_node);
    qtest_add_data_func("sn1/cross-node/l1-reset-restarts-its-node/peer",
                        &l1_reset_peer, test_l1_reset_restarts_its_node);
    qtest_add_data_func("sn1/cross-node/l1-reset-restarts-its-node/both",
                        &l1_reset_both, test_l1_reset_restarts_its_node);
    qtest_add_func("sn1/cross-node/l1-reset-request-restarts-its-node",
                   test_l1_reset_request_restarts_its_node);
    qtest_add_func("sn1/cross-node/l1-reset-leaves-an-off-brick-off",
                   test_l1_reset_leaves_an_off_brick_off);
    qtest_add_func("sn1/cross-node/l1-reset-restarts-a-counting-node",
                   test_l1_reset_restarts_a_counting_node);
    qtest_add_data_func("sn1/cross-node/bedrock-reset-reaches-every-node/"
                        "node0", GUINT_TO_POINTER(0),
                        test_bedrock_reset_reaches_every_node);
    qtest_add_data_func("sn1/cross-node/bedrock-reset-reaches-every-node/"
                        "node1", GUINT_TO_POINTER(2),
                        test_bedrock_reset_reaches_every_node);
    qtest_add_data_func("sn1/cross-node/bedrock-reset-leaves-a-disabled-peer/"
                        "node0", GUINT_TO_POINTER(0),
                        test_bedrock_reset_leaves_a_disabled_peer_running);
    qtest_add_data_func("sn1/cross-node/bedrock-reset-leaves-a-disabled-peer/"
                        "node1", GUINT_TO_POINTER(2),
                        test_bedrock_reset_leaves_a_disabled_peer_running);
    qtest_add_func("sn1/cross-node/bedrock-local-reset-ignores-the-enable",
                   test_bedrock_local_reset_ignores_the_enable);
    qtest_add_func("sn1/cross-node/bedrock-port-reset-alone-restarts-the-peer",
                   test_bedrock_port_reset_alone_restarts_the_peer);
    qtest_add_func("sn1/cross-node/bedrock-reset-never-reaches-an-off-node",
                   test_bedrock_reset_never_reaches_an_off_node);
    qtest_add_func("sn1/cross-node/owed-start-runs-once",
                   test_owed_start_runs_once);
    qtest_add_func("sn1/cross-node/reset-clears-an-owed-start",
                   test_reset_clears_an_owed_start);
    qtest_add_func("sn1/cross-node/load-clears-an-owed-start",
                   test_load_clears_an_owed_start);
    qtest_add_func("sn1/cross-node/node-power-halts-only-its-processors",
                   test_node_power_halts_only_its_processors);
    qtest_add_func("sn1/cross-node/node-power-stops-one-node",
                   test_node_power_stops_one_node);
    qtest_add_func("sn1/cross-node/off-node-survives-migration",
                   test_off_node_survives_migration);
    qtest_add_func("sn1/cross-node/power-return-rearms-the-cache-as-ram-"
                   "window",
                   test_power_return_rearms_the_cache_as_ram_window);
    qtest_add_func("sn1/cross-node/brick-power-ends-the-machine-last",
                   test_brick_power_ends_the_machine_last);
    qtest_add_func("sn1/cross-node/broadcast-power-down-ends-the-machine",
                   test_broadcast_power_down_ends_the_machine);
    qtest_add_func("sn1/cross-node/brick-power-stops-one-counter",
                   test_brick_power_stops_one_counter);
    qtest_add_func("sn1/cross-node/last-brick-power-down-announces-after-"
                   "the-stop",
                   test_last_brick_power_down_announces_after_the_stop);
    qtest_add_func("sn1/cross-node/stop-cont-keeps-the-pending-announcement",
                   test_stop_cont_keeps_the_pending_announcement);
    qtest_add_func("sn1/cross-node/repeated-loss-announces-when-the-last-"
                   "stop-ran",
                   test_repeated_loss_announces_when_the_last_stop_ran);
    qtest_add_func("sn1/cross-node/load-into-off-nodes",
                   test_load_into_off_nodes);
    qtest_add_func("sn1/cross-node/load-into-on-nodes",
                   test_load_into_on_nodes);
    qtest_add_func("sn1/cross-node/load-keeps-running-processors",
                   test_load_keeps_running_processors);
    qtest_add_data_func("sn1/cross-node/unpowered-router-removes-its-memory",
                        "power",
                        test_dead_router_removes_the_memory_behind_it);
    qtest_add_data_func("sn1/cross-node/held-router-removes-its-memory",
                        "reset-hold",
                        test_dead_router_removes_the_memory_behind_it);
    qtest_add_func("sn1/cross-node/router-l1-starts-powered-and-released",
                   test_router_l1_starts_powered_and_released);
    qtest_add_func("sn1/cross-node/router-l1-power-follows-the-switch",
                   test_router_l1_power_follows_the_switch);
    qtest_add_func("sn1/cross-node/router-l1-starts-off-when-its-l1-does",
                   test_router_l1_starts_off_when_its_l1_does);
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
    qtest_add_func("sn1/cross-node/remote-probe-faults-and-machine-runs-on",
                   test_remote_probe_faults_and_machine_runs_on);
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
    qtest_add_func("sn1/cross-node/reset-page-is-node-zeros",
                   test_reset_page_is_node_zeros);
    qtest_add_func("sn1/cross-node/routed-reset-page-follows-the-route",
                   test_routed_reset_page_follows_the_route);
    qtest_add_func("sn1/cross-node/two-routers-route-by-entry-link",
                   test_two_routers_route_by_entry_link);
    qtest_add_func(
        "sn1/cross-node/origin3800-high-bandwidth/routes-over-ports-7-and-8",
        test_origin3800_routes_over_ports_7_and_8);
    qtest_add_data_func("sn1/cross-node/routers-reset-unprogrammed/one",
                        &(const RouterResetCase){ ORIGIN3400_ARGS, 1, { 0 } },
                        test_routers_reset_unprogrammed);
    qtest_add_data_func("sn1/cross-node/routers-reset-unprogrammed/two",
                        &(const RouterResetCase){ TWO_ROUTER_ARGS, 2,
                                                  { 0, 6 } },
                        test_routers_reset_unprogrammed);
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
                        "origin300-numalink-pci-expansion",
                        test_bte_memory_route);
    qtest_add_data_func("sn1/cross-node/unpowered-node-memory-is-absent/"
                        "direct", "origin300-two-chassis",
                        test_unpowered_node_memory_is_absent);
    qtest_add_data_func("sn1/cross-node/unpowered-node-memory-is-absent/"
                        "routed", "origin300-numalink-pci-expansion",
                        test_unpowered_node_memory_is_absent);
    qtest_add_data_func("sn1/cross-node/unpowered-node-survives-migration/"
                        "direct", "origin300-two-chassis",
                        test_unpowered_node_survives_migration);
    qtest_add_data_func("sn1/cross-node/unpowered-node-survives-migration/"
                        "routed", "origin300-numalink-pci-expansion",
                        test_unpowered_node_survives_migration);
    qtest_add_func("sn1/cross-node/unpowered-first-node-survives-migration",
                   test_unpowered_first_node_survives_migration);
    qtest_add_func("sn1/cross-node/bte-data-goes-by-number",
                   test_bte_data_goes_by_number);
    qtest_add_func("sn1/cross-node/every-processor-starts-through-its-router",
                   test_every_processor_starts_through_its_router);
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
