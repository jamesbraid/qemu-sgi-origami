/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Node-board populations: each Origin 2000 and Onyx2 chassis fits one
 * node board up to its most, in the order SGI fills the node slots, and
 * the nodes= machine property picks the count. The fitted count decides
 * which Hubs exist, which Router boards and cables join them, and which
 * XIO slots a board can use. The tests below check the topology
 * configured by this QEMU model.
 */

#include "qemu/osdep.h"
#include <poll.h>
#include <sys/wait.h>
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-test.h"

#define XIO_REG_DATA 4
#define XBOW_LINK_STATUS(w) (0x0110 + ((w) - 8) * 0x40)
#define XBOW_LINK_AUX(w) (0x0138 + ((w) - 8) * 0x40)
#define XBOW_AUX_FAILURE_MODE (1u << 6)
#define XBOW_AUX_PRESENT (1u << 5)
#define XBOW_LINK_ALIVE (1u << 31)
#define XBOW_LINK_MAX_RETRY (1u << 5)

#define REJECT_TIMEOUT_US (30 * G_USEC_PER_SEC)

typedef struct PopulationCase {
    const char *name;
    const char *args;
    unsigned int nodes;
    unsigned int routers;
    /* The fitted cables, by the name each takes under /machine. */
    const char *cables[5];
    /* Crossbow widgets with something attached, as seen from each. */
    uint16_t crossbow0;
    /* 0: the row fits no node board that reaches a second Crossbow. */
    uint16_t crossbow1;
} PopulationCase;

/*
 * Every chassis at every count. On the eight-processor module node 1
 * enters Crossbow 0 at widget 9 and node 3 at widget A, node 2 enters
 * Crossbow 1 at widget A and node 4 at widget 9; the BaseIO sits at
 * Crossbow 0 widget 8 and an Onyx2 rack's pipe at widget F (IO3). The
 * Onyx2 deskside's one Crossbow puts node 1 at widget A, node 2 at 9,
 * the pipe at 8 and the BaseIO-G at F. Two Standard Routers join a rack
 * module's two to four node boards; a deskside joins two with a Null
 * Router and three or four with the same two Standard Routers.
 */
static const PopulationCase population_cases[] = {
    { "origin2000-rack/1",
      "-M origin2000,nodes=1 -smp 2 -m 64M",
      1, 0, { NULL }, 0x0300, 0 },
    { "origin2000-rack/2",
      "-M origin2000,nodes=2 -smp 4 -m 128M",
      2, 2, { "r1-node1", "r1-node2", "r1-r2", NULL }, 0x0300, 0x0400 },
    { "origin2000-rack/3",
      "-M origin2000,nodes=3 -smp 6 -m 192M",
      3, 2, { "r1-node1", "r1-node2", "r1-r2", "r2-node3", NULL },
      0x0700, 0x0400 },
    { "origin2000-rack/4",
      "-M origin2000,nodes=4 -smp 8 -m 256M",
      4, 2, { "r1-node1", "r1-node2", "r1-r2", "r2-node3", "r2-node4" },
      0x0700, 0x0600 },
    { "origin2000-deskside/1",
      "-M origin2000,topology=origin2000-deskside,nodes=1 -smp 2 -m 64M",
      1, 0, { NULL }, 0x0300, 0 },
    { "origin2000-deskside/2",
      "-M origin2000,topology=origin2000-deskside,nodes=2 -smp 4 -m 128M",
      2, 0, { "null-router", NULL }, 0x0300, 0x0400 },
    { "origin2000-deskside/3",
      "-M origin2000,topology=origin2000-deskside,nodes=3 -smp 6 -m 768M",
      3, 2, { "r1-node1", "r1-node2", "r1-r2", "r2-node3", NULL },
      0x0700, 0x0400 },
    { "origin2000-deskside/4",
      "-M origin2000,topology=origin2000-deskside,nodes=4 -smp 8 -m 1G",
      4, 2, { "r1-node1", "r1-node2", "r1-r2", "r2-node3", "r2-node4" },
      0x0700, 0x0600 },
    { "onyx2-rack/1",
      "-M onyx2,nodes=1 -smp 2 -m 64M",
      1, 0, { NULL }, 0x8300, 0 },
    { "onyx2-rack/2",
      "-M onyx2,nodes=2 -smp 4 -m 128M",
      2, 2, { "r1-node1", "r1-node2", "r1-r2", NULL }, 0x8300, 0x0400 },
    { "onyx2-rack/3",
      "-M onyx2,nodes=3 -smp 6 -m 192M",
      3, 2, { "r1-node1", "r1-node2", "r1-r2", "r2-node3", NULL },
      0x8700, 0x0400 },
    { "onyx2-rack/4",
      "-M onyx2,nodes=4 -smp 8 -m 256M",
      4, 2, { "r1-node1", "r1-node2", "r1-r2", "r2-node3", "r2-node4" },
      0x8700, 0x0600 },
    { "onyx2-deskside/1",
      "-M onyx2,topology=onyx2-deskside,nodes=1 -smp 2 -m 64M",
      1, 0, { NULL }, 0x8500, 0 },
    { "onyx2-deskside/2",
      "-M onyx2,topology=onyx2-deskside,nodes=2 -smp 4 -m 128M",
      2, 0, { "null-router", NULL }, 0x8700, 0 },
};

static bool machine_has_child(QTestState *qts, const char *path,
                              const char *child)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'qom-list',"
                                     " 'arguments': { 'path': %s } }", path);
    QList *list = qdict_get_qlist(response, "return");
    const QListEntry *entry;
    bool found = false;

    QLIST_FOREACH_ENTRY(list, entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(property, "name"), child)) {
            found = true;
            break;
        }
    }
    qobject_unref(response);
    return found;
}

static int64_t qom_get_int(QTestState *qts, const char *path,
                           const char *property)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'qom-get', 'arguments': {"
        "  'path': %s, 'property': %s } }", path, property);

    return qdict_get_int(response, "return");
}

static char *qom_get_link(QTestState *qts, const char *path,
                          const char *property)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'qom-get', 'arguments': {"
        "  'path': %s, 'property': %s } }", path, property);

    return g_strdup(qdict_get_str(response, "return"));
}

/*
 * One end of a cable: the Hub or Router it plugs into, the port there,
 * and the property through which that device refers back to the cable.
 */
typedef struct CableEnd {
    const char *device;
    unsigned int port;
    const char *back;
} CableEnd;

/*
 * Each cable's two ends, from 108-0170-001 Table 6-1: Router 1 port 5
 * to node board n1, 4 to n2 and 6 to Router 2's port 6; Router 2 port 5
 * to n3 and 4 to n4. Node board nK is /machine/node<K-1>, whose Hub has
 * one port, 0, and Router 1 and 2 are /machine/router0 and router1. A
 * Null Router joins n1's Hub to n2's, port 0 at each end; the manual
 * gives the cable no direction, so either order passes.
 */
static const struct {
    const char *name;
    CableEnd end[2];
} cable_ends[] = {
    { "r1-node1", { { "/machine/router0", 5, "port5-cable" },
                    { "/machine/node0/hub", 0, "net-cable" } } },
    { "r1-node2", { { "/machine/router0", 4, "port4-cable" },
                    { "/machine/node1/hub", 0, "net-cable" } } },
    { "r1-r2", { { "/machine/router0", 6, "port6-cable" },
                 { "/machine/router1", 6, "port6-cable" } } },
    { "r2-node3", { { "/machine/router1", 5, "port5-cable" },
                    { "/machine/node2/hub", 0, "net-cable" } } },
    { "r2-node4", { { "/machine/router1", 4, "port4-cable" },
                    { "/machine/node3/hub", 0, "net-cable" } } },
    { "null-router", { { "/machine/node0/hub", 0, "net-cable" },
                       { "/machine/node1/hub", 0, "net-cable" } } },
};

/*
 * Whether @path's end @side is @end: the device the cable's own link
 * names, the port index, and the device's own reference back to it.
 */
static bool cable_end_is(QTestState *qts, const char *path,
                         unsigned int side, const CableEnd *end)
{
    g_autofree char *link_prop = g_strdup_printf("end%u", side);
    g_autofree char *index_prop = g_strdup_printf("end%u-index", side);
    g_autofree char *device = qom_get_link(qts, path, link_prop);
    g_autofree char *back = NULL;

    if (!g_str_equal(device, end->device) ||
        qom_get_int(qts, path, index_prop) != end->port) {
        return false;
    }
    back = qom_get_link(qts, end->device, end->back);
    return g_str_equal(back, path);
}

static void assert_cable_ends(QTestState *qts, const char *path,
                              const char *name)
{
    for (size_t i = 0; i < G_N_ELEMENTS(cable_ends); i++) {
        const CableEnd *end = cable_ends[i].end;
        bool unordered = g_str_equal(name, "null-router");
        g_autofree char *end0 = NULL;
        g_autofree char *end1 = NULL;

        if (!g_str_equal(cable_ends[i].name, name)) {
            continue;
        }
        if (cable_end_is(qts, path, 0, &end[0]) &&
            cable_end_is(qts, path, 1, &end[1])) {
            return;
        }
        if (unordered && cable_end_is(qts, path, 0, &end[1]) &&
            cable_end_is(qts, path, 1, &end[0])) {
            return;
        }
        end0 = qom_get_link(qts, path, "end0");
        end1 = qom_get_link(qts, path, "end1");
        g_test_message("cable %s joins %s port %" PRId64 " to %s port %"
                       PRId64, name, end0,
                       qom_get_int(qts, path, "end0-index"), end1,
                       qom_get_int(qts, path, "end1-index"));
        g_assert_not_reached();
    }
    g_assert_not_reached();
}

static uint32_t crossbow_read(QTestState *qts, unsigned int cpu,
                              uint64_t offset)
{
    uint8_t bytes[sizeof(uint32_t)];

    g_assert_true(qtest_memread_cpu(qts, cpu,
                                    SPACE_IO + offset + XIO_REG_DATA,
                                    bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

/*
 * Only a port with something attached reports a widget; an absent node
 * board's port reads as an empty one, presence clear and in failed
 * initialization, so the PROM finds an empty slot, not a dead node.
 */
static void assert_crossbow_ports(QTestState *qts, unsigned int cpu,
                                  uint16_t attached)
{
    for (unsigned int widget = 8; widget < 16; widget++) {
        uint32_t aux = crossbow_read(qts, cpu, XBOW_LINK_AUX(widget));
        uint32_t status = crossbow_read(qts, cpu, XBOW_LINK_STATUS(widget));

        if (attached & (1u << widget)) {
            g_assert_cmphex(aux & XBOW_AUX_PRESENT, ==, XBOW_AUX_PRESENT);
            g_assert_cmphex(status & XBOW_LINK_ALIVE, ==, XBOW_LINK_ALIVE);
        } else {
            g_assert_cmphex(aux, ==, XBOW_AUX_FAILURE_MODE);
            g_assert_cmphex(status, ==, XBOW_LINK_MAX_RETRY);
        }
    }
}

static void test_population(const void *data)
{
    const PopulationCase *c = data;
    QTestState *qts = qtest_initf("%s -display none", c->args);
    unsigned int i, cables = 0;

    for (i = 0; i < 4; i++) {
        g_autofree char *node = g_strdup_printf("node%u", i);

        g_assert_cmpint(machine_has_child(qts, "/machine", node), ==,
                        i < c->nodes);
    }
    for (i = 0; i < 2; i++) {
        g_autofree char *router = g_strdup_printf("router%u", i);

        g_assert_cmpint(machine_has_child(qts, "/machine", router), ==,
                        i < c->routers);
    }
    for (i = 0; i < G_N_ELEMENTS(c->cables) && c->cables[i]; i++) {
        g_autofree char *path = g_strdup_printf("/machine/%s", c->cables[i]);

        g_assert_true(machine_has_child(qts, "/machine", c->cables[i]));
        assert_cable_ends(qts, path, c->cables[i]);
        cables++;
    }
    /* No cable beyond the row's: every cable name any row uses. */
    {
        static const char *const every_cable[] = {
            "r1-node1", "r1-node2", "r1-r2", "r2-node3", "r2-node4",
            "null-router",
        };
        unsigned int present = 0;

        for (i = 0; i < G_N_ELEMENTS(every_cable); i++) {
            present += machine_has_child(qts, "/machine", every_cable[i]);
        }
        g_assert_cmpuint(present, ==, cables);
    }

    /* CPU 0 sits on node 1, which enters Crossbow 0. */
    assert_crossbow_ports(qts, 0, c->crossbow0);
    g_assert_cmpint(machine_has_child(qts, "/machine", "crossbow1"), ==,
                    c->crossbow1 != 0);
    if (c->crossbow1) {
        /* CPU 2 sits on node 2, which enters Crossbow 1. */
        assert_crossbow_ports(qts, 2, c->crossbow1);
    }
    qtest_quit(qts);
}

/* The nodes property's value, which a launch left unset reads as the most. */
static void assert_default_nodes(const char *args, unsigned int expected)
{
    QTestState *qts = qtest_initf("%s -display none", args);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'qom-get', 'arguments': {"
        "  'path': '/machine', 'property': 'nodes' } }");
    g_autofree char *last = g_strdup_printf("node%u", expected - 1);
    g_autofree char *beyond = g_strdup_printf("node%u", expected);

    g_assert_cmpint(qdict_get_int(response, "return"), ==, expected);
    g_assert_true(machine_has_child(qts, "/machine", last));
    g_assert_false(machine_has_child(qts, "/machine", beyond));
    qtest_quit(qts);
}

static void test_default_is_the_most(const void *data)
{
    assert_default_nodes("-M origin2000 -smp 8", 4);
    assert_default_nodes("-M origin2000,topology=origin2000-deskside -smp 8 "
                         "-m 1G", 4);
    assert_default_nodes("-M onyx2 -smp 8", 4);
    assert_default_nodes("-M onyx2,topology=onyx2-deskside -smp 4 -m 128M",
                         2);
}

/*
 * Launch @args and require QEMU to refuse them with @message on stderr,
 * exiting rather than dying on a signal.
 */
static void assert_refused_with(const char *args, bool with_bios,
                                const char *message)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *blank = g_malloc0(0x100000);
    g_autofree char *bios = NULL;
    g_autofree char *line = NULL;
    g_auto(GStrv) argv = NULL;
    g_autoptr(GString) output = g_string_new(NULL);
    gint64 deadline = g_get_monotonic_time() + REJECT_TIMEOUT_US;
    int wait_status = 0;
    int err_fd, fd;
    GPid pid;

    /*
     * A blank PROM: the machine refuses a launch with no firmware first.
     * A launch naming pflash drives takes no -bios, which they replace.
     */
    if (with_bios) {
        fd = g_file_open_tmp("sn-node-population-bios-XXXXXX.bin", &bios,
                             &error);
        g_assert_no_error(error);
        close(fd);
        g_assert_true(g_file_set_contents(bios, (const char *)blank,
                                          0x100000, &error));
        g_assert_no_error(error);
    }
    line = g_strdup_printf("%s %s%s%s -display none -S",
                           qtest_qemu_binary(NULL), args,
                           with_bios ? " -bios " : "", with_bios ? bios : "");
    g_assert_true(g_shell_parse_argv(line, NULL, &argv, &error));
    g_assert_true(g_spawn_async_with_pipes(NULL, argv, NULL,
                                           G_SPAWN_DO_NOT_REAP_CHILD |
                                           G_SPAWN_STDOUT_TO_DEV_NULL,
                                           NULL, NULL, &pid, NULL, NULL,
                                           &err_fd, &error));
    g_assert_no_error(error);
    for (;;) {
        struct pollfd pfd = { .fd = err_fd, .events = POLLIN };
        gint64 left = deadline - g_get_monotonic_time();
        char buf[512];
        ssize_t n;

        if (left <= 0) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            close(err_fd);
            g_test_message("QEMU accepted %s", args);
            g_assert_not_reached();
        }
        if (poll(&pfd, 1, left / 1000 + 1) <= 0) {
            continue;
        }
        n = read(err_fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        g_string_append_len(output, buf, n);
    }
    close(err_fd);
    g_assert_cmpint(waitpid(pid, &wait_status, 0), ==, pid);
    g_assert_false(WIFSIGNALED(wait_status));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    if (with_bios) {
        g_assert_cmpint(unlink(bios), ==, 0);
    }
    if (!strstr(output->str, message)) {
        g_test_message("expected \"%s\", got: %s", message, output->str);
        g_assert_not_reached();
    }
}

/*
 * An absent node board has no boot flash: a two-node machine takes a
 * pflash drive for each of its two node boards, and a third drive, which
 * node board 3 would take, is one nothing claims.
 */
static void assert_refused(const char *args, const char *message)
{
    assert_refused_with(args, true, message);
}

static void test_absent_nodes_take_no_flash(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *blank = g_malloc0(0x100000);
    g_autofree char *paths[3] = { NULL, NULL, NULL };
    g_autofree char *three = NULL;
    QTestState *qts;

    for (int i = 0; i < 3; i++) {
        int fd = g_file_open_tmp("sn-node-population-flash-XXXXXX.bin",
                                 &paths[i], &error);

        g_assert_no_error(error);
        close(fd);
        g_assert_true(g_file_set_contents(paths[i], (const char *)blank,
                                          0x100000, &error));
        g_assert_no_error(error);
    }
    qts = qtest_initf("-M origin2000,nodes=2 -smp 4 -m 128M -display none "
                      "-drive if=pflash,index=0,file=%s,format=raw "
                      "-drive if=pflash,index=1,file=%s,format=raw",
                      paths[0], paths[1]);
    g_assert_true(machine_has_child(qts, "/machine", "node1"));
    g_assert_false(machine_has_child(qts, "/machine", "node2"));
    qtest_quit(qts);

    three = g_strdup_printf("-M origin2000,nodes=2 -smp 4 -m 128M "
                            "-drive if=pflash,index=0,file=%s,format=raw "
                            "-drive if=pflash,index=1,file=%s,format=raw "
                            "-drive if=pflash,index=2,file=%s,format=raw",
                            paths[0], paths[1], paths[2]);
    assert_refused_with(three, false, "machine type does not support "
                        "if=pflash,bus=0,unit=2");
    for (int i = 0; i < 3; i++) {
        g_assert_cmpint(unlink(paths[i]), ==, 0);
    }
}

static void test_refusals(const void *data)
{
    /* A count with no row. */
    assert_refused("-M origin2000,nodes=5 -smp 8",
                   "topology 'origin2000-rack' fits 1, 2, 3 or 4 node "
                   "boards, not 5");
    assert_refused("-M onyx2,topology=onyx2-deskside,nodes=3 -smp 4",
                   "topology 'onyx2-deskside' fits 1 or 2 node boards, not 3");
    assert_refused("-M origin2000,nodes=0", "nodes must be at least 1");

    /* A processor count outside the row's range, one or two per node. */
    assert_refused("-M origin2000,nodes=1 -smp 4 -m 64M",
                   "topology 'origin2000-rack' accepts 1 or 2 CPUs, not 4");
    assert_refused("-M origin2000,topology=origin2000-deskside,nodes=3 "
                   "-smp 8 -m 768M",
                   "topology 'origin2000-deskside' accepts 3, 4, 5 or 6 "
                   "CPUs, not 8");
    assert_refused("-M onyx2,topology=onyx2-deskside,nodes=2 -smp 1 -m 128M",
                   "topology 'onyx2-deskside' accepts 2, 3 or 4 CPUs, not 1");

    /*
     * An XIO slot the row cannot use names the node board it needs:
     * IO7 to IO12 are Crossbow 1's, which only node board 2 reaches.
     */
    assert_refused("-M origin2000,topology=origin2000-deskside,nodes=1 "
                   "-smp 2 -m 64M -device sgi-xio-baseio,preset=mscsi,"
                   "slot=io7",
                   "XIO slot io7 of topology 'origin2000-deskside' needs "
                   "node board n2, which nodes=1 does not fit; nodes=2 and "
                   "up fit it");
    assert_refused("-M origin2000,nodes=1 -smp 2 -m 64M "
                   "-device sgi-xio-baseio,preset=menet,slot=io12",
                   "XIO slot io12 of topology 'origin2000-rack' needs node "
                   "board n2, which nodes=1 does not fit; nodes=2 and up fit "
                   "it");
}

/*
 * The first bank still works with one node board: IO4 and IO6, which a
 * four-node module gives to node 3, take a board on node 1.
 */
static void test_one_node_first_bank(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000,nodes=1 -smp 2 -m 64M -display none "
        "-device sgi-xio-baseio,preset=mscsi,slot=io4 "
        "-device sgi-xio-baseio,preset=menet,slot=io6");

    g_assert_true(machine_has_child(qts, "/machine/module0", "io4"));
    g_assert_true(machine_has_child(qts, "/machine/module0", "io6"));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    for (size_t i = 0; i < G_N_ELEMENTS(population_cases); i++) {
        g_autofree char *path = g_strdup_printf("sn/node-population/%s",
                                                population_cases[i].name);

        qtest_add_data_func(path, &population_cases[i], test_population);
    }
    qtest_add_data_func("sn/node-population/default-is-the-most", NULL,
                        test_default_is_the_most);
    qtest_add_data_func("sn/node-population/refusals", NULL, test_refusals);
    qtest_add_data_func("sn/node-population/absent-nodes-take-no-flash",
                        NULL, test_absent_nodes_take_no_flash);
    qtest_add_data_func("sn/node-population/one-node-first-bank", NULL,
                        test_one_node_first_bank);
    return g_test_run();
}
