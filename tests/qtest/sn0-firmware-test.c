/*
 * SGI SN0 synthesized firmware structures and cross-module relationships not
 * covered by the generated machine-graph qtests: the hardware directory,
 * GDA, ARCS private vector, and KLCONFIG memory layout versus Hub mode.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define GRAPH_COMPONENTS    0x4400
#define GRAPH_COMP_STRIDE   0x80
#define NODE_STRIDE         0x100000000ULL

#define DUAL_NODE_ARGS \
    "-M origin200 -m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"

#define COMP_MEMORY         3
#define COMP_MEMORY_INDEX   2

/* The NI status register; its more-nodes bit is the addressing mode. */
#define HUB_LOCAL           (0x20000000000ULL + 0x01000000)
#define NI_STATUS           0x600000
#define NI_MORE_NODES       (1ULL << 18)

/* Memory-component fields, in the eight-bank layout M mode calls for. */
#define MEM_TOTAL_MB        0x38
#define MEM_BANK_SIZES      0x3c
#define MEM_BANKS           8

#define GDA                 0x11000
#define GDA_SIGNATURE       0x58464552  /* "XFER" */
#define GDA_VERSION         2
#define GDA_NODE_TABLE      0x80
#define GDA_NODES           64      /* the count this family is built with */
#define GDA_NODE_ABSENT     0xffff

/*
 * The hardware directory: the NMI path's only route to its save blocks,
 * so an unwritten entry is not a cosmetic gap.
 */
#define HW_DIRECTORY        0x2000
#define HW_DIR_ENTRY_SIZE   0x40
#define HW_DIR_SIGNATURE    0x434d5f53505f5357ULL
#define HW_DIR_LAUNCH       0
#define HW_DIR_NMI          2
#define HW_DIR_FREEMEM      4
#define HW_DIR_DEBUG_STACKS 5
#define HW_DIR_PI_ERROR     6
#define HW_DIR_OFF_SIGNATURE 0x00
#define HW_DIR_OFF_OFFSET    0x08
#define HW_DIR_OFF_SIZE      0x18
#define HW_DIR_OFF_COUNT     0x20
#define HW_DIR_OFF_STRIDE    0x28
#define NMI_BLOCKS           0x3000
#define NMI_STRIDE           0x40

#define LAUNCH_BLOCKS        0x2400
#define LAUNCH_ENTRIES       2
#define LAUNCH_STRIDE        0x200
#define LAUNCH_DIR_SIZE      (LAUNCH_ENTRIES * LAUNCH_STRIDE)
#define LAUNCH_SIGNATURE     0xaddbead2addbead3ULL

#define FIRST_FREE           0x19000
#define FREEMEM_SIZE         0xffffffffffffffffULL

#define DEBUG_STACKS_BASE    0x25000
#define DEBUG_STACKS_SIZE    0xe000
#define DEBUG_STACKS_N       2
#define DEBUG_STACKS_STRIDE  0x7000

#define PI_ERROR_BASE        0x12000
#define PI_ERROR_SIZE        0x4000

/*
 * The private vector's stubs continue the main vector's, so its first
 * entry is checked against the main's last.
 */
#define ARCS_VECTOR          0x2800
#define ARCS_ENTRIES         35
#define ARCS_PRIVATE_VECTOR  0x2c00
#define ARCS_PRIVATE_ENTRIES 17
#define ARCS_STUB_SIZE       16
#define ARCS_TRAP_LOCAL      0x1fd00000
#define ARCS_CAPABILITY_OP   (ARCS_ENTRIES + 9) /* private entry at offset 0x48 */
#define ARCS_CAPABILITY_LEVEL 3

/* Component record fields this file reads. */
#define COMP_TYPE           0x00

static void test_gda_signature(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* These fields select the versioned node-table representation. */
    g_assert_cmphex(qtest_readl(qts, GDA), ==, GDA_SIGNATURE);
    g_assert_cmphex(qtest_readw(qts, GDA + 0x04), ==, GDA_VERSION);

    /*
     * Signed halfwords, the guest's way: a byte-per-entry table looks
     * right byte for byte and wrong to a halfword load.
     */
    g_assert_cmphex(qtest_readw(qts, GDA + GDA_NODE_TABLE), ==, 0);
    g_assert_cmphex(qtest_readw(qts, GDA + GDA_NODE_TABLE + 2), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, GDA + GDA_NODE_TABLE + 4), ==, 0xffff);

    qtest_quit(qts);
}

/*
 * Checked against the datasheet's table, not each other. OpenBSD stores
 * its NMI handler through the one live entry before the console exists.
 */
static void test_directory_entries(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t launch = HW_DIRECTORY + HW_DIR_LAUNCH * HW_DIR_ENTRY_SIZE;
    uint64_t nmi = HW_DIRECTORY + HW_DIR_NMI * HW_DIR_ENTRY_SIZE;
    uint64_t freemem = HW_DIRECTORY + HW_DIR_FREEMEM * HW_DIR_ENTRY_SIZE;
    uint64_t debug = HW_DIRECTORY + HW_DIR_DEBUG_STACKS * HW_DIR_ENTRY_SIZE;
    uint64_t pi_error = HW_DIRECTORY + HW_DIR_PI_ERROR * HW_DIR_ENTRY_SIZE;
    int i;

    g_assert_cmphex(qtest_readq(qts, launch + HW_DIR_OFF_SIGNATURE), ==,
                    HW_DIR_SIGNATURE);
    g_assert_cmphex(qtest_readq(qts, launch + HW_DIR_OFF_OFFSET), ==,
                    LAUNCH_BLOCKS);
    g_assert_cmphex(qtest_readq(qts, launch + HW_DIR_OFF_SIZE), ==,
                    LAUNCH_DIR_SIZE);
    g_assert_cmphex(qtest_readq(qts, launch + HW_DIR_OFF_COUNT), ==,
                    LAUNCH_ENTRIES);
    g_assert_cmphex(qtest_readq(qts, launch + HW_DIR_OFF_STRIDE), ==,
                    LAUNCH_STRIDE);

    g_assert_cmphex(qtest_readq(qts, nmi + HW_DIR_OFF_SIGNATURE), ==,
                    HW_DIR_SIGNATURE);
    g_assert_cmphex(qtest_readq(qts, nmi + HW_DIR_OFF_OFFSET), ==,
                    NMI_BLOCKS);
    g_assert_cmphex(qtest_readq(qts, nmi + HW_DIR_OFF_SIZE), ==,
                    NMI_STRIDE);
    g_assert_cmphex(qtest_readq(qts, nmi + HW_DIR_OFF_COUNT), ==, 2);
    g_assert_cmphex(qtest_readq(qts, nmi + HW_DIR_OFF_STRIDE), ==,
                    NMI_STRIDE);

    g_assert_cmphex(qtest_readq(qts, freemem + HW_DIR_OFF_SIGNATURE), ==,
                    HW_DIR_SIGNATURE);
    g_assert_cmphex(qtest_readq(qts, freemem + HW_DIR_OFF_OFFSET), ==,
                    FIRST_FREE);
    g_assert_cmphex(qtest_readq(qts, freemem + HW_DIR_OFF_SIZE), ==,
                    FREEMEM_SIZE);
    g_assert_cmphex(qtest_readq(qts, freemem + HW_DIR_OFF_COUNT), ==, 1);

    g_assert_cmphex(qtest_readq(qts, debug + HW_DIR_OFF_SIGNATURE), ==,
                    HW_DIR_SIGNATURE);
    g_assert_cmphex(qtest_readq(qts, debug + HW_DIR_OFF_OFFSET), ==,
                    DEBUG_STACKS_BASE);
    g_assert_cmphex(qtest_readq(qts, debug + HW_DIR_OFF_SIZE), ==,
                    DEBUG_STACKS_SIZE);
    g_assert_cmphex(qtest_readq(qts, debug + HW_DIR_OFF_COUNT), ==,
                    DEBUG_STACKS_N);
    g_assert_cmphex(qtest_readq(qts, debug + HW_DIR_OFF_STRIDE), ==,
                    DEBUG_STACKS_STRIDE);

    g_assert_cmphex(qtest_readq(qts, pi_error + HW_DIR_OFF_SIGNATURE), ==,
                    HW_DIR_SIGNATURE);
    g_assert_cmphex(qtest_readq(qts, pi_error + HW_DIR_OFF_OFFSET), ==,
                    PI_ERROR_BASE);
    g_assert_cmphex(qtest_readq(qts, pi_error + HW_DIR_OFF_SIZE), ==,
                    PI_ERROR_SIZE);
    g_assert_cmphex(qtest_readq(qts, pi_error + HW_DIR_OFF_COUNT), ==, 1);

    /*
     * The launch blocks themselves, not just the entry pointing at them.
     * Nothing else in a block has a value yet.
     */
    for (i = 0; i < LAUNCH_ENTRIES; i++) {
        g_assert_cmphex(qtest_readq(qts, LAUNCH_BLOCKS + i * LAUNCH_STRIDE),
                        ==, LAUNCH_SIGNATURE);
    }

    qtest_quit(qts);
}

/*
 * No guest we boot reads this vector, so a broken one passes every other
 * test; all-entries-one-stub still passes a range check.
 */
static void test_private_vector(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t main_last = qtest_readq(qts,
                                     ARCS_VECTOR + (ARCS_ENTRIES - 1) * 8);
    uint64_t expected_first = main_last + ARCS_STUB_SIZE;
    int i;

    for (i = 0; i < ARCS_PRIVATE_ENTRIES; i++) {
        uint64_t entry = qtest_readq(qts, ARCS_PRIVATE_VECTOR + i * 8);

        /* Each entry a distinct stub, one stride past the previous. */
        g_assert_cmphex(entry, ==, expected_first + i * ARCS_STUB_SIZE);
    }

    /*
     * The capability entry, read through the trap page the way its stub
     * would: the dispatcher running, not a value sitting in a table.
     */
    g_assert_cmphex(qtest_readq(qts,
                                ARCS_TRAP_LOCAL + ARCS_CAPABILITY_OP * 8),
                    ==, ARCS_CAPABILITY_LEVEL);

    qtest_quit(qts);
}

/*
 * The layout is conditional on the addressing mode and nothing in the
 * structure says so; register and layout are checked together because
 * separately both pass while disagreeing.
 */
static void test_memory_layout_matches_addressing_mode(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -m 256");
    uint64_t mem = GRAPH_COMPONENTS + COMP_MEMORY_INDEX * GRAPH_COMP_STRIDE;
    unsigned int i;

    /* The condition the eight-bank layout below depends on. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + NI_STATUS) & NI_MORE_NODES,
                    ==, 0);

    g_assert_cmphex(qtest_readb(qts, mem + COMP_TYPE), ==, COMP_MEMORY);
    g_assert_cmphex(qtest_readw(qts, mem + MEM_TOTAL_MB), ==, 256);
    g_assert_cmphex(qtest_readw(qts, mem + MEM_BANK_SIZES), ==, 256);

    /* Zeroed later banks bound a mode mismatch to a zero attribute. */
    for (i = 1; i < MEM_BANKS; i++) {
        g_assert_cmphex(qtest_readw(qts, mem + MEM_BANK_SIZES + i * 2), ==, 0);
    }

    /*
     * Catches a table shorter than the guest will read -- the dangerous
     * direction; longer is not a hazard.
     */
    for (i = 0; i < GDA_NODES; i++) {
        uint16_t entry = qtest_readw(qts, GDA + GDA_NODE_TABLE + i * 2);

        g_assert_cmphex(entry, ==, i == 0 ? 0 : GDA_NODE_ABSENT);
    }

    qtest_quit(qts);
}

static void test_dual_node_gda(const void *data)
{
    QTestState *qts = qtest_init(DUAL_NODE_ARGS);
    unsigned int i;

    for (i = 0; i < 2; i++) {
        uint64_t base = i * NODE_STRIDE;

        g_assert_cmphex(qtest_readw(qts, base + GDA + GDA_NODE_TABLE), ==, 0);
        g_assert_cmphex(qtest_readw(qts, base + GDA + GDA_NODE_TABLE + 2),
                        ==, 1);
        g_assert_cmphex(qtest_readw(qts, base + GDA + GDA_NODE_TABLE + 4),
                        ==, GDA_NODE_ABSENT);
    }

    qtest_quit(qts);
}

static unsigned int count_trace_records(const char *trace, const char *record)
{
    unsigned int count = 0;

    while ((trace = strstr(trace, record))) {
        count++;
        trace += strlen(record);
    }
    return count;
}

static void test_reset_programs_each_node_discovery_widget(const void *data)
{
    g_autofree char *path = NULL;
    g_autofree char *trace = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn0-firmware-trace-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf(DUAL_NODE_ARGS
                      " -trace sn0_firmware_widget,file=%s", path);
    qtest_system_reset(qts);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &trace, NULL, NULL));
    g_assert_cmpuint(count_trace_records(trace, "node 0 widget 0"), ==, 2);
    g_assert_cmpuint(count_trace_records(trace, "node 1 widget 0"), ==, 2);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/firmware/gda-signature", NULL,
                        test_gda_signature);
    qtest_add_data_func("sn0/firmware/directory-entries", NULL,
                        test_directory_entries);
    qtest_add_data_func("sn0/firmware/private-vector", NULL,
                        test_private_vector);
    qtest_add_data_func("sn0/firmware/memory-layout-matches-addressing-mode",
                        NULL, test_memory_layout_matches_addressing_mode);
    qtest_add_data_func("sn0/firmware/dual-node-gda", NULL,
                        test_dual_node_gda);
    qtest_add_data_func("sn0/firmware/reset-programs-each-discovery-widget",
                        NULL, test_reset_programs_each_node_discovery_widget);

    return g_test_run();
}
