/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Which node an access reaches on a two-chassis Origin 300: the claim
 * layer in hw/mips/sgi/sn1-node.c, against the rule in Origami's
 * docs/designs/2026-09-25-origin300-two-node-addressing.md. A node field
 * names the node whose local block carries that number, the reader-relative
 * forms always reach the reader's own node, and a number nobody reachable
 * carries is a decode error.
 *
 * qtest's own accesses go to the global map, where each node sits at its
 * placement (node order << 33); a processor's view is read with the
 * monitor's xp and a cpu-index, which is how a test sees memory as one
 * processor does. Processors 0 and 1 are node 0's, 2 and 3 node 1's. The
 * PROM image is zeros, and under qtest no processor executes it.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "sgi-test.h"

#define MACHINE_ARGS "-M origin300,topology=origin300-two-chassis " \
                     "-cpu R14000 -smp 4 -m 1024"

#define NODE_SHIFT 33
#define NODE(n) ((uint64_t)(n) << NODE_SHIFT)
/* The Bedrock's node-named register form, and its local alias (IALIAS). */
#define BEDROCK_NODE(n) (SPACE_XTALK + NODE(n) + UINT64_C(0x01800000))
#define BEDROCK_LOCAL (SPACE_XTALK + UINT64_C(0x01000000))
#define HUB_LOCAL_REGISTERS (SPACE_HUB + UINT64_C(0x10000000))
#define LB_REVISION UINT64_C(0x600000)
#define LB_SCRATCH2 UINT64_C(0x608010)
#define MD_MEMORY_CONFIG UINT64_C(0x780008)
#define MD_INSTALLED_SELECT2 ((UINT64_C(2) << 32) | UINT64_C(0x3300))
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
 * qtest's own accesses go through processor 0's view, as a program on
 * node 0 would, so node 1 is reached the way the PROM reaches a peer before
 * it has a number: through node 0's vector engine on route 0. The launch
 * sequence is the firmware's (sn1-bedrock-link-test.c).
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
 * says which node answered. Node 1 is numbered 1 to reach it, then set
 * back to 0: both local blocks leave reset carrying 0.
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
 * them: each node reaches only itself at node field zero, and nothing
 * answers at node 1's placement.
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
 * The Bedrock's own register alias and the Hub special local register
 * window (read here through a processor LED) reach the reader's node,
 * whatever number it carries.
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
 * The cached alias: below the size node 1's processor interface carries,
 * node field zero is node 1's own memory; at and above it, node 0's. The
 * window follows the register as soon as it is written.
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
 * An interrupt packet the IO-8's XBridge sends goes to the node whose
 * local block carries the number its node field names: the XBridge head
 * at widget 0xf on node 0 is programmed, as IRIX programs it, to post to
 * the processor interface's pending-modify register in the register space
 * of a named node, then forced.
 */
#define XBRIDGE_F (SPACE_XTALK + UINT64_C(0xf) * UINT64_C(0x1000000))
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
 * The engines' data moves by the number a node carries, not by where the
 * node is placed in QEMU's global map, and without the reader-relative
 * cached alias. Node 1 renumbered to 5 is written at node field 5, where
 * the global map has nothing; node 1's own engine writing node field zero
 * with node 1's cached alias covering the line writes node 0's memory, as
 * a processor's cached alias does not apply to the engine.
 */
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
 * reader's own node's flash. Each node gets its own 16 MiB boot flash;
 * node 1's carries a marker at position zero, node 0's is blank.
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

static QTestState *machine_start_marked(char **flashes)
{
    g_autofree char *quoted = g_shell_quote(prom_path);

    flashes[0] = create_flash(0);
    flashes[1] = create_flash(PROM_MARKER);
    return qtest_initf(MACHINE_ARGS " -bios %s "
                       "-drive if=pflash,index=0,format=raw,file=%s "
                       "-drive if=pflash,index=1,format=raw,file=%s",
                       quoted, flashes[0], flashes[1]);
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
 * The L1 UART sits in the local register window at +0x80, a
 * reader-relative form: each node's processors reach their own chassis's
 * L1, processor interface 1's included, whose own register overlay leaves
 * that range to the form beneath it. The UART's scratch register tells
 * the two apart: node 0's is written through processor 0's local form,
 * node 1's through node field 1 once node 1 carries that number.
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
 * The PROM's cache-as-RAM phase keeps the PROM's RAM-copy range and the
 * data window reader-local; it has to end at the OS handoff even for a
 * processor that never touches the data window again. Otherwise, once the
 * nodes carry different numbers, a processor on node 1 storing to node
 * field zero in either range still lands in its own node's memory instead
 * of node 0's.
 *
 * Guest code does it: every processor reads a node tag from its own
 * memory; node 0's processors park, node 1's enter the phase exactly as
 * the PROM does (boot-status bit 1 in FPR 0 and a cached load in the data
 * window, running from the flash), withdraw both, and jump to a RAM loop
 * that never touches the window. There they report ready and wait for the
 * host to renumber node 1 and say go, then store a marker with node field
 * zero at the RAM-copy address and, uncached, in the data window.
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
    unsigned int tries;

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
    for (tries = 0; tries < 500; tries++) {
        if (cpu_readq(qts, 2, HANDOFF_READY, &value) && (value >> 32) == 1) {
            break;
        }
        g_usleep(10000);
    }
    g_assert_cmphex(value >> 32, ==, 1);
    /* Give the handoff time to be noticed: many of its poll periods. */
    g_usleep(500000);

    set_node_number(qts, 1, 1);
    qtest_writeq(qts, HANDOFF_GO, UINT64_C(1) << 32);
    for (tries = 0; tries < 500; tries++) {
        if ((qtest_readq(qts, HANDOFF_DONE) >> 32) == 1) {
            break;
        }
        g_usleep(10000);
    }
    g_assert_cmphex(qtest_readq(qts, HANDOFF_DONE) >> 32, ==, 1);
    g_assert_cmphex(qtest_readq(qts, PROM_DATA_WINDOW) >> 32, ==,
                    HANDOFF_MARKER);
    g_assert_cmphex(qtest_readq(qts, NODE(1) + PROM_DATA_WINDOW), ==, 0);
    g_assert_cmphex(qtest_readq(qts, PROM_RAM_COPY) >> 32, ==, HANDOFF_MARKER);
    g_assert_cmphex(qtest_readq(qts, NODE(1) + PROM_RAM_COPY), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    prom_path = create_prom();
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
    qtest_add_func("sn1/cross-node/remote-interrupt-reaches-the-peer",
                   test_remote_interrupt_reaches_the_peer);
    qtest_add_func("sn1/cross-node/bte-data-goes-by-number",
                   test_bte_data_goes_by_number);
    ret = g_test_run();
    g_unlink(prom_path);
    g_free(prom_path);
    return ret;
}
