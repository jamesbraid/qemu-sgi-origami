/*
 * SGI SN0 synthesized firmware: the fields whose failure a boot cannot report.
 *
 * The machine writes the firmware structures a guest reads before it owns a
 * console: a configuration graph describing the boards and components, and a
 * global data area carrying the node table. Almost everything in them announces
 * its own failure, because a guest that cannot make sense of the graph stops
 * with a message. Four things do not, and those are what this file is for.
 *
 * The first is the console's serial-port base, which a guest splits with two
 * different masks that mean different things. The low twenty bits are the
 * register offset that has to reach hardware; only the low twelve decide which
 * generation of I/O controller it believes it has, and at 0x380 or above it
 * reads a configuration register this machine does not have. Both beliefs
 * produce output, so no line of a transcript distinguishes them. And because
 * 0x178 and 0x20178 share their low twelve bits, a value can pass the
 * generation check while reaching no register at all.
 *
 * The second is the console offset in the graph header, which is redundant with
 * the descriptor stored behind it. A guest reaches the descriptor by adding the
 * offset to the header's own address, so a wrong offset points it at the middle
 * of the header, where it finds a plausible address made of whichever fields it
 * landed on.
 *
 * The third is each component's enabled flag. Consumers skip a component that
 * does not have it, and skipping is silent. A skipped processor component
 * leaves the guest's clock at zero, which breaks the delay loop it calibrates
 * from that clock, and it dies with no message just after its last printed
 * line, a failure that looks like a hang in whatever ran next.
 *
 * The fourth is the pair of signatures. A consumer that does not find the one
 * it wants concludes the structure is absent and falls back, and the fallback
 * is quiet.
 *
 * The graph lives in memory, which this machine places at the base of its
 * address space, so a node offset is a physical address as it stands.
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
 * The controller's type 0 PCI configuration window, which is a sibling of its
 * memory window in the bridge's widget space, not something inside it —
 * the bridge's type 0 base plus the device's own 4 KB slot. Written out whole
 * rather than composed from the two fields either side of it, because the
 * defect this covers was precisely that it had been composed from them.
 */
#define CONSOLE_CFG_IN_WIDGET   0x022000

/*
 * Where each device on the base-I/O board sits on the Bridge's PCI bus, which
 * is also what the configuration graph reports as that component's physical
 * identifier. Named here rather than written as bare numbers so a device
 * moving on the bus fails this test rather than passing it quietly.
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
 * The two call vectors and the trap page their stubs read through. The
 * private vector's stubs continue immediately where the firmware call
 * vector's own thirty-five leave off, in the same stub area and the same
 * trap page, so its first entry is checked against the main vector's last
 * one rather than against a hand-computed absolute address — the exact
 * relationship the builder creates, without duplicating its sign-extension
 * arithmetic in the test.
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
     * Assert the composed address, then each mask the guest applies to it. Two
     * wrong values have already been written into this field and every check
     * short of the whole address accepted at least one of them.
     *
     * 0x178 alone is the generation discriminator and reaches no register.
     * 0x20178 is the right register offset inside the controller but, without
     * the bridge's device window above it, lands in the bridge's own PCI
     * configuration space rather than at any device. Both keep 0x178 in the low
     * twelve bits, the only part a guest announces a decision about, so both
     * produce a machine that identifies its console correctly and then writes
     * 8N1 to something that is not a serial port.
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
     * The header's offset has to lead to the descriptor rather than into the
     * header. Following it and finding the descriptor's own fields where they
     * belong is what says the two agree; asserting the offset's value alone
     * would pass just as happily if the descriptor moved.
     */
    g_assert_cmphex(qtest_readl(qts, CONFIG_GRAPH + 0x10), ==,
                    GRAPH_CONSOLE_OFF);

    console = CONFIG_GRAPH + qtest_readl(qts, CONFIG_GRAPH + 0x10);

    /*
     * The memory base is the field Linux reads, and it reads no other. It takes
     * this as the base of the controller chip and adds the chip's own register
     * offset itself, checking nothing. So it has to carry the bridge's device
     * window, and be an address the processor can issue rather than a bare
     * physical one. Wrong here, Linux takes a bus error or spins forever on a
     * transmitter-empty bit that never sets, and both are a silent boot with
     * no diagnostic to work from.
     *
     * Asserted as a relationship rather than as a constant, because the defect
     * this catches is not a wrong address — it is the two fields disagreeing
     * after one of them was corrected. The serial-port base already had the
     * device window added and this one did not, which no assertion on either
     * field alone would have noticed.
     */
    g_assert_cmphex(qtest_readq(qts, console + 0x10) + IOC3_SERIAL_A, ==,
                    qtest_readq(qts, console + 0x00));

    /*
     * The configuration base, which the vendor kernel follows as a pointer
     * into PCI configuration space. Asserted as a whole value rather than as a
     * relationship, and that is the difference from the two fields above: they
     * are offset from one another, so a relationship catches them drifting
     * apart, while this one is a sibling of the memory window rather than
     * anything inside it and has nothing to be offset from.
     *
     * The defect it covers is exactly the composition that could not express
     * it. This field held the memory base — the same value as the field above
     * — because all three shared a root that already included the device
     * window, and the configuration window sits below that window rather than
     * within it, so no value reachable from that root was right. A check
     * relating it to either neighbour would have been satisfied by the wrong
     * value it was written to replace.
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
     * Without this the graph does not exist as far as a guest is concerned. It
     * is an eight-byte field holding a value that fits in four, so it has to
     * be read as a doubleword: reading the first four bytes finds the zero
     * half, and would report a missing graph on a machine that has one.
     */
    g_assert_cmphex(qtest_readq(qts, CONFIG_GRAPH), ==, GRAPH_MAGIC);

    /* And without this, neither does the node table. */
    g_assert_cmphex(qtest_readl(qts, GDA), ==, GDA_SIGNATURE);
    g_assert_cmphex(qtest_readw(qts, GDA + 0x04), ==, GDA_VERSION);

    /*
     * The node table, read the way a guest reads it: signed sixteen-bit
     * identifiers at a stride of two, skipped when negative. Node zero exists
     * and is numbered zero, and every slot after it is all ones so that the
     * guest's signed load sees a negative number and stops.
     *
     * The width is the assertion, not a detail of it. Written a byte per entry
     * the table still looks right byte for byte, but the guest's halfword load
     * on the first entry pairs node zero with the absent marker beside it and
     * reads 0x00ff, a positive 255 that no negative test rejects, so the guest
     * goes scanning a node numbered 255 and dies on the first address it forms
     * there. Reading halfwords here is what makes this test able to see that.
     */
    g_assert_cmphex(qtest_readw(qts, GDA + GDA_NODE_TABLE), ==, 0);
    g_assert_cmphex(qtest_readw(qts, GDA + GDA_NODE_TABLE + 2), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, GDA + GDA_NODE_TABLE + 4), ==, 0xffff);

    qtest_quit(qts);
}

/*
 * The six hardware-directory entries the firmware fills, checked against the
 * datasheet's own table rather than against each other. The non-maskable-
 * interrupt entry is the one with a live consumer: OpenBSD reads its offset
 * during machine setup and stores through it directly, installing its own
 * handler address and a complement — a four-word write to wherever this
 * entry says, before the console exists and with nothing that could report
 * a wrong destination. A zeroed entry sends that write to node offset zero,
 * inside the exception-handler area; the fault was invisible only because
 * the kernel copies its own exception vectors over the same bytes later in
 * setup, which is an ordering accident and not a property of the design.
 * The other five have no consumer on any guest we boot, which is why they
 * are checked here rather than by anything a transcript would show.
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
     * The launch blocks themselves, not just the directory entry that points
     * at them: both carry the launch signature. Nothing else in the block is
     * asserted, because nothing else has a value yet — there is no second
     * processor to launch.
     */
    for (i = 0; i < LAUNCH_ENTRIES; i++) {
        g_assert_cmphex(qtest_readq(qts, LAUNCH_BLOCKS + i * LAUNCH_STRIDE),
                        ==, LAUNCH_SIGNATURE);
    }

    qtest_quit(qts);
}

/*
 * The Hub component's port descriptor names no crossbar link on this
 * machine, and the required value for that is the invalid marker in the
 * node-identifier bits, not zero. Zero is a live node identifier here: a
 * consumer that treats an unconnected port as a real link follows it into a
 * board lookup at this node's own hardware directory, which is exactly the
 * kind of silent failure this file exists to catch — nothing we boot reads
 * this field today, so a wrong value here has no other way to be noticed.
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
 * The SGI-private call vector. IRIX and the SGI standalone shell need it;
 * the guests we boot never look at it, which is exactly why a broken vector
 * here would go unnoticed by every other test. Checked two ways, because a
 * range check alone proves memory safety and nothing about the vector's own
 * contract: a broken implementation that pointed all seventeen entries at
 * the same stub would still pass "every entry is non-null and in range."
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

        /*
         * Each entry is a distinct stub, one stride past the previous —
         * exactly the arrangement the builder creates, and the property a
         * "same stub everywhere" bug fails on directly.
         */
        g_assert_cmphex(entry, ==, expected_first + i * ARCS_STUB_SIZE);
    }

    /*
     * The capability entry, invoked through the trap page rather than
     * merely read out of the vector — the same memory access its stub would
     * perform when a guest jumps to it and loads from its slot. This is the
     * dispatcher actually running, not a value sitting in a table.
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
             * The guest calibrates its delay loop from this. Zero here is the
             * failure that looks like a hang somewhere else entirely.
             */
            g_assert_cmpuint(qtest_readw(qts, c + COMP_CPU_CLOCK), >, 0);
            saw_processor = true;
        }
    }

    g_assert_true(saw_processor);
    qtest_quit(qts);
}

/*
 * The memory component's layout is conditional on a Hub register, and nothing
 * about the structure says so. A guest reads the addressing mode from the Hub's
 * network-interface status register and parses the memory component differently
 * depending on the answer: in M mode eight bank sizes with the directory
 * attribute behind them, in N mode four bank sizes and a record eight bytes
 * shorter, so the mode does not merely change a count, it moves the attribute
 * and resizes the record. One guest has no runtime choice at all and is built
 * for eight.
 *
 * So the two have to be checked together or the pair can drift: the machine
 * could start reporting N mode and every assertion about the component would
 * still pass, and a guest would read the fifth bank's size where it expects the
 * attribute while separately computing every node-relative address with the
 * wrong shift. That bit has already been wrong once in this machine's history,
 * when the Hub was realized without a bus and never reset, so its reset value
 * read as N mode.
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
     * Every bank slot after the first is zero, which bounds the damage if the
     * mode bit ever disagrees with this layout after all: the slot an N-mode
     * guest would read as the directory attribute is one of these, so it would
     * read a zero attribute rather than a bank size wearing an attribute's
     * clothes.
     */
    for (i = 1; i < MEM_BANKS; i++) {
        g_assert_cmphex(qtest_readw(qts, mem + MEM_BANK_SIZES + i * 2), ==, 0);
    }

    /*
     * The same bit decides how far a guest walks the node table, which is the
     * mode's third consequence and the reason this test covers two structures
     * rather than one. A guest declares capacity for 128 entries and clamps to
     * that first, then clamps again to 64 in M mode, and only the second clamp
     * keeps it inside a table this size. In N mode it would read 64 entries
     * past the end, and because the area beyond is zeroed every one of them
     * reads as node zero rather than as the negative marker that ends the walk.
     * So it would not fault. It would quietly scan node zero sixty-four more
     * times.
     *
     * What this loop catches is a table shorter than the guest will read, the
     * dangerous direction. It does not catch a longer one, since reading the
     * first sixty-four finds them all correct, and a longer table is not a
     * hazard anyway, only a departure from what real firmware writes.
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
             * Four components in this order — the I/O bridge, two disk
             * controllers, then the I/O controller — matching a real
             * Origin 200's own captured graph and hardware inventory
             * (docs/library/boot-logs.md, docs/component-taxonomy.md).
             *
             * Every field each record carries is asserted rather than the
             * one that motivated writing the check, because the defect this
             * covers was one identifier being right while the identifier
             * beside it in the same record was zero. A test that asserts
             * only the field just corrected passes on exactly the machine
             * that has the bug in its neighbour.
             *
             * The three identifiers mean three different things. The
             * physical identifier is the widget number for the I/O bridge
             * and the PCI device number for each controller — the vendor's
             * own firmware reads that field, not the widget field, to learn
             * which widget its bridge is. The widget identifier is the same
             * for all four because they hang off one widget. The virtual
             * identifier is the ordinal among components of that type.
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
     * The list is terminated only by a zero link, and a consumer reads a record
     * before checking the link that led to it — so a stray link is a walk
     * through whatever the memory held. Walking it here with a bound proves it
     * ends, and the bound is what keeps a broken list from hanging this test
     * instead of failing it.
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
         * The record version has an exact required value per board type,
         * not a lower bound. IRIX's board-naming routine treats anything
         * below two as unnamed regardless of what is above it, so a
         * lower-bound check would pass the node board carrying the
         * midplane's version, every board carrying the same wrong value, or
         * a version far above what real firmware ever writes — none of
         * which is the property that has to hold.
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
         * The declared count is exactly zero on the midplane and greater
         * than zero on every other board — not "greater than zero"
         * everywhere. Real firmware writes zero on the midplane on purpose,
         * while still populating one component behind it, so a bound this
         * test enforced unconditionally would reject the machine it is
         * meant to model. Keeping the bound on every other board type means
         * an accidentally zeroed count elsewhere still fails here.
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
         * The midplane's declared count excludes its one real component from
         * an ordinary walk, but the component is still there — one slot past
         * the count, which is the exact slot real firmware's own consumer
         * reads it back from. A test that only walked to the declared count
         * would never look at this slot at all.
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
