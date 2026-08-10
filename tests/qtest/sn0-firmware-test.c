/*
 * SGI SN0 synthesized firmware: the fields whose failure a boot cannot report.
 *
 * The machine writes the firmware structures a guest reads before it owns a
 * console: a configuration graph describing the boards and components, and a
 * global data area carrying the node table. Almost everything in them announces
 * its own failure, because a guest that cannot make sense of the graph stops
 * with a message. Four things do not, and those are what this file is for.
 *
 * The first is the console's serial-port base. A guest picks which generation
 * of I/O controller it has from the low twelve bits of that address and from
 * nothing else. An Origin 200's console port is at 0x178; at 0x380 or above a
 * guest decides it has the later controller and reads a configuration register
 * that does not exist on this machine. Both beliefs produce output, so no line
 * of a boot transcript distinguishes them.
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

#define CONSOLE_PORT_OFFSET 0x178
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
    uint64_t port = base & 0xfff;

    /*
     * The one field in the whole of the synthesized firmware that a guest reads
     * for a decision it never announces.
     */
    g_assert_cmphex(port, ==, CONSOLE_PORT_OFFSET);
    g_assert_cmphex(port, <, CONSOLE_PORT_LIMIT);

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
    qtest_add_data_func("sn0/firmware/board-list", NULL, test_board_list);

    return g_test_run();
}
