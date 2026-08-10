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
#define GRAPH_COMPONENTS_N  6

#define BOARD_LOCAL         1
#define COMP_ENABLED        1
#define COMP_PROCESSOR      1
#define COMP_MEMORY         3
#define COMP_MEMORY_INDEX   2

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
#define CONSOLE_BAUD        9600

#define GDA                 0x11000
#define GDA_SIGNATURE       0x58464552  /* "XFER" */
#define GDA_VERSION         2
#define GDA_NODE_TABLE      0x80

/* Component record fields this file reads. */
#define COMP_TYPE           0x00
#define COMP_FLAGS          0x02
#define COMP_CPU_CLOCK      0x3c

/* Board record fields this file reads. */
#define BOARD_NEXT          0x00
#define BOARD_KIND          0x04
#define BOARD_COMPONENTS    0x17
#define BOARD_COMP_LIST     0x24

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

    g_assert_cmphex(qtest_readw(qts, console + 0x18), ==, CONSOLE_BAUD);
    g_assert_cmphex(qtest_readb(qts, console + 0x22), ==, CONSOLE_WIDGET);

    /* One device behind the bridge, or a guest walks past the console. */
    g_assert_cmphex(qtest_readb(qts, console + 0x23), ==, 1);

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

        components = qtest_readb(qts, board + BOARD_COMPONENTS);
        g_assert_cmpuint(components, >, 0);
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

        board = qtest_readl(qts, board + BOARD_NEXT);
        boards++;
    }

    /* A node board, a midplane and a base-I/O board. */
    g_assert_cmpuint(boards, ==, 3);
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
    qtest_add_data_func("sn0/firmware/components-enabled", NULL,
                        test_components_enabled);
    qtest_add_data_func("sn0/firmware/memory-layout-matches-addressing-mode",
                        NULL, test_memory_layout_matches_addressing_mode);
    qtest_add_data_func("sn0/firmware/board-list", NULL, test_board_list);

    return g_test_run();
}
