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
#include "qobject/qlist.h"
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

/*
 * A transfer the board starts. The card is added by the fixture rather than
 * by the descriptor, so the shipped board keeps exactly the parts it names.
 */
#define DMA_DEVICE      3
#define DMA_MMIO        0x01000000u
#define BRIDGE_DIRECT_MAP   0x080
#define BRIDGE_DEVICE(n)    (0x200 + 8 * (n))
#define DEV_SWAP_TRANSLATED (1u << 20)
#define DEV_SWAP_DIRECT     (1u << 19)
#define DEV_MEMORY_SPACE    (1u << 12)
#define DIRECT_WIDGET_SHIFT 20
#define PCI_COMMAND         0x04
#define PCI_COMMAND_MEMORY_MASTER 0x0006
#define PCI_BAR0            0x10
#define DMA_REG_ADDRESS     0x00
#define DMA_REG_LENGTH      0x04
#define DMA_REG_SEED        0x08
#define DMA_REG_GO          0x0c
#define DMA_REG_ADDRESS_HI  0x18
/*
 * The chip's device windows are not a uniform stride: the first two slots
 * get two megabytes each and the rest one, all from 0x200000. Slot three
 * therefore starts at 0x700000, and writing to 0x200000 would land in the
 * first SCSI adapter's window instead of the card's.
 */
#define DMA_WINDOW          0x700000
#define DMA_LEN             16
#define DMA_TARGET          UINT64_C(0x02000000)
#define DMA_SEED            0x40
#define PCI_DIRECT          UINT64_C(0x80000000)

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

/*
 * The pieces a Hub's own packet source needs: its register block and its
 * big windows, both inside the node's crosstalk space, the table entry that
 * aims window zero at a widget in I/O space, and the one offset that window
 * carries -- the identity of the Hub at the far end, answered from its own
 * register rather than from anything the sender supplies.
 */
#define HUB_REGISTERS   0x1000000
#define BIG_WINDOW      0x20000000
#define HUB_IO_BWIN0    0x400160
#define BWIN_IO_SPACE   (1u << 12)
#define BWIN_WIDGET(w)  ((w) << 8)
#define PACKET_IDENTITY 0x01600000
#define NI_STATUS       0x600000

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

/* The station address the command line asks for, distinct from the default. */
#define ASKED_MAC "52:54:00:aa:bb:cc"

/*
 * The adapter the one legacy drive lands behind: the first SCSI function the
 * descriptor names, on its own channel. A drive that survived on paper but
 * came back under a different adapter -- or under nothing -- fails here.
 */
#define DRIVE_OWNER "/machine/xtalk/baseio0/isp0/scsi.0/legacy[0]"

/*
 * The same fixture given the two host backends a real command line brings:
 * one legacy SCSI drive for the first adapter and one ethernet
 * configuration for the IOC3. Both are claimed while the board is built.
 */
static QTestState *attach_start_backed(const char *machine_options)
{
    return qtest_initf("-M origin2000-xtalk-attach-test%s -m 256M "
                       "-nodefaults -display none -serial none "
                       "-drive if=scsi,file=null-co://,format=raw "
                       "-nic user,id=n0,mac=" ASKED_MAC, machine_options);
}

static char *qom_string(QTestState *qts, const char *path,
                        const char *property)
{
    QDict *response = qtest_qmp(qts,
        "{ 'execute': 'qom-get', 'arguments': {"
        "  'path': %s, 'property': %s } }", path, property);
    char *value;

    g_assert_true(qdict_haskey(response, "return"));
    value = g_strdup(qdict_get_str(response, "return"));
    qobject_unref(response);
    return value;
}

/*
 * Which device owns the drive the command line named, as the monitor
 * reports it, or the empty string where no backend of that name is
 * attached to anything. The owner is the point: a rebuilt board leaves the
 * drive behind, and a drive nothing has claimed is still a drive.
 */
static char *block_backend_owner(QTestState *qts, const char *name)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'query-block' }");
    QList *devices;
    QListEntry *entry;
    char *owner = g_strdup("");

    g_assert_true(qdict_haskey(response, "return"));
    devices = qdict_get_qlist(response, "return");
    g_assert_nonnull(devices);
    QLIST_FOREACH_ENTRY(devices, entry) {
        QDict *device = qobject_to(QDict, qlist_entry_obj(entry));
        const char *qdev;

        g_assert_nonnull(device);
        if (!g_str_equal(qdict_get_str(device, "device"), name)) {
            continue;
        }
        qdev = qdict_get_try_str(device, "qdev");
        g_free(owner);
        owner = g_strdup(qdev ? qdev : "");
    }
    qobject_unref(response);
    return owner;
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

/* Whether a refused access has stopped the machine. */
static bool stopped_on_error(QTestState *qts)
{
    QDict *response = qtest_qmp_assert_success_ref(qts,
        "{ 'execute': 'query-status' }");
    bool stopped = g_str_equal(qdict_get_str(response, "status"),
                               "internal-error");

    qobject_unref(response);
    return stopped;
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

/* Which node sits at a widget, so a transfer can be checked where it lands. */
static unsigned int node_at_widget(QTestState *qts, unsigned int widget)
{
    unsigned int node;

    /*
     * Both switches number their widgets the same way, so the widget alone
     * names two nodes and one of them the board cannot reach at all. The
     * link register says which switch a node is on.
     */
    for (node = 0; node < NODES; node++) {
        if (node_widget(qts, node) == widget &&
            board_link_present(qts, node)) {
            return node;
        }
    }
    g_assert_not_reached();
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
        g_assert_false(stopped_on_error(qts));
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
        g_assert_false(stopped_on_error(qts));
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
 * the machine attaches cleanly on the retry, which is what proves it. Every
 * Hub endpoint is attached by the time the board is built, so this is a
 * failure with the whole of that to give back rather than the first step.
 */
static void test_a_failed_attach_leaves_a_machine_to_retry(void)
{
    QTestState *qts = attach_start_with(",refuse-board=on");

    g_assert_true(qtest_qom_get_bool(qts, "/machine", "verified"));
    g_assert_cmphex(switch_readl(qts, 0, XBOW_ID) & ID_ALWAYS_ONE, ==,
                    ID_ALWAYS_ONE);
    qtest_quit(qts);
}

/*
 * The last failure an attach can have: one switch realized, the other
 * refusing. Recovering from that means giving both switches up and building
 * them again, because a switch keeps the endpoints it was handed, refuses a
 * second set, and refuses to realize once it has been unrealized. So the
 * retry is only real if the machine that comes out of it routes -- reading a
 * switch register says the switches came back, and reading the board through
 * one says their endpoints did too.
 */
static void test_a_failed_switch_leaves_a_machine_to_retry(void)
{
    QTestState *qts = attach_start_with(",refuse-crossbow=on");
    uint32_t id;

    g_assert_true(qtest_qom_get_bool(qts, "/machine", "verified"));
    g_assert_cmphex(switch_readl(qts, 0, XBOW_ID) & ID_ALWAYS_ONE, ==,
                    ID_ALWAYS_ONE);
    id = board_reg_readl(qts, 0, BRIDGE_IDENTIFICATION);
    g_assert_cmpuint(ID_PART(id), ==, BRIDGE_PART);
    g_assert_cmpuint(ID_MANUFACTURER(id), ==, BRIDGE_MANUFACTURER);
    g_assert_false(stopped_on_error(qts));
    qtest_quit(qts);
}

/*
 * What a retry must not quietly lose. A drive and an ethernet configuration
 * are claimed once per process: the SCSI bus a legacy drive is looked up on
 * takes its number from a counter that only counts up, and an entry is
 * marked used the first time a device takes it. Neither can be given back,
 * so a board that has claimed them has to survive the unwind and be handed
 * to the switch that replaces the one that refused.
 *
 * Both halves are checked because they fail separately: the disk disappears
 * from the monitor entirely, while the adapter comes up carrying its own
 * built-in address instead -- which the guest also reads out of the
 * board's one-wire identity, so it is not cosmetic.
 */
static void test_a_retried_attach_keeps_its_host_backends(void)
{
    QTestState *qts = attach_start_backed(",refuse-crossbow=on");
    g_autofree char *mac = NULL;
    g_autofree char *owner = NULL;

    g_assert_true(qtest_qom_get_bool(qts, "/machine", "verified"));
    mac = qom_string(qts, "/machine/xtalk/baseio0/ioc3", "mac");
    g_assert_cmpstr(mac, ==, ASKED_MAC);
    owner = block_backend_owner(qts, "scsi0-hd0");
    g_assert_cmpstr(owner, ==, DRIVE_OWNER);
    qtest_quit(qts);
}

/*
 * The other half of the split, with the same two backends at stake. A chip
 * that refuses to realize has no functions behind it -- they are built on
 * the bus realize creates -- so that board has taken nothing, and the
 * assembly is free to release it and build a second one. The retry has to
 * come back with the drive and the address the command line asked for, the
 * same as an attach that never failed.
 */
static void test_a_rebuilt_board_takes_the_host_backends(void)
{
    QTestState *qts = attach_start_backed(",refuse-board=on");
    g_autofree char *mac = NULL;
    g_autofree char *owner = NULL;

    g_assert_true(qtest_qom_get_bool(qts, "/machine", "verified"));
    mac = qom_string(qts, "/machine/xtalk/baseio0/ioc3", "mac");
    g_assert_cmpstr(mac, ==, ASKED_MAC);
    owner = block_backend_owner(qts, "scsi0-hd0");
    g_assert_cmpstr(owner, ==, DRIVE_OWNER);
    qtest_quit(qts);
}

/*
 * The control: the same command line without the refusal. Without it the
 * check above cannot tell a retry that lost its backends from a fixture
 * that never had them.
 */
static void test_an_attach_takes_the_host_backends_it_is_given(void)
{
    QTestState *qts = attach_start_backed("");
    g_autofree char *mac = NULL;
    g_autofree char *owner = NULL;

    g_assert_true(qtest_qom_get_bool(qts, "/machine", "verified"));
    mac = qom_string(qts, "/machine/xtalk/baseio0/ioc3", "mac");
    g_assert_cmpstr(mac, ==, ASKED_MAC);
    owner = block_backend_owner(qts, "scsi0-hd0");
    g_assert_cmpstr(owner, ==, DRIVE_OWNER);
    qtest_quit(qts);
}

/*
 * Terminal teardown, where a machine owner would run it: after a successful
 * attach. No machine calls it yet -- these fixtures are its only callers.
 * Nothing exercised that before, and it took the machine down: the board's
 * devices were released with bus children still queued for release, so the
 * reference handed back a grace period later named storage already freed.
 */
static void test_an_attached_machine_can_be_torn_down(void)
{
    QTestState *qts = attach_start_with(",teardown=on");

    g_assert_true(qtest_qom_get_bool(qts, "/machine", "verified"));
    g_assert_true(qtest_qom_get_bool(qts, "/machine", "released"));
    qtest_quit(qts);
}

/*
 * How many PCI functions the monitor can see, read the way a monitor reads
 * them: query-pci walks the PCI core's own list of host bridges, which is a
 * file-scope list that holds no reference to what is on it.
 */
static size_t pci_functions_seen(QTestState *qts)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'query-pci' }");
    QList *buses;
    QListEntry *entry;
    size_t count = 0;

    g_assert_true(qdict_haskey(response, "return"));
    buses = qdict_get_qlist(response, "return");
    g_assert_nonnull(buses);
    QLIST_FOREACH_ENTRY(buses, entry) {
        QDict *bus = qobject_to(QDict, qlist_entry_obj(entry));
        QList *devices;

        g_assert_nonnull(bus);
        devices = qdict_get_qlist(bus, "devices");
        g_assert_nonnull(devices);
        count += qlist_size(devices);
    }
    qobject_unref(response);
    return count;
}

/*
 * Reading the PCI topology after the board has been given up. The chip that
 * carried it has to have left the core's host-bridge list on the way out,
 * because the list outlives it and the monitor walks it on demand. The board
 * is the only PCI host this machine has, so nothing is left to report.
 */
static void test_the_monitor_reads_pci_after_teardown(void)
{
    QTestState *qts = attach_start_with(",teardown=on");

    g_assert_true(qtest_qom_get_bool(qts, "/machine", "released"));
    g_assert_cmpuint(pci_functions_seen(qts), ==, 0);
    g_assert_false(stopped_on_error(qts));
    qtest_quit(qts);
}

/*
 * The same read after an attach that failed and was retried. Here the board
 * is still fitted, so the three functions the descriptor names have to come
 * back exactly once -- a stale entry would either double them or answer out
 * of storage the retry released.
 */
static void test_the_monitor_reads_pci_after_a_retry(void)
{
    QTestState *qts = attach_start_with(",refuse-crossbow=on");

    g_assert_true(qtest_qom_get_bool(qts, "/machine", "verified"));
    g_assert_cmpuint(pci_functions_seen(qts), ==, 3);
    g_assert_false(stopped_on_error(qts));
    qtest_quit(qts);
}

/*
 * A board that could not be finished ends the attach for good. A function
 * takes its host configuration partway through its own realize, so from
 * outside a board that is short of complete cannot be told apart from one
 * that took a drive or an ethernet address and then failed. Building a
 * second board would produce a machine quietly missing what the first took,
 * so the assembly gives the board up and refuses to attach again.
 *
 * Reaching this point at all is the other half of the check. qdev asserts
 * at the end of machine init that every device under the machine is
 * realized, so the function the PCI core refused has to have come off the
 * board rather than stayed on it unrealized.
 */
static void test_a_part_built_board_ends_the_attach(void)
{
    QTestState *qts = attach_start_with(",refuse-function=on");

    g_assert_true(qtest_qom_get_bool(qts, "/machine", "refused-again"));
    g_assert_false(qtest_qom_get_bool(qts, "/machine", "verified"));
    g_assert_cmpuint(pci_functions_seen(qts), ==, 0);
    g_assert_false(stopped_on_error(qts));
    qtest_quit(qts);
}

static uint64_t hub_readq(QTestState *qts, unsigned int node,
                          uint64_t offset)
{
    uint8_t bytes[sizeof(uint64_t)];

    g_assert_true(qtest_memread_cpu(qts, NODE_CPU(node),
                                    xtalk_window(qts) + HUB_REGISTERS +
                                    offset, bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

/*
 * What a Hub gets when it asks a widget who it is. The window entry names
 * the widget; the reply comes back through this Hub's own packet source, so
 * the answer says which switch port that source is attached to.
 */
static uint64_t peer_identity(QTestState *qts, unsigned int node,
                              unsigned int widget)
{
    uint8_t entry[sizeof(uint64_t)];
    uint8_t bytes[sizeof(uint64_t)];

    stq_be_p(entry, BWIN_IO_SPACE | BWIN_WIDGET(widget));
    g_assert_true(qtest_memwrite_cpu(qts, NODE_CPU(node),
                                     xtalk_window(qts) + HUB_REGISTERS +
                                     HUB_IO_BWIN0, entry, sizeof(entry)));
    g_assert_true(qtest_memread_cpu(qts, NODE_CPU(node),
                                    xtalk_window(qts) + BIG_WINDOW +
                                    PACKET_IDENTITY, bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

/*
 * The packet side of the switch, which no identity read through a processor
 * window touches: each Hub sends from its own port, so it reaches the Hub
 * sharing its switch and cannot reach itself. Attaching the sources to each
 * other's ports inverts both halves of that.
 */
static void test_a_hub_asks_its_switch_mate_who_it_is(void)
{
    QTestState *qts = attach_start();
    unsigned int a = node_at_widget(qts, WIDGET_HUB_A);
    unsigned int b = node_at_widget(qts, WIDGET_HUB_B);

    g_assert_cmphex(hub_readq(qts, a, NI_STATUS), !=, 0);
    g_assert_cmphex(peer_identity(qts, a, WIDGET_HUB_B), ==,
                    hub_readq(qts, b, NI_STATUS));
    g_assert_cmphex(peer_identity(qts, b, WIDGET_HUB_A), ==,
                    hub_readq(qts, a, NI_STATUS));

    /* A link does not send to itself here either, and nothing answers. */
    g_assert_cmphex(peer_identity(qts, a, WIDGET_HUB_A), ==, 0);
    g_assert_cmphex(peer_identity(qts, b, WIDGET_HUB_B), ==, 0);
    qtest_quit(qts);
}

/*
 * A Hub's own window into crosstalk I/O space outlives the assembly that
 * gave it one: it is an alias built at realize, and a realized Hub does not
 * give its link back. So the window still exists after teardown, still aims
 * where a guest points it, and has to reach nothing rather than reach into
 * storage somebody has since freed. The identity read is the same one that
 * answers while the machine is assembled.
 */
static void test_a_torn_down_big_window_reaches_nothing(void)
{
    QTestState *qts = attach_start_with(",teardown=on");

    g_assert_true(qtest_qom_get_bool(qts, "/machine", "released"));
    g_assert_cmphex(peer_identity(qts, 0, WIDGET_HUB_A), ==, 0);
    g_assert_cmphex(peer_identity(qts, 0, WIDGET_HUB_B), ==, 0);
    g_assert_false(stopped_on_error(qts));
    qtest_quit(qts);
}

/*
 * The other Hub's processor-side face, which refuses every access and stops
 * the machine. Reaching it needs a node on the same switch, which the two
 * Hub widgets are. The no-route-to-itself check argues this face is wired by
 * the machine not stopping; this is the same claim from the other side. The
 * second access is what a paired take-and-release of the stop lock would
 * deadlock or double-release on.
 */
static void test_the_other_hubs_face_refuses_and_stops(void)
{
    QTestState *qts = attach_start();
    unsigned int own = node_widget(qts, 0);
    unsigned int peer = own == WIDGET_HUB_A ? WIDGET_HUB_B : WIDGET_HUB_A;
    uint8_t bytes[sizeof(uint32_t)];

    g_assert_false(stopped_on_error(qts));
    qtest_memread_cpu(qts, NODE_CPU(0), xtalk_window(qts) + WIDGET_SLOT(peer),
                      bytes, sizeof(bytes));
    g_assert_true(stopped_on_error(qts));
    qtest_memread_cpu(qts, NODE_CPU(0), xtalk_window(qts) + WIDGET_SLOT(peer),
                      bytes, sizeof(bytes));
    g_assert_true(stopped_on_error(qts));
    qtest_quit(qts);
}

/* The switch carrying the board: base I/O at 8, the two Hubs at 9 and 10. */
#define CROSSBOW0       "/machine/xtalk/crossbow0"
#define PRESENT         ((1u << WIDGET_IO) | (1u << WIDGET_HUB_A) | \
                         (1u << WIDGET_HUB_B))

static void set_links_alive(QTestState *qts, unsigned int widgets)
{
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'qom-set', 'arguments': {"
        "  'path': '" CROSSBOW0 "', 'property': 'links-alive',"
        "  'value': %u } }", widgets);
}

static void board_writel(QTestState *qts, unsigned int node,
                         unsigned int offset, uint32_t value)
{
    uint8_t bytes[sizeof(uint32_t)];

    stl_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, NODE_CPU(node),
                                     xtalk_window(qts) + board_slot(qts) +
                                     offset, bytes, sizeof(bytes)));
}

static void board_reg_writel(QTestState *qts, unsigned int node,
                             unsigned int offset, uint32_t value)
{
    board_writel(qts, node, offset + REG_DATA, value);
}

/*
 * A processor's bytes leave in the machine's own order; turning them round
 * for a little-endian card is the chip's byte-lane job, not the test's.
 */
static void card_writel(QTestState *qts, unsigned int node, unsigned int reg,
                        uint32_t value)
{
    uint8_t bytes[sizeof(uint32_t)];

    stl_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, NODE_CPU(node),
                                     xtalk_window(qts) + board_slot(qts) +
                                     DMA_WINDOW + reg, bytes, sizeof(bytes)));
}

/*
 * Give the card its registers, let it master the bus, aim its window at
 * them, and point the direct route at a Hub. Both byte-lane bits are set so
 * a transfer carries the bytes it was given: what these read is the address.
 */
static void card_start(QTestState *qts, unsigned int node,
                       unsigned int widget)
{
    board_writel(qts, node, BRIDGE_CFG0(DMA_DEVICE) + PCI_BAR0, DMA_MMIO);
    board_writel(qts, node, BRIDGE_CFG0(DMA_DEVICE) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY_MASTER);
    board_reg_writel(qts, node, BRIDGE_DEVICE(DMA_DEVICE),
                     DEV_SWAP_TRANSLATED | DEV_SWAP_DIRECT |
                     DEV_MEMORY_SPACE | (DMA_MMIO >> 20));
    board_reg_writel(qts, node, BRIDGE_DIRECT_MAP,
                     widget << DIRECT_WIDGET_SHIFT);
}

static void card_transfer(QTestState *qts, unsigned int node,
                          uint64_t pci_addr, uint32_t seed)
{
    card_writel(qts, node, DMA_REG_ADDRESS, (uint32_t)pci_addr);
    card_writel(qts, node, DMA_REG_ADDRESS_HI, pci_addr >> 32);
    card_writel(qts, node, DMA_REG_LENGTH, DMA_LEN);
    card_writel(qts, node, DMA_REG_SEED, seed);
    card_writel(qts, node, DMA_REG_GO, 1);
}

/*
 * A pattern no transfer produces. Zeroing and asserting zero cannot tell a
 * transfer that never arrived from one that arrived carrying zeroes.
 */
static void sentinel_bytes(uint8_t *bytes, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        bytes[i] = 0xa5 ^ (uint8_t)(i * 7);
    }
}

static void seed_the_landing(QTestState *qts, unsigned int node)
{
    uint8_t sentinel[DMA_LEN];

    sentinel_bytes(sentinel, sizeof(sentinel));
    g_assert_true(qtest_memwrite_cpu(qts, NODE_CPU(node), DMA_TARGET,
                                     sentinel, sizeof(sentinel)));
}

static void assert_landed(QTestState *qts, unsigned int node, uint32_t seed)
{
    uint8_t seen[DMA_LEN];
    unsigned int i;

    g_assert_true(qtest_memread_cpu(qts, NODE_CPU(node), DMA_TARGET, seen,
                                    sizeof(seen)));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(seed + i));
    }
}

static void assert_untouched(QTestState *qts, unsigned int node)
{
    uint8_t seen[DMA_LEN];
    uint8_t sentinel[DMA_LEN];
    unsigned int i;

    sentinel_bytes(sentinel, sizeof(sentinel));
    g_assert_true(qtest_memread_cpu(qts, NODE_CPU(node), DMA_TARGET, seen,
                                    sizeof(seen)));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, sentinel[i]);
    }
}

static QTestState *dma_start(void)
{
    return attach_start_with(" -device sgi-bridge-dma-test,"
                             "bus=pci.baseio0,addr=3");
}

/*
 * A transfer the board starts reaches the Hub its route names, and only
 * that one. Aiming it twice is what separates a switch that routes on the
 * destination field from one that delivers to whoever is listening.
 */
static void test_board_dma_reaches_the_widget_it_names(void)
{
    QTestState *qts = dma_start();
    unsigned int a = node_at_widget(qts, WIDGET_HUB_A);
    unsigned int b = node_at_widget(qts, WIDGET_HUB_B);

    seed_the_landing(qts, a);
    seed_the_landing(qts, b);
    card_start(qts, a, WIDGET_HUB_A);
    card_transfer(qts, a, PCI_DIRECT | DMA_TARGET, DMA_SEED);
    assert_landed(qts, a, DMA_SEED);
    assert_untouched(qts, b);

    seed_the_landing(qts, a);
    card_start(qts, a, WIDGET_HUB_B);
    card_transfer(qts, a, PCI_DIRECT | DMA_TARGET, DMA_SEED);
    assert_landed(qts, b, DMA_SEED);
    assert_untouched(qts, a);
    qtest_quit(qts);
}

/*
 * A destination whose link is down receives nothing, and the other one is
 * unaffected. A switch could get this right looking only at where a packet
 * is addressed. The board is driven from the node whose link stays up,
 * because a processor reaches it across the same switch.
 */
static void test_a_down_destination_takes_only_its_own_traffic(void)
{
    QTestState *qts = dma_start();
    unsigned int down = node_at_widget(qts, WIDGET_HUB_A);
    unsigned int driver = node_at_widget(qts, WIDGET_HUB_B);

    seed_the_landing(qts, down);
    seed_the_landing(qts, driver);
    set_links_alive(qts, PRESENT & ~(1u << WIDGET_HUB_A));

    card_start(qts, driver, WIDGET_HUB_A);
    card_transfer(qts, driver, PCI_DIRECT | DMA_TARGET, DMA_SEED);
    assert_untouched(qts, down);

    card_start(qts, driver, WIDGET_HUB_B);
    card_transfer(qts, driver, PCI_DIRECT | DMA_TARGET, DMA_SEED);
    assert_landed(qts, driver, DMA_SEED);
    qtest_quit(qts);
}

/*
 * The board's own link is the one every transfer leaves by. With it down
 * nothing the board sends arrives at either Hub, and restoring it brings the
 * programming from before back without a processor touching the chip again.
 *
 * That link is also the one a processor reaches the board over, so this
 * cannot say which of the two refusals stopped the transfer. Separating them
 * needs a source a processor can still drive with its link down, which is
 * what the Hub-source check below uses.
 */
static void test_a_down_source_stops_every_transfer(void)
{
    QTestState *qts = dma_start();
    unsigned int a = node_at_widget(qts, WIDGET_HUB_A);
    unsigned int b = node_at_widget(qts, WIDGET_HUB_B);

    card_start(qts, a, WIDGET_HUB_A);
    seed_the_landing(qts, a);
    seed_the_landing(qts, b);

    set_links_alive(qts, PRESENT & ~(1u << WIDGET_IO));
    card_transfer(qts, a, PCI_DIRECT | DMA_TARGET, DMA_SEED);
    assert_untouched(qts, a);
    assert_untouched(qts, b);

    set_links_alive(qts, PRESENT);
    card_transfer(qts, a, PCI_DIRECT | DMA_TARGET, DMA_SEED);
    assert_landed(qts, a, DMA_SEED);
    assert_untouched(qts, b);
    qtest_quit(qts);
}

/*
 * A source sends nothing while its own link is down, however alive the
 * widget it names. A Hub is the source that shows this: a processor drives
 * its own Hub's packet source through the node rather than across the
 * switch, so the trigger survives the link going down. A switch that looked
 * only at where a packet was addressed would deliver this one.
 */
static void test_a_down_source_sends_nothing(void)
{
    QTestState *qts = attach_start();
    unsigned int a = node_at_widget(qts, WIDGET_HUB_A);
    unsigned int b = node_at_widget(qts, WIDGET_HUB_B);
    uint64_t identity = hub_readq(qts, b, NI_STATUS);

    g_assert_cmphex(peer_identity(qts, a, WIDGET_HUB_B), ==, identity);
    set_links_alive(qts, PRESENT & ~(1u << WIDGET_HUB_A));
    g_assert_cmphex(peer_identity(qts, a, WIDGET_HUB_B), ==, 0);
    set_links_alive(qts, PRESENT);
    g_assert_cmphex(peer_identity(qts, a, WIDGET_HUB_B), ==, identity);
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
    qtest_add_func("sn0/origin2000/xtalk-attach/hub-face-refuses",
                   test_the_other_hubs_face_refuses_and_stops);
    qtest_add_func("sn0/origin2000/xtalk-attach/packet-peer-identity",
                   test_a_hub_asks_its_switch_mate_who_it_is);
    qtest_add_func("sn0/origin2000/xtalk-attach/failed-attach-can-retry",
                   test_a_failed_attach_leaves_a_machine_to_retry);
    qtest_add_func("sn0/origin2000/xtalk-attach/failed-switch-can-retry",
                   test_a_failed_switch_leaves_a_machine_to_retry);
    qtest_add_func("sn0/origin2000/xtalk-attach/attach-takes-its-backends",
                   test_an_attach_takes_the_host_backends_it_is_given);
    qtest_add_func("sn0/origin2000/xtalk-attach/retry-keeps-its-backends",
                   test_a_retried_attach_keeps_its_host_backends);
    qtest_add_func("sn0/origin2000/xtalk-attach/rebuild-takes-its-backends",
                   test_a_rebuilt_board_takes_the_host_backends);
    qtest_add_func("sn0/origin2000/xtalk-attach/part-built-board-is-final",
                   test_a_part_built_board_ends_the_attach);
    qtest_add_func("sn0/origin2000/xtalk-attach/attached-machine-tears-down",
                   test_an_attached_machine_can_be_torn_down);
    qtest_add_func("sn0/origin2000/xtalk-attach/torn-down-big-window",
                   test_a_torn_down_big_window_reaches_nothing);
    qtest_add_func("sn0/origin2000/xtalk-attach/torn-down-pci-topology",
                   test_the_monitor_reads_pci_after_teardown);
    qtest_add_func("sn0/origin2000/xtalk-attach/retried-pci-topology",
                   test_the_monitor_reads_pci_after_a_retry);
    qtest_add_func("sn0/origin2000/xtalk-attach/board-dma-reaches-its-widget",
                   test_board_dma_reaches_the_widget_it_names);
    qtest_add_func("sn0/origin2000/xtalk-attach/down-destination-only-its-own",
                   test_a_down_destination_takes_only_its_own_traffic);
    qtest_add_func("sn0/origin2000/xtalk-attach/down-source-stops-everything",
                   test_a_down_source_stops_every_transfer);
    qtest_add_func("sn0/origin2000/xtalk-attach/down-source-sends-nothing",
                   test_a_down_source_sends_nothing);
    qtest_add_func("sn0/origin2000/xtalk-attach/processor-reads-the-board",
                   test_a_processor_reads_the_board);
    qtest_add_func("sn0/origin2000/xtalk-attach/board-on-one-switch",
                   test_only_the_nodes_sharing_its_switch_reach_the_board);
    qtest_add_func("sn0/origin2000/xtalk-attach/board-pci-functions",
                   test_the_board_carries_the_functions_the_descriptor_named);

    return g_test_run();
}
