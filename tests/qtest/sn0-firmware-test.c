/*
 * SGI SN0 synthesized firmware: the fields whose failure a boot cannot
 * report. Most of the configuration graph announces its own failure -- a
 * guest that cannot parse it stops with a message. What cannot: the
 * console base (both wrong values produce output), the console offset in
 * the graph header (a wrong one points into the header, which yields a
 * plausible address), each component's enabled flag (a disabled component
 * is skipped silently, and a skipped processor kills the delay-loop
 * calibration long after the last printed line), and the signatures (a
 * missing one means a quiet fallback). Node offsets are physical
 * addresses as they stand; memory sits at the base of the address space.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define CONFIG_GRAPH        0x4000
#define GRAPH_MAGIC         0xbeedbabeULL
#define GRAPH_CONSOLE_OFF   0x18
#define GRAPH_BOARDS        0x4100
#define GRAPH_BOARD_SIZE    0xc0
#define GRAPH_COMPONENTS    0x4400
#define GRAPH_COMP_STRIDE   0x80
#define GRAPH_COMPONENTS_N  8
#define NODE_STRIDE         0x100000000ULL

#define BOARD_LOCAL         1
#define COMP_ENABLED        1
#define COMP_PROCESSOR      1
#define COMP_MEMORY         3
#define COMP_MEMORY_INDEX   2
#define COMP_HUB_INDEX      1
#define COMP_PORT_OFFSET    0x3c
#define COMP_PORT_NODE_INVALID 0xffff

/*
 * The Hub's network-interface status register, reached through the crossbar's
 * widget-1 window at an address that resolves to the reading processor's own
 * node. Its more-nodes bit is the addressing mode.
 */
#define HUB_LOCAL           (0x20000000000ULL + 0x01000000)
#define NI_STATUS           0x600000
#define NI_MORE_NODES       (1ULL << 18)

/* Memory-component fields, in the eight-bank layout M mode calls for. */
#define MEM_TOTAL_MB        0x38
#define MEM_BANK_SIZES      0x3c
#define MEM_BANKS           8

/*
 * The console's address within widget 8, composed: the I/O bridge's window for
 * the third device on its bus, plus serial port A inside the controller. A real
 * Origin 200 reports its console at this address.
 */
#define BRIDGE_DEVICE2      0x600000
#define IOC3_SERIAL_A       0x020178
#define CONSOLE_IN_WIDGET   (BRIDGE_DEVICE2 + IOC3_SERIAL_A)
#define CONSOLE_PORT_MASK   0xfffff     /* the offset the guest drives */
#define CONSOLE_PORT_LIMIT  0x380       /* at or above: the later controller */
#define CONSOLE_WIDGET      8

/*
 * The controller's type 0 configuration window: a sibling of its memory
 * window in the widget space, not something inside it. Written whole --
 * composing it from the neighbouring fields is the defect it guards.
 */
#define CONSOLE_CFG_IN_WIDGET   0x022000

/*
 * Where each base-I/O device sits on the PCI bus, which the graph reports
 * as the component's physical identifier.
 */
#define SCSI_DEVICE0        0
#define SCSI_DEVICE1        1
#define IOC3_DEVICE         2
#define CONSOLE_BAUD        9600

#define GDA                 0x11000
#define GDA_SIGNATURE       0x58464552  /* "XFER" */
#define GDA_VERSION         2
#define GDA_NODE_TABLE      0x80
#define GDA_NODES           64      /* the count this family is built with */
#define GDA_NODE_ABSENT     0xffff

/*
 * The hardware directory: sixteen entries of sixty-four bytes each, every
 * entry the same shape. This is the machine-identification fallback and the
 * non-maskable-interrupt path's only route to its own save blocks, so a
 * directory entry that has never been written is not a cosmetic gap.
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
 * The two call vectors and their trap page. The private vector's stubs
 * continue where the main vector's thirty-five leave off, so its first
 * entry is checked against the main vector's last one -- the builder's own
 * relationship, without duplicating its arithmetic here.
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
#define COMP_FLAGS          0x02
#define COMP_CPU_CLOCK      0x3c
#define COMP_PHYSICAL_ID    0x18
#define COMP_VIRTUAL_ID     0x1c
#define COMP_WIDGET_ID      0x20

#define COMP_TYPE_BRIDGE    5
#define COMP_TYPE_SCSI      11
#define COMP_TYPE_IOC3      6

/* Board record fields this file reads. */
#define BOARD_NEXT          0x00
#define BOARD_KIND          0x04
#define BOARD_TYPE          0x05
#define BOARD_VERSION       0x06
#define BOARD_COMPONENTS    0x17
#define BOARD_NASID         0x20
#define BOARD_COMP_LIST     0x24

#define BOARD_TYPE_NODE     0x11
#define BOARD_TYPE_MIDPLANE 0x41
#define BOARD_TYPE_BASE_IO  0x21
#define BOARD_VERSION_NODE  3      /* every other board type gets two */
#define BOARD_VERSION_OTHER 2

static void test_console_port_offset(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t base = qtest_readq(qts, CONFIG_GRAPH + GRAPH_CONSOLE_OFF);
    uint64_t in_widget = base & 0xffffff;

    /*
     * The composed address, then each mask the guest applies. Every check
     * short of the whole address accepts at least one known-wrong value:
     * 0x178 and 0x20178 both keep 0x178 in the low twelve bits -- the only
     * part a guest announces a decision about -- and both produce a
     * machine that identifies its console and then writes 8N1 to something
     * that is not a serial port.
     */
    g_assert_cmphex(in_widget, ==, CONSOLE_IN_WIDGET);

    /* The register offset the guest drives, after its twenty-bit split. */
    g_assert_cmphex(base & CONSOLE_PORT_MASK, ==, IOC3_SERIAL_A);

    /* And the generation check, which neither of the above implies. */
    g_assert_cmphex(base & 0xfff, <, CONSOLE_PORT_LIMIT);

    /* The widget the console hangs off, which the descriptor repeats below. */
    g_assert_cmphex((base >> 24) & 0xff, ==, CONSOLE_WIDGET);

    qtest_quit(qts);
}

static void test_console_descriptor(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t console;

    /*
     * Followed, not just read: asserting the offset's value alone would
     * pass if the descriptor moved.
     */
    g_assert_cmphex(qtest_readl(qts, CONFIG_GRAPH + 0x10), ==,
                    GRAPH_CONSOLE_OFF);

    console = CONFIG_GRAPH + qtest_readl(qts, CONFIG_GRAPH + 0x10);

    /*
     * The memory base is the one field Linux reads; wrong, the boot is
     * silent. Asserted as a relationship because the failure to catch is
     * the two fields disagreeing -- no assertion on either alone notices.
     */
    g_assert_cmphex(qtest_readq(qts, console + 0x10) + IOC3_SERIAL_A, ==,
                    qtest_readq(qts, console + 0x00));

    /*
     * The configuration base, followed by the vendor kernel as a pointer.
     * Asserted as a whole value: it is a sibling of the memory window with
     * nothing to be offset from, and a check relating it to a neighbour is
     * satisfied by exactly the wrong value it once held.
     */
    g_assert_cmphex(qtest_readq(qts, console + 0x08) & 0xffffff, ==,
                    CONSOLE_CFG_IN_WIDGET);

    /* And it must not be the memory base, which is what it used to be. */
    g_assert_cmphex(qtest_readq(qts, console + 0x08), !=,
                    qtest_readq(qts, console + 0x10));

    /* Same widget and same space as its siblings, whatever the offset. */
    g_assert_cmphex(qtest_readq(qts, console + 0x08) >> 24, ==,
                    qtest_readq(qts, console + 0x10) >> 24);

    g_assert_cmphex(qtest_readw(qts, console + 0x18), ==, CONSOLE_BAUD);
    g_assert_cmphex(qtest_readb(qts, console + 0x22), ==, CONSOLE_WIDGET);

    /* The console controller's PCI device number, not a device count. */
    g_assert_cmphex(qtest_readb(qts, console + 0x23), ==, IOC3_DEVICE);

    qtest_quit(qts);
}

static void test_signatures(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Read as a doubleword: the value fits in four bytes, so a four-byte
     * read finds the zero half and reports a missing graph on a machine
     * that has one.
     */
    g_assert_cmphex(qtest_readq(qts, CONFIG_GRAPH), ==, GRAPH_MAGIC);

    /* And without this, neither does the node table. */
    g_assert_cmphex(qtest_readl(qts, GDA), ==, GDA_SIGNATURE);
    g_assert_cmphex(qtest_readw(qts, GDA + 0x04), ==, GDA_VERSION);

    /*
     * The node table, read the guest's way: signed halfwords, negative
     * ends the walk. The width is the assertion -- a byte-per-entry table
     * looks right byte for byte, but a halfword load pairs node zero with
     * the marker beside it and reads a positive 255 the walk never
     * rejects.
     */
    g_assert_cmphex(qtest_readw(qts, GDA + GDA_NODE_TABLE), ==, 0);
    g_assert_cmphex(qtest_readw(qts, GDA + GDA_NODE_TABLE + 2), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, GDA + GDA_NODE_TABLE + 4), ==, 0xffff);

    qtest_quit(qts);
}

/*
 * The hardware-directory entries, checked against the datasheet's table
 * rather than each other. The NMI entry has a live consumer: OpenBSD
 * stores its handler through it before the console exists, so a zeroed
 * entry sends that write into the exception-handler area with nothing to
 * report it. The rest have no consumer on any guest we boot, which is why
 * they are checked here and nowhere else.
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
 * An unconnected port must carry the invalid marker, not zero: zero is a
 * live node identifier, and a consumer following it lands in a board
 * lookup at this node's own directory. Nothing we boot reads the field,
 * so a wrong value has no other way to be noticed.
 */
static void test_hub_port_unconnected(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t hub = GRAPH_COMPONENTS + COMP_HUB_INDEX * GRAPH_COMP_STRIDE;

    g_assert_cmphex(qtest_readw(qts, hub + COMP_PORT_OFFSET), ==,
                    COMP_PORT_NODE_INVALID);

    qtest_quit(qts);
}

/*
 * The SGI-private call vector, which no guest we boot reads -- the reason
 * a broken one passes every other test. A range check alone is not the
 * contract: all seventeen entries pointing at one stub still passes
 * "non-null and in range".
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

static void test_components_enabled(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    unsigned int i;
    bool saw_processor = false;

    for (i = 0; i < GRAPH_COMPONENTS_N; i++) {
        uint64_t c = GRAPH_COMPONENTS + i * GRAPH_COMP_STRIDE;
        uint8_t type = qtest_readb(qts, c + COMP_TYPE);

        /* Silently skipped otherwise, and the skip is what does the damage. */
        g_assert_cmphex(qtest_readb(qts, c + COMP_FLAGS) & COMP_ENABLED, ==,
                        COMP_ENABLED);
        g_assert_cmpuint(type, !=, 0);

        if (type == COMP_PROCESSOR) {
            /*
             * The delay-loop calibration source; zero is the failure that
             * looks like a hang somewhere else entirely.
             */
            g_assert_cmpuint(qtest_readw(qts, c + COMP_CPU_CLOCK), >, 0);
            saw_processor = true;
        }
    }

    g_assert_true(saw_processor);
    qtest_quit(qts);
}

/*
 * The memory component's layout is conditional on the Hub's addressing
 * mode, and nothing in the structure says so: M mode means eight bank
 * sizes with the directory attribute behind them, N mode four and a
 * shorter record. The register and the layout are checked together
 * because separately both can pass while disagreeing -- the guest then
 * reads a bank size where it expects the attribute.
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

    /*
     * Later bank slots are zero, which bounds the damage of a mode
     * mismatch: an N-mode guest reads a zero attribute rather than a bank
     * size wearing an attribute's clothes.
     */
    for (i = 1; i < MEM_BANKS; i++) {
        g_assert_cmphex(qtest_readw(qts, mem + MEM_BANK_SIZES + i * 2), ==, 0);
    }

    /*
     * The same bit bounds the guest's node-table walk: in N mode it reads
     * 64 entries past this table's end, all zeroed, so it quietly scans
     * node zero sixty-four more times rather than faulting. This loop
     * catches a table shorter than the guest will read -- the dangerous
     * direction; a longer one is not a hazard.
     */
    for (i = 0; i < GDA_NODES; i++) {
        uint16_t entry = qtest_readw(qts, GDA + GDA_NODE_TABLE + i * 2);

        g_assert_cmphex(entry, ==, i == 0 ? 0 : GDA_NODE_ABSENT);
    }

    qtest_quit(qts);
}

static void test_base_io_components(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t board = GRAPH_BOARDS;
    bool found = false;

    while (board != 0) {
        if (qtest_readb(qts, board + BOARD_TYPE) == BOARD_TYPE_BASE_IO) {
            /*
             * Four components in a real Origin 200's own captured order
             * (project-tree boot logs). Every field of each record is
             * asserted, because the defect covered was one identifier
             * right beside another zeroed -- asserting only the corrected
             * field passes on exactly the machine with the bug in its
             * neighbour. Physical id is the widget number for the bridge
             * and the PCI device for the controllers; widget id is shared;
             * virtual id is the per-type ordinal.
             */
            static const struct {
                uint8_t type;
                uint8_t physical_id;
                uint8_t widget_id;
                uint32_t virtual_id;
            } expected[] = {
                { COMP_TYPE_BRIDGE, CONSOLE_WIDGET, CONSOLE_WIDGET, 0 },
                { COMP_TYPE_SCSI,   SCSI_DEVICE0,   CONSOLE_WIDGET, 0 },
                { COMP_TYPE_SCSI,   SCSI_DEVICE1,   CONSOLE_WIDGET, 1 },
                { COMP_TYPE_IOC3,   IOC3_DEVICE,    CONSOLE_WIDGET, 0 },
            };
            uint8_t components = qtest_readb(qts, board + BOARD_COMPONENTS);

            found = true;
            g_assert_cmpuint(components, ==, G_N_ELEMENTS(expected));

            for (unsigned int i = 0; i < components; i++) {
                uint32_t off = qtest_readl(qts, board + BOARD_COMP_LIST +
                                           i * 4);

                g_assert_cmpuint(qtest_readb(qts, off + COMP_TYPE), ==,
                                 expected[i].type);
                g_assert_cmpuint(qtest_readb(qts, off + COMP_PHYSICAL_ID), ==,
                                 expected[i].physical_id);
                g_assert_cmpuint(qtest_readb(qts, off + COMP_WIDGET_ID), ==,
                                 expected[i].widget_id);
                g_assert_cmpuint(qtest_readl(qts, off + COMP_VIRTUAL_ID), ==,
                                 expected[i].virtual_id);
            }
        }
        board = qtest_readl(qts, board + BOARD_NEXT);
    }

    g_assert_true(found);
    qtest_quit(qts);
}

static void test_board_list(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t board = GRAPH_BOARDS;
    unsigned int boards = 0;

    /*
     * The list ends only at a zero link, so the walk is bounded here or a
     * broken list hangs this test instead of failing it.
     */
    g_assert_cmphex(qtest_readl(qts, CONFIG_GRAPH + 0x14), ==, GRAPH_BOARDS);

    while (board != 0) {
        uint8_t components;

        g_assert_cmpuint(boards, <, 8);
        g_assert_cmphex(board, >=, GRAPH_BOARDS);
        g_assert_cmphex(board, <, GRAPH_BOARDS + 8 * GRAPH_BOARD_SIZE);

        /* A board that is not local is skipped, components and all. */
        g_assert_cmphex(qtest_readb(qts, board + BOARD_KIND), ==, BOARD_LOCAL);

        /*
         * An exact per-type version, not a lower bound: a lower bound
         * passes a node board carrying the midplane's version, or every
         * board carrying one wrong value.
         */
        {
            uint8_t type = qtest_readb(qts, board + BOARD_TYPE);
            uint8_t expected_version = (type == BOARD_TYPE_NODE) ?
                                       BOARD_VERSION_NODE : BOARD_VERSION_OTHER;

            g_assert_cmphex(qtest_readb(qts, board + BOARD_VERSION), ==,
                            expected_version);
        }

        components = qtest_readb(qts, board + BOARD_COMPONENTS);
        /*
         * Exactly zero on the midplane, greater than zero elsewhere: real
         * firmware writes the midplane's zero on purpose while populating
         * one component behind it.
         */
        if (qtest_readb(qts, board + BOARD_TYPE) == BOARD_TYPE_MIDPLANE) {
            g_assert_cmpuint(components, ==, 0);
        } else {
            g_assert_cmpuint(components, >, 0);
        }
        g_assert_cmpuint(components, <=, GRAPH_COMPONENTS_N);

        /* Every component a board points at has to be one that exists. */
        for (unsigned int i = 0; i < components; i++) {
            uint32_t off = qtest_readl(qts, board + BOARD_COMP_LIST + i * 4);

            g_assert_cmphex(off, >=, GRAPH_COMPONENTS);
            g_assert_cmphex(off, <,
                            GRAPH_COMPONENTS +
                            GRAPH_COMPONENTS_N * GRAPH_COMP_STRIDE);
            g_assert_cmphex((off - GRAPH_COMPONENTS) % GRAPH_COMP_STRIDE, ==,
                            0);
        }

        /*
         * The midplane's real component sits one slot past its declared
         * count -- the slot real firmware's own consumer reads, which a
         * walk to the count never looks at.
         */
        if (qtest_readb(qts, board + BOARD_TYPE) == BOARD_TYPE_MIDPLANE) {
            uint32_t off = qtest_readl(qts, board + BOARD_COMP_LIST +
                                       components * 4);

            g_assert_cmphex(off, >=, GRAPH_COMPONENTS);
            g_assert_cmphex(off, <,
                            GRAPH_COMPONENTS +
                            GRAPH_COMPONENTS_N * GRAPH_COMP_STRIDE);
        }

        board = qtest_readl(qts, board + BOARD_NEXT);
        boards++;
    }

    /* A node board, a midplane and a base-I/O board. */
    g_assert_cmpuint(boards, ==, 3);
    qtest_quit(qts);
}

static void test_dual_node_inventory(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
    uint64_t node1 = NODE_STRIDE;
    uint64_t board = node1 + GRAPH_BOARDS;
    uint64_t component[4];
    unsigned int i;

    for (i = 0; i < 2; i++) {
        uint64_t base = i * NODE_STRIDE;

        g_assert_cmphex(qtest_readw(qts, base + GDA + GDA_NODE_TABLE), ==, 0);
        g_assert_cmphex(qtest_readw(qts, base + GDA + GDA_NODE_TABLE + 2),
                        ==, 1);
        g_assert_cmphex(qtest_readw(qts, base + GDA + GDA_NODE_TABLE + 4),
                        ==, GDA_NODE_ABSENT);
    }

    g_assert_cmphex(qtest_readq(qts, node1 + CONFIG_GRAPH), ==, GRAPH_MAGIC);
    g_assert_cmphex(qtest_readw(qts, board + BOARD_NASID), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, board + BOARD_COMPONENTS), ==, 4);
    for (i = 0; i < 4; i++) {
        component[i] = node1 + qtest_readl(
            qts, board + BOARD_COMP_LIST + i * sizeof(uint32_t));
    }
    g_assert_cmpuint(qtest_readb(qts, component[0] + COMP_TYPE), ==,
                     COMP_PROCESSOR);
    g_assert_cmpuint(qtest_readb(qts, component[1] + COMP_TYPE), ==,
                     COMP_PROCESSOR);
    g_assert_cmpuint(qtest_readb(qts, component[2] + COMP_TYPE), ==, 2);
    g_assert_cmphex(qtest_readb(qts, component[2] + COMP_PHYSICAL_ID), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, component[3] + COMP_TYPE), ==,
                     COMP_MEMORY);
    g_assert_cmphex(qtest_readw(qts, component[3] + MEM_TOTAL_MB), ==, 64);

    board = node1 + qtest_readl(qts, board + BOARD_NEXT);
    g_assert_cmpuint(qtest_readb(qts, board + BOARD_TYPE), ==,
                     BOARD_TYPE_MIDPLANE);

    board = node1 + qtest_readl(qts, board + BOARD_NEXT);
    g_assert_cmpuint(qtest_readb(qts, board + BOARD_TYPE), ==,
                     BOARD_TYPE_BASE_IO);
    g_assert_cmphex(qtest_readw(qts, board + BOARD_NASID), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, board + BOARD_COMPONENTS), ==, 4);
    for (i = 0; i < 4; i++) {
        component[i] = node1 + qtest_readl(
            qts, board + BOARD_COMP_LIST + i * sizeof(uint32_t));
    }
    g_assert_cmpuint(qtest_readb(qts, component[0] + COMP_TYPE), ==,
                     COMP_TYPE_BRIDGE);
    g_assert_cmpuint(qtest_readb(qts, component[1] + COMP_TYPE), ==,
                     COMP_TYPE_SCSI);
    g_assert_cmpuint(qtest_readb(qts, component[2] + COMP_TYPE), ==,
                     COMP_TYPE_SCSI);
    g_assert_cmpuint(qtest_readb(qts, component[3] + COMP_TYPE), ==,
                     COMP_TYPE_IOC3);
    g_assert_cmphex(qtest_readl(qts, board + BOARD_NEXT), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/firmware/console-port-offset", NULL,
                        test_console_port_offset);
    qtest_add_data_func("sn0/firmware/console-descriptor", NULL,
                        test_console_descriptor);
    qtest_add_data_func("sn0/firmware/signatures", NULL, test_signatures);
    qtest_add_data_func("sn0/firmware/directory-entries", NULL,
                        test_directory_entries);
    qtest_add_data_func("sn0/firmware/hub-port-unconnected", NULL,
                        test_hub_port_unconnected);
    qtest_add_data_func("sn0/firmware/private-vector", NULL,
                        test_private_vector);
    qtest_add_data_func("sn0/firmware/components-enabled", NULL,
                        test_components_enabled);
    qtest_add_data_func("sn0/firmware/memory-layout-matches-addressing-mode",
                        NULL, test_memory_layout_matches_addressing_mode);
    qtest_add_data_func("sn0/firmware/board-list", NULL, test_board_list);
    qtest_add_data_func("sn0/firmware/base-io-components", NULL,
                        test_base_io_components);
    qtest_add_data_func("sn0/firmware/dual-node-inventory", NULL,
                        test_dual_node_inventory);

    return g_test_run();
}
