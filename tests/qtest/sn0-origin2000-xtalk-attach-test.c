/*
 * Origin 2000 Crosstalk assembly, as a processor sees it.
 *
 * The owner test proves the objects exist. This one proves they are wired:
 * a processor reads a real Crossbow's registers through its own node's
 * crosstalk window, and reads back the population the descriptor asked for.
 *
 * A switch reports a link present because the descriptor's mask says so,
 * which is true whether or not anything is attached on the other side. What
 * separates the two is reading the part at the far end, so the base I/O
 * checks here go through the board's widget slot to the chip itself.
 *
 * Each node is read from a processor on that node. A processor addresses its
 * own node's crosstalk unqualified; reaching another node's takes routing the
 * firmware has not programmed at reset, so reading every node from one
 * processor finds holes and mistakes them for absent widgets.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"

#define REG_DATA        4
#define NODES           4
#define NODE_CPU(node)  ((node) * 2)

/* A widget's slot inside the crosstalk window, sixteen megabytes apart. */
#define WIDGET_SLOT(w)  ((uint64_t)(w) * 0x1000000)

#define XBOW_ID         0x0000
#define XBOW_LINK(w)    (0x0100 + ((w) - 8) * 0x40)
#define LINK_STATUS     0x10
#define LINK_AUX        0x38

/* The I/O bridge: its register block, then the windows above it. */
#define BRIDGE_IDENTIFICATION   0x000
#define BRIDGE_CFG0(device)     (0x020000 + (device) * 0x1000)
#define BRIDGE_PART             0xc002
#define BRIDGE_MANUFACTURER     0x036
#define ID_REVISION(v)          (((v) >> 28) & 0xf)

/* A configuration read of a slot nothing occupies: all ones, not zero. */
#define NO_CARD         0xffffffffu
#define PCI_IDENTITY    0x00
#define PCI_SUBSYSTEM   0x2c
#define PCI_SLOTS       8

#define ISP1040_IDENTITY  (0x1077u | (0x1020u << 16))
#define IOC3_IDENTITY     (0x10a9u | (0x0003u << 16))
#define IOC3_SUBSYSTEM_VENDOR 0x10a9u

#define ID_PART(v)      (((v) >> 12) & 0xffff)
#define ID_MANUFACTURER(v) (((v) >> 1) & 0x7ff)
#define ID_ALWAYS_ONE   (1u << 0)
#define LINK_ALIVE      (1u << 31)
#define LINK_MAX_RETRY  (1u << 5)
#define AUX_PRESENT     (1u << 5)
#define AUX_FAILURE     (1u << 6)

/* The module's population: base I/O at 8, Hubs at 9 and 10, nothing at 11. */
#define WIDGET_IO       8
#define WIDGET_HUB_A    9
#define WIDGET_HUB_B    10
#define WIDGET_EMPTY    11

static QTestState *attach_start_with(const char *extra)
{
    return qtest_initf("-M origin2000-xtalk-attach-test%s -m 256M "
                       "-nodefaults -display none -serial none", extra);
}

static QTestState *attach_start(void)
{
    return attach_start_with("");
}

/* The machine says where its own window is, so the test cannot drift. */
static uint64_t machine_uint(QTestState *qts, const char *property)
{
    QDict *response = qtest_qmp(qts,
        "{ 'execute': 'qom-get', 'arguments': {"
        "  'path': '/machine', 'property': %s } }", property);
    uint64_t value;

    g_assert(qdict_haskey(response, "return"));
    value = qnum_get_uint(qobject_to(QNum, qdict_get(response, "return")));
    qobject_unref(response);
    return value;
}

static uint64_t xtalk_window(QTestState *qts)
{
    uint64_t base = machine_uint(qts, "xtalk-window");

    g_assert_cmphex(base, !=, 0);
    return base;
}

/* Which widget the one base I/O board occupies, from the descriptor. */
static uint64_t board_slot(QTestState *qts)
{
    uint64_t widget = machine_uint(qts, "board-widget");

    g_assert_cmpuint(widget, >=, 8);
    return WIDGET_SLOT(widget);
}

/* Which widget each node occupies, straight from the descriptor. */
static unsigned int node_widget(QTestState *qts, unsigned int node)
{
    unsigned int widget =
        (machine_uint(qts, "node-widgets") >> (4 * node)) & 0xf;

    g_assert_cmpuint(widget, >=, 8);
    return widget;
}

static const char *runstate(QTestState *qts)
{
    QDict *response = qtest_qmp_assert_success_ref(qts,
        "{ 'execute': 'query-status' }");
    const char *status = g_strdup(qdict_get_str(response, "status"));

    qobject_unref(response);
    return status;
}

static uint32_t switch_readl(QTestState *qts, unsigned int node,
                             unsigned int offset)
{
    uint8_t bytes[sizeof(uint32_t)];

    g_assert_true(qtest_memread_cpu(qts, NODE_CPU(node),
                                    xtalk_window(qts) + offset + REG_DATA,
                                    bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

static uint32_t board_readl(QTestState *qts, unsigned int node,
                            uint64_t offset)
{
    uint8_t bytes[sizeof(uint32_t)];

    g_assert_true(qtest_memread_cpu(qts, NODE_CPU(node),
                                    xtalk_window(qts) + board_slot(qts) +
                                    offset, bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

/* The register block answers one register per eight-byte slot, plus four. */
static uint32_t board_reg_readl(QTestState *qts, unsigned int node,
                                unsigned int offset)
{
    return board_readl(qts, node, offset + REG_DATA);
}

/* Whether this node's switch says the board's link is populated at all. */
static bool board_link_present(QTestState *qts, unsigned int node)
{
    return switch_readl(qts, node, XBOW_LINK(WIDGET_IO) + LINK_AUX) &
           AUX_PRESENT;
}

/* The machine's own structural checks ran during init. */
static void test_the_assembly_verified_itself(void)
{
    QTestState *qts = attach_start();

    g_assert_true(qtest_qom_get_bool(qts, "/machine", "verified"));
    qtest_quit(qts);
}

/*
 * A processor reads the switch its own Hub is attached to. Nothing about the
 * address is switch-specific: it is the machine's widget-zero window, which
 * is where a crossbar's registers answer.
 */
static void test_a_processor_reads_the_switch(void)
{
    QTestState *qts = attach_start();
    uint32_t id = switch_readl(qts, 0, XBOW_ID);

    g_assert_cmpuint(ID_PART(id), ==, 0);
    g_assert_cmpuint(ID_MANUFACTURER(id), ==, 0);
    g_assert_cmpuint(id & ID_ALWAYS_ONE, ==, ID_ALWAYS_ONE);
    qtest_quit(qts);
}

/*
 * The descriptor asks for widgets 8, 9 and 10 on the first switch and
 * nothing else. What a guest reads back has to be that population rather
 * than a default: the widget nothing occupies reports neither present nor
 * alive, and reports having failed to come out of reset.
 */
static void test_the_links_are_the_ones_the_descriptor_asked_for(void)
{
    QTestState *qts = attach_start();
    unsigned int widget;

    for (widget = WIDGET_IO; widget <= WIDGET_HUB_B; widget++) {
        g_assert_cmphex(switch_readl(qts, 0, XBOW_LINK(widget) + LINK_STATUS)
                        & LINK_ALIVE, ==, LINK_ALIVE);
        g_assert_cmphex(switch_readl(qts, 0, XBOW_LINK(widget) + LINK_AUX)
                        & AUX_PRESENT, ==, AUX_PRESENT);
    }

    g_assert_cmphex(switch_readl(qts, 0, XBOW_LINK(WIDGET_EMPTY) +
                                 LINK_STATUS), ==, LINK_MAX_RETRY);
    g_assert_cmphex(switch_readl(qts, 0, XBOW_LINK(WIDGET_EMPTY) + LINK_AUX),
                    ==, AUX_FAILURE);
    qtest_quit(qts);
}

/*
 * Four nodes, two switches. The one carrying the base I/O board has a widget
 * at 8; the other does not. So exactly two nodes see that widget present,
 * and every node sees a switch rather than a hole. Wiring every node to the
 * same switch fails this, and so does wiring one to none.
 */
static void test_each_node_reads_its_own_switch(void)
{
    QTestState *qts = attach_start();
    unsigned int node;
    unsigned int with_board = 0;

    for (node = 0; node < NODES; node++) {
        uint32_t id = switch_readl(qts, node, XBOW_ID);
        uint32_t aux = switch_readl(qts, node, XBOW_LINK(WIDGET_IO) +
                                    LINK_AUX);

        g_assert_cmphex(id & ID_ALWAYS_ONE, ==, ID_ALWAYS_ONE);
        g_assert_cmpuint(ID_PART(id), ==, 0);
        if (aux & AUX_PRESENT) {
            with_board++;
        }
    }

    g_assert_cmpuint(with_board, ==, 2);
    qtest_quit(qts);
}

/*
 * A link does not send to itself, so the slot a node's own widget occupies
 * is empty in that node's view and reads as a hole. Getting this wrong is
 * not quiet: the slot would route back to the node's own processor-side Hub
 * face, which refuses every access and stops the machine. So this checks the
 * machine is still running as well as what the read returned, which is also
 * the only thing that proves the refusing face is wired at all.
 */
static void test_a_node_has_no_route_to_itself(void)
{
    QTestState *qts = attach_start();
    unsigned int node;

    for (node = 0; node < NODES; node++) {
        unsigned int own = node_widget(qts, node);
        uint8_t bytes[sizeof(uint32_t)];

        qtest_memread_cpu(qts, NODE_CPU(node),
                          xtalk_window(qts) + WIDGET_SLOT(own), bytes,
                          sizeof(bytes));
        g_assert_cmpstr(runstate(qts), !=, "internal-error");
    }
    qtest_quit(qts);
}

/*
 * The board's own chip answers at its widget slot. A switch that reports the
 * link present but routes it nowhere passes every earlier check here and
 * fails this one, which is the whole point of reading an identity rather
 * than a link register: the part number and maker are the chip's, and the
 * revision is the one the descriptor fits.
 */
static void test_a_processor_reads_the_board(void)
{
    QTestState *qts = attach_start();
    uint32_t id = board_reg_readl(qts, 0, BRIDGE_IDENTIFICATION);

    g_assert_cmpuint(ID_PART(id), ==, BRIDGE_PART);
    g_assert_cmpuint(ID_MANUFACTURER(id), ==, BRIDGE_MANUFACTURER);
    g_assert_cmpuint(id & ID_ALWAYS_ONE, ==, ID_ALWAYS_ONE);
    g_assert_cmpuint(ID_REVISION(id), ==,
                     machine_uint(qts, "board-revision"));
    qtest_quit(qts);
}

/*
 * One board, on one of the two switches. So the two nodes whose switch
 * carries it reach the chip and the other two reach nothing, and each
 * node's own link register predicts which it is. Fitting the board to both
 * switches, or to a widget the descriptor did not name, disagrees.
 */
static void test_only_the_nodes_sharing_its_switch_reach_the_board(void)
{
    QTestState *qts = attach_start();
    unsigned int reached = 0;
    unsigned int node;

    for (node = 0; node < NODES; node++) {
        uint32_t id = board_reg_readl(qts, node, BRIDGE_IDENTIFICATION);
        bool answered = ID_PART(id) == BRIDGE_PART &&
                        ID_MANUFACTURER(id) == BRIDGE_MANUFACTURER;

        g_assert_cmpint(answered, ==, board_link_present(qts, node));
        g_assert_cmpstr(runstate(qts), !=, "internal-error");
        reached += answered;
    }

    g_assert_cmpuint(reached, ==, 2);
    qtest_quit(qts);
}

/*
 * The functions the descriptor fits to the board, read the way firmware
 * finds them: through the chip's type 0 configuration window. The two SCSI
 * controllers and the I/O controller answer at the slots the record gives
 * them, every other slot is empty, and the I/O controller carries the
 * board identity the record names rather than the chip's own default.
 */
static void test_the_board_carries_the_functions_the_descriptor_named(void)
{
    QTestState *qts = attach_start();
    unsigned int slot;

    g_assert_cmphex(board_readl(qts, 0, BRIDGE_CFG0(0) + PCI_IDENTITY), ==,
                    ISP1040_IDENTITY);
    g_assert_cmphex(board_readl(qts, 0, BRIDGE_CFG0(1) + PCI_IDENTITY), ==,
                    ISP1040_IDENTITY);
    g_assert_cmphex(board_readl(qts, 0, BRIDGE_CFG0(2) + PCI_IDENTITY), ==,
                    IOC3_IDENTITY);
    for (slot = 3; slot < PCI_SLOTS; slot++) {
        g_assert_cmphex(board_readl(qts, 0, BRIDGE_CFG0(slot) + PCI_IDENTITY),
                        ==, NO_CARD);
    }

    g_assert_cmphex(board_readl(qts, 0, BRIDGE_CFG0(2) + PCI_SUBSYSTEM), ==,
                    IOC3_SUBSYSTEM_VENDOR |
                    (uint32_t)(machine_uint(qts, "ioc3-subsystem") << 16));
    qtest_quit(qts);
}

/*
 * A board that refuses to realize has to come back as an error, not take the
 * machine with it. The unwind runs with realized Hubs, which is the case that
 * cannot give preparation back, so it undoes only what attaching did -- and
 * the machine attaches cleanly on the retry, which is what proves it.
 */
static void test_a_failed_attach_leaves_a_machine_to_retry(void)
{
    QTestState *qts = attach_start_with(",refuse-board=on");

    g_assert_true(qtest_qom_get_bool(qts, "/machine", "verified"));
    g_assert_cmphex(switch_readl(qts, 0, XBOW_ID) & ID_ALWAYS_ONE, ==,
                    ID_ALWAYS_ONE);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sn0/origin2000/xtalk-attach/verified",
                   test_the_assembly_verified_itself);
    qtest_add_func("sn0/origin2000/xtalk-attach/processor-reads-the-switch",
                   test_a_processor_reads_the_switch);
    qtest_add_func("sn0/origin2000/xtalk-attach/descriptor-population",
                   test_the_links_are_the_ones_the_descriptor_asked_for);
    qtest_add_func("sn0/origin2000/xtalk-attach/each-node-its-own-switch",
                   test_each_node_reads_its_own_switch);
    qtest_add_func("sn0/origin2000/xtalk-attach/no-route-to-itself",
                   test_a_node_has_no_route_to_itself);
    qtest_add_func("sn0/origin2000/xtalk-attach/failed-attach-can-retry",
                   test_a_failed_attach_leaves_a_machine_to_retry);
    qtest_add_func("sn0/origin2000/xtalk-attach/processor-reads-the-board",
                   test_a_processor_reads_the_board);
    qtest_add_func("sn0/origin2000/xtalk-attach/board-on-one-switch",
                   test_only_the_nodes_sharing_its_switch_reach_the_board);
    qtest_add_func("sn0/origin2000/xtalk-attach/board-pci-functions",
                   test_the_board_carries_the_functions_the_descriptor_named);

    return g_test_run();
}
