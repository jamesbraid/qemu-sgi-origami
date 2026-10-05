/*
 * The format-2 builder (sn-machine.c): the command-line contract every
 * product shares: -numa refused, an unsupported -smp or -m population
 * rejected, and a topology belonging to a different product refused;
 * checked against the real Origin 200, Origin 200 (dual), Origin 2000 and
 * Origin 2000 (one-node) machines, whose every accepted population is also
 * checked to build. The console field test drives
 * tests/qtest/sn-builder-fault-test.c's sn-builder-console-test machine, a
 * real two-node build.
 *
 * The one-memory-transaction cases count flat-view renders before the
 * first reset, so a builder that commits per device again fails here. The
 * reset-renders cases count them during a system reset, so a reset path
 * that commits per region again fails too.
 *
 * The named structural faults sgi_machine_validate_topology used to reject
 * here (a minimal two-node fixture topology fed straight to that validator)
 * are gone along with it: hw/mips/sgi/sn-machine.c now trusts the compiled
 * descriptor, and tests/unit/test-sn-model.c checks those invariants once
 * instead.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "qemu/bswap.h"
#include "hw/pci/pci_regs.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"
#include "sgi-test.h"
#include "sgi-migrate.h"
#include "sgi-l1-test.h"

static char *qom_get_str(QTestState *qts, const char *path, const char *prop)
{
    QDict *r = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                         "{'path': %s, 'property': %s}}", path, prop);
    char *value = g_strdup(qdict_get_str(r, "return"));

    qobject_unref(r);
    return value;
}

static uint64_t qom_get_uint(QTestState *qts, const char *path,
                             const char *prop)
{
    QDict *r = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                         "{'path': %s, 'property': %s}}", path, prop);
    uint64_t value = qdict_get_uint(r, "return");

    qobject_unref(r);
    return value;
}

static bool qom_get_bool(QTestState *qts, const char *path,
                         const char *prop)
{
    QDict *r = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                         "{'path': %s, 'property': %s}}", path, prop);
    bool value = qdict_get_bool(r, "return");

    qobject_unref(r);
    return value;
}

/*
 * How long a refused launch may take to exit. A QEMU that wrongly accepts
 * the arguments runs the guest instead, so without a bound this would
 * wait for the whole test's timeout rather than fail by name.
 */
#define REJECT_TIMEOUT_US (60 * G_USEC_PER_SEC)

static void assert_qemu_rejects(const char *const args[], const char *message)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GString) error_output = g_string_new(NULL);
    gint64 deadline = g_get_monotonic_time() + REJECT_TIMEOUT_US;
    int wait_status = 0;
    int err_fd;
    GPid pid;

    g_assert_true(g_spawn_async_with_pipes(NULL, (char **)args, NULL,
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
            g_test_message("QEMU accepted %s; stderr so far: %s", args[0],
                           error_output->str);
            g_assert_not_reached();
        }
        if (poll(&pfd, 1, left / 1000 + 1) <= 0) {
            continue;
        }
        n = read(err_fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        g_string_append_len(error_output, buf, n);
    }
    close(err_fd);
    g_assert_cmpint(waitpid(pid, &wait_status, 0), ==, pid);
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    /*
     * Require a reported error and normal failure exit, not SIGABRT or
     * SIGSEGV; WIFSIGNALED distinguishes signals from nonzero status.
     */
    g_assert_false(WIFSIGNALED(wait_status));
    if (!strstr(error_output->str, message)) {
        g_autofree char *command = g_strjoinv(" ", (char **)args);

        g_printerr("QEMU refusal mismatch\ncommand: %s\n"
                   "expected stderr substring: %s\nactual stderr: %s\n",
                   command, message, error_output->str);
    }
    g_assert_nonnull(strstr(error_output->str, message));
}

static char *create_sized_temp_file(const char *template, off_t size)
{
    g_autoptr(GError) error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp(template, &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

/*
 * Read a named QOM child's type, or NULL if absent, to distinguish
 * board classes even when their properties match.
 */
static char *qom_child_type(QTestState *qts, const char *parent,
                            const char *child)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'qom-list',"
                                     " 'arguments': { 'path': %s } }",
                                     parent);
    QList *children;
    const QListEntry *entry;
    char *found = NULL;

    g_assert_true(qdict_haskey(response, "return"));
    children = qdict_get_qlist(response, "return");
    for (entry = qlist_first(children); entry; entry = qlist_next(entry)) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));
        const char *type = qdict_get_try_str(item, "type");

        if (type && g_str_has_prefix(type, "child<") &&
            g_str_equal(qdict_get_str(item, "name"), child)) {
            found = g_strndup(type + strlen("child<"),
                              strlen(type) - strlen("child<") - 1);
        }
    }

    qobject_unref(response);
    return found;
}

/*
 * An empty XIO slot's own Crossbow port, the fabric a board given one
 * outright names: the packet ingress of that slot's widget on its
 * Crossbow. Origin 2000 IO4 is Crossbow 0 widget 11, IO7 Crossbow 1
 * widget 14 and IO9 Crossbow 1 widget 15.
 */
#define SLOT_PORT(crossbow, widget) \
    "/machine/crossbow" #crossbow "/sgi-xbow.widget" #widget \
    ".ingress.packet[0]"

/*
 * A Crossbow's own uint property, the same way sn0-origin2000-xtalk-
 * owner-test.c's assert_crossbow() reads one: used here to confirm a late
 * `-device` board attaching through an empty slot's fabric proxy leaves the
 * Crossbow's own count of populated widgets untouched.
 */
static uint64_t crossbow_uint(QTestState *qts, const char *crossbow,
                              const char *property)
{
    g_autofree char *path = g_strdup_printf("/machine/%s", crossbow);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': %s,"
        "'property': %s}}", path, property);

    g_assert_false(qdict_haskey(response, "error"));
    return qdict_get_int(response, "return");
}

/*
 * Whether typename's own class (device-list-properties always walks its
 * parents) declares a qdev property named prop, checked with no instance
 * of either class ever attached, to prove two board classes share a property
 * by name and not merely by coincidence at one particular attachment.
 */
static bool device_class_has_property(QTestState *qts, const char *typename,
                                      const char *prop)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'device-list-properties',"
                                     " 'arguments': { 'typename': %s } }",
                                     typename);
    QList *properties;
    const QListEntry *entry;
    bool found = false;

    g_assert_true(qdict_haskey(response, "return"));
    properties = qdict_get_qlist(response, "return");
    for (entry = qlist_first(properties); entry; entry = qlist_next(entry)) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(item, "name"), prop)) {
            found = true;
            break;
        }
    }
    qobject_unref(response);
    return found;
}

static void test_numa_refused(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-m", "64M",
        "-object", "memory-backend-ram,id=ram0,size=64M",
        "-numa", "node,nodeid=0,memdev=ram0",
        "-display", "none", NULL
    };

    assert_qemu_rejects(args, "origin200: -numa is refused");
}

/*
 * sgi-pci-expansion (board-pci-expansion.c) is user-creatable: a `-device`
 * board placed by widget number, as here, names its own "fabric" (a
 * board placed by slot name gets its slot's from the machine, see
 * test_pci_expansion_attaches_at_a_named_slot) -- so a bare
 * `-device sgi-pci-expansion,module=1,slot=9`
 * reaches past qdev's pluggable-device check and refuses for a later,
 * different reason: the same missing-fabric error build_widget() reports
 * from the machine builder or the board-test fixture. -device is
 * processed after sgi_machine_init, so this still needs a -bios that
 * clears the stock-PROM requirement, the same blank flash
 * write_config_block_fixture's sibling in sn0-address-map-test.c uses for
 * the same reason.
 */
static void test_pci_expansion_requires_fabric(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *blank = g_malloc0(0x100000);
    g_autofree char *path = NULL;
    int fd;
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000", "-bios", NULL,
        "-device", "sgi-pci-expansion,module=1,slot=9",
        "-display", "none", NULL
    };

    fd = g_file_open_tmp("sn-builder-blank-bios-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    close(fd);
    g_assert_true(g_file_set_contents(path, (const char *)blank, 0x100000,
                                      &error));
    g_assert_no_error(error);
    args[4] = path;

    assert_qemu_rejects(args, "sgi-pci-expansion requires the fabric its "
                        "interrupt packets travel");
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * sgi-pci-expansion and sgi-xio-baseio are the two -device-creatable XIO
 * board classes (board-pci-expansion.c, xio-baseio.c); sgi-xio-board
 * (xio-board.c) is their shared, non-instantiable base, which owns
 * "module" and "slot". Each subclass repeats "fabric" by name and type
 * itself -- its own internal Bridge needs the link typed on its own
 * class, not the base -- rather than one board's -device surface
 * differing from the other's for what is otherwise the same attach step.
 */
static void test_expansion_boards_share_the_attach_contract(const void *data)
{
    static const char * const shared_properties[] = {
        "module", "slot", "fabric",
    };
    static const char * const board_classes[] = {
        "sgi-pci-expansion", "sgi-xio-baseio",
    };
    QTestState *qts = qtest_init("-M origin2000 -display none");
    size_t i, j;

    for (i = 0; i < ARRAY_SIZE(board_classes); i++) {
        for (j = 0; j < ARRAY_SIZE(shared_properties); j++) {
            g_assert_true(device_class_has_property(qts, board_classes[i],
                                                     shared_properties[j]));
        }
    }

    qtest_quit(qts);
}

/*
 * A `-device` board names the empty XIO slot it plugs into, and the
 * machine hands it that slot's fabric proxy (TYPE_SGI_XIO_SLOTS,
 * xio-board.h): the board takes the slot's module and widget, and
 * sgi_xio_board_reparent() (xio-board.c) lands it under that module. On
 * the default origin2000 topology IO4 is crossbow0's widget 11.
 *
 * crossbow0's own startup-links-alive (sn0-origin2000-xtalk-owner-test.c:
 * 0x0700, its onboard BaseIO and two host nodes) stays exactly
 * that after this board attaches. sgi_machine_done() does bring the
 * slot's widget alive and register it as an inbound destination once a
 * board claims it (sgi_xbow_attach_endpoint_live() then
 * sgi_xbow_mark_widget_alive(), see board-pci-expansion.c's header
 * comment), but through the Crossbow's runtime link state, not by
 * rewriting its configured "startup-links-alive" -- the declared boot
 * health a reset restores.
 */
static void test_pci_expansion_attaches_at_a_named_slot(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none -device sgi-pci-expansion,slot=io4");
    g_autofree char *type = qom_child_type(qts, "/machine/module0", "io4");

    g_assert_cmpstr(type, ==, "sgi-pci-expansion");
    g_assert_cmphex(crossbow_uint(qts, "crossbow0", "startup-links-alive"), ==,
                    UINT64_C(0x0700));
    qtest_quit(qts);
}

/* Runs QEMU with one -device and checks it refuses with @message. */
static void assert_device_refused(const char *device, const char *message)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *blank = g_malloc0(0x100000);
    g_autofree char *path = NULL;
    int fd;
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000", "-bios", NULL,
        "-device", device, "-display", "none", NULL
    };

    fd = g_file_open_tmp("sn-builder-blank-bios-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    close(fd);
    g_assert_true(g_file_set_contents(path, (const char *)blank, 0x100000,
                                      &error));
    g_assert_no_error(error);
    args[4] = path;

    assert_qemu_rejects(args, message);
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * A BaseIO sits in IO1. Fitted by slot name it takes no other slot, even
 * one whose widget, F, is IO1 on the Onyx2 deskside; placed by widget,
 * the plain BaseIO still refuses that widget, which only a BaseIO-G on a
 * deskside occupies.
 */
static void test_xio_baseio_fits_only_io1(const void *data)
{
    assert_device_refused("sgi-xio-baseio,preset=io6g,slot=io9",
                          "sgi-xio-baseio preset 'io6g' fits only XIO slot "
                          "io1, not io9");
    assert_device_refused("sgi-xio-baseio,preset=io6,module=0,slot=15,"
                          "fabric=" SLOT_PORT(1, 15),
                          "sgi-xio-baseio preset 'io6' cannot sit at "
                          "XIO widget 15");
}

/*
 * A slot name the topology does not have, one its own board fills, a
 * module the slot is not in, and a second board in a claimed slot are
 * each refused by name, rather than landing the board somewhere else.
 */
static void test_named_slot_refusals(const void *data)
{
    assert_device_refused("sgi-pci-expansion,slot=io13",
                          "topology 'origin2000-rack' has no empty XIO "
                          "slot io13");
    assert_device_refused("sgi-pci-expansion,slot=io1",
                          "XIO slot io1 of topology 'origin2000-rack' "
                          "already holds the topology's own board");
    assert_device_refused("sgi-pci-expansion,slot=io4,module=1",
                          "XIO slot io4 of topology 'origin2000-rack' is "
                          "in module 0, not module 1");
}

/* Two -device boards on a blank-PROM origin2000 are refused with @message. */
static void assert_device_pair_refused(const char *first, const char *second,
                                       const char *message)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *blank = g_malloc0(0x100000);
    g_autofree char *path = NULL;
    int fd;
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000", "-bios", NULL,
        "-device", first, "-device", second, "-display", "none", NULL
    };

    fd = g_file_open_tmp("sn-builder-blank-bios-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    close(fd);
    g_assert_true(g_file_set_contents(path, (const char *)blank, 0x100000,
                                      &error));
    g_assert_no_error(error);
    args[4] = path;

    assert_qemu_rejects(args, message);
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * A board's id names it and its PCI bus, so a second device may not take
 * one a board already holds, whether the board's own slot name or an id
 * given outright; qdev alone would not notice once the board has moved
 * under its module.
 */
static void test_board_ids_are_unique(const void *data)
{
    assert_device_pair_refused("sgi-xio-baseio,preset=menet,slot=io5",
                               "sgi-xio-baseio,preset=menet,slot=io4,id=io5",
                               "a device with id 'io5' already exists");
    assert_device_pair_refused("sgi-xio-baseio,preset=menet,slot=io4,id=io5",
                               "sgi-xio-baseio,preset=menet,slot=io5",
                               "a device with id 'io5' already exists");
}

/*
 * A module's two Crossbows number their widgets alike: IO5 is Crossbow 0
 * widget 14 and IO7 Crossbow 1 widget 14. Both take a board, each on its
 * own fabric, and the two read different one-wire serials; a second
 * board on either one's fabric is still refused.
 */
static void test_boards_on_two_crossbows(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none "
        "-device sgi-xio-baseio,preset=menet,slot=io5 "
        "-device sgi-xio-baseio,preset=menet,slot=io7");

    g_assert_cmphex(qom_get_uint(qts, "/machine/module0/io5",
                                 "onewire-serial"), !=,
                    qom_get_uint(qts, "/machine/module0/io7",
                                 "onewire-serial"));
    g_assert_cmphex(qom_get_uint(qts, "/machine/module0/io5",
                                 "ioc3-onewire-serial"), !=,
                    qom_get_uint(qts, "/machine/module0/io7",
                                 "ioc3-onewire-serial"));
    qtest_quit(qts);

    assert_device_pair_refused(
        "sgi-xio-baseio,preset=menet,slot=io7",
        "sgi-pci-expansion,module=0,slot=14,fabric=" SLOT_PORT(1, 14),
        "XIO slot io7 already holds a board");
}

/*
 * The same two positions reached by widget number, each board handed its
 * slot's fabric outright and given no id: both are widget 14 of module 0,
 * so each takes its name from the slot its fabric resolves to rather than
 * from the widget, which would name both slot14.
 */
static void test_widget_boards_on_two_crossbows(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none "
        "-device sgi-pci-expansion,module=0,slot=14,"
        "fabric=" SLOT_PORT(0, 14) " "
        "-device sgi-pci-expansion,module=0,slot=14,"
        "fabric=" SLOT_PORT(1, 14));
    g_autofree char *io5 = qom_child_type(qts, "/machine/module0", "io5");
    g_autofree char *io7 = qom_child_type(qts, "/machine/module0", "io7");

    g_assert_cmpstr(io5, ==, "sgi-pci-expansion");
    g_assert_cmpstr(io7, ==, "sgi-pci-expansion");
    qtest_quit(qts);

    /* A board that already took the slot's name as its id keeps it. */
    assert_device_pair_refused(
        "sgi-xio-baseio,preset=menet,slot=io5,id=io7",
        "sgi-pci-expansion,module=0,slot=14,fabric=" SLOT_PORT(1, 14),
        "module 0 already holds a device named 'io7'");
}

static void test_named_slot_takes_one_board(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *blank = g_malloc0(0x100000);
    g_autofree char *path = NULL;
    int fd;
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000", "-bios", NULL,
        "-device", "sgi-pci-expansion,slot=io4",
        "-device", "sgi-pci-expansion,slot=io4,pci-bus-name=second",
        "-display", "none", NULL
    };

    fd = g_file_open_tmp("sn-builder-blank-bios-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    close(fd);
    g_assert_true(g_file_set_contents(path, (const char *)blank, 0x100000,
                                      &error));
    g_assert_no_error(error);
    args[4] = path;

    assert_qemu_rejects(args, "XIO slot io4 already holds a board");
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * A board given a slot's fabric outright, as a fixture does, must name
 * that slot's own module and widget: the board no longer lands on the
 * fabric's widget while claiming another. It is then known by that
 * slot's name.
 */
static void test_explicit_fabric_must_match_its_slot(const void *data)
{
    QTestState *qts;

    assert_device_refused("sgi-pci-expansion,module=0,slot=12,"
                          "fabric=" SLOT_PORT(0, 11),
                          "fabric of XIO slot io4 is module 0 widget 11, "
                          "not the module 0 widget 12 the board names");
    qts = qtest_init("-M origin2000 -display none -device "
                     "sgi-pci-expansion,module=0,slot=11,"
                     "fabric=" SLOT_PORT(0, 11));
    g_assert_cmpstr(qom_child_type(qts, "/machine/module0", "io4"), ==,
                    "sgi-pci-expansion");
    qtest_quit(qts);
}

/*
 * The Hub's own real XIO decode, confirmed against a real widget by
 * sn0-origin2000-xtalk-owner-test.c's assert_owner(): a physical read at
 * SPACE_IO plus a widget's small-window stride plus a register's own
 * offset reaches that widget's real chip registers through the real
 * Crossbow. Reused here read-only, on the machine's own onboard BaseIO
 * (XIO widget 8, the io6 preset's own xio_widgets), to observe whatever
 * the board fitted at IO4 below actually delivers to it.
 */
#define SMALL_WINDOW_SIZE       UINT64_C(0x1000000)
#define SMALL_WINDOW(widget)    (SPACE_IO + \
                                 (uint64_t)(widget) * SMALL_WINDOW_SIZE)
#define REG_DATA                4
#define BASEIO_WIDGET           8

/*
 * The spare board's own Bridge, driven the same way sn0-bridge-widget-
 * test.c's card_start()/card_transfer() drive a bare one. The board is now
 * addressable through crossbow0's own small window for its widget (see
 * test_pci_expansion_is_inbound_addressable below), but this outbound test
 * keeps to a scratch physical base of its own via qtest_sysbus_mmio_map(),
 * exercising the board's outbound path without leaning on the inbound one;
 * the base sits well clear of every address a real origin2000 topology ever
 * composes.
 */
#define SPARE_BASE              UINT64_C(0xf0000000000)
#define SPARE_REG(offset)       (SPARE_BASE + (offset) + REG_DATA)
#define SPARE_BRIDGE            "/machine/module0/io4/bridge"
#define SENTINEL                0xa5

/*
 * BaseIO's own read response buffer registers (hw/pci-host/sgi-bridge.c,
 * BRIDGE_RRB_EVEN): plain 32-bit storage with a full writable mask and no
 * side effect, unlike an interrupt-host address register, whose writable
 * bits cover only part of the word and would silently drop the rest of
 * whatever pattern a transfer carries.
 */
#define BASEIO_RRB_EVEN         0x280

/*
 * The card this board's own Bridge masters, driven exactly the way
 * sn0-bridge-widget-test.c's card_start()/card_transfer() drive one against
 * a bare Bridge: its config space and window 0 sit at their bare offsets,
 * neither being a chip register with the +REG_DATA skew.
 */
#define SPARE_CFG0(slot)        (SPARE_BASE + 0x020000 + (slot) * 0x1000)
#define SPARE_WINDOW0           (SPARE_BASE + 0x200000)
#define SPARE_DEVICE(n)         (0x200 + 8 * (n))
#define DEV_SWAP_TRANSLATED     (1u << 20)
#define DEV_SWAP_DIRECT         (1u << 19)
#define CARD_SLOT               0
#define CARD_MMIO               0x01000000u
#define PCI_COMMAND_MEMORY_MASTER 0x0006
#define DMA_REG_ADDRESS         0x00
#define DMA_REG_LENGTH          0x04
#define DMA_REG_SEED            0x08
#define DMA_REG_GO              0x0c
#define DMA_REG_ADDRESS_HI      0x18

/*
 * The 64-bit-direct route's own destination: bits 63:60 of a bus-mastered
 * PCI address name a widget (sgi_bridge_translate_common(),
 * hw/pci-host/sgi-bridge.c) and the low 48 bits pass straight through to
 * sgi_bridge_compose_xio() -- the same composer sgi_bridge_send_
 * interrupt() uses, so a widget's own decode of the result cannot tell
 * the two delivery mechanisms apart. Bit 47 of that low half is what
 * sgi_bridge_realize() maps a widget's own registers onto (the packet_io
 * alias sits at 1 << (SGI_BRIDGE_PACKET_ADDRESS_BITS - 1)), and REG_DATA is
 * the same +4 skew every read in this file adds at the point of access; here
 * it has to be baked into the destination address itself, since a DMA
 * transfer has no separate access-time step once under way.
 */
#define DEST_WIDGET_SHIFT       60
#define DEST_PACKET_IO          (UINT64_C(1) << 47)
#define DEST_OFFSET             (BASEIO_RRB_EVEN + REG_DATA)
#define DEST_PCI_ADDRESS        (((uint64_t)BASEIO_WIDGET << \
                                DEST_WIDGET_SHIFT) | DEST_PACKET_IO | \
                                DEST_OFFSET)
#define DEST_SEED               0x10

/*
 * Give the card its registers and let it master the bus, the same way
 * sn0-bridge-widget-test.c's card_start() does for a bare Bridge. Both
 * byte-lane bits are set so a transfer carries the bytes it was given: what
 * this test reads is the address, not the order.
 */
static void spare_card_start(QTestState *qts)
{
    qtest_writel(qts, SPARE_CFG0(CARD_SLOT) + PCI_BAR0, CARD_MMIO);
    qtest_writel(qts, SPARE_CFG0(CARD_SLOT) + PCI_COMMAND,
                PCI_COMMAND_MEMORY_MASTER);
    qtest_writel(qts, SPARE_REG(SPARE_DEVICE(CARD_SLOT)),
                DEV_SWAP_TRANSLATED | DEV_SWAP_DIRECT | DEV_MEMORY_SPACE |
                (CARD_MMIO >> 20));
}

static void spare_card_transfer(QTestState *qts, uint64_t pci_addr,
                                uint32_t length, uint32_t seed)
{
    qtest_writel(qts, SPARE_WINDOW0 + DMA_REG_ADDRESS, (uint32_t)pci_addr);
    qtest_writel(qts, SPARE_WINDOW0 + DMA_REG_ADDRESS_HI, pci_addr >> 32);
    qtest_writel(qts, SPARE_WINDOW0 + DMA_REG_LENGTH, length);
    qtest_writel(qts, SPARE_WINDOW0 + DMA_REG_SEED, seed);
    qtest_writel(qts, SPARE_WINDOW0 + DMA_REG_GO, 1);
}

static void assert_spare_landed(QTestState *qts, uint64_t at, uint32_t seed)
{
    uint8_t seen[sizeof(uint32_t)];
    unsigned int i;

    qtest_memread(qts, at, seen, sizeof(seen));
    for (i = 0; i < sizeof(seen); i++) {
        g_assert_cmpuint(seen[i], ==, (uint8_t)(seed + i));
    }
}

/*
 * The through-fabric half of the same attach: a spare board's own Bridge
 * masters a real PCI DMA transfer from its own attached card, through the
 * proxy sgi_machine_done() finishes, to BaseIO. BaseIO is alive at boot
 * and shares the fabric. The board's header comment
 * (board-pci-expansion.c) says outbound transfers can reach it. The
 * transfer takes the 64-bit-direct
 * route sn0-bridge-widget-test.c's own WIDE_WIDGET route exercises against a
 * bare fixture; here the widget field names the real BaseIO widget and the
 * destination is one of its own chip registers. BASEIO_RRB_EVEN has no side
 * effect of its own, so a value observed there can only have arrived as the
 * transfer spare_card_transfer() below sends; nothing else in this test or
 * in either board's own realize ever touches it.
 *
 * sgi_xbow_sync_routes() disables a port's entire ingress/packet_ingress
 * region whenever that port's own sgi_xbow_alive() is false, always true
 * for the empty slot's widget this board claims (IO4, crossbow0's widget
 * 11, outside its startup-links-alive of 0x0700) -- this board's own
 * outbound transfers have nowhere live to
 * originate from until something marks that widget alive without also
 * counting it as one of the Crossbow's own onboard links.
 */
static void test_pci_expansion_delivers_through_spare_fabric(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none -device sgi-pci-expansion,"
        "slot=io4 "
        "-device sgi-bridge-dma-test,bus=io4.0,addr=0");
    uint64_t dest = SMALL_WINDOW(BASEIO_WIDGET) + BASEIO_RRB_EVEN;

    qtest_sysbus_mmio_map(qts, SPARE_BRIDGE, 0, SPARE_BASE);

    /* A known value proves the read/write path itself, not just a zero. */
    qtest_writel(qts, dest + REG_DATA, SENTINEL);
    g_assert_cmphex(qtest_readl(qts, dest + REG_DATA), ==, SENTINEL);

    spare_card_start(qts);

    /* Nothing has been sent yet: still the sentinel, not the transfer. */
    g_assert_cmphex(qtest_readl(qts, dest + REG_DATA), ==, SENTINEL);

    spare_card_transfer(qts, DEST_PCI_ADDRESS, sizeof(uint32_t), DEST_SEED);
    assert_spare_landed(qts, dest + REG_DATA, DEST_SEED);

    qtest_quit(qts);
}

/*
 * A transfer the Crossbow drops has no response, so the spare board's
 * Bridge hears of a dropped read only from its own request timer:
 * interrupt status bit 9, with the PCI address and the requesting device
 * in the response buffer error registers (Bridge ASIC specification rev
 * 4.0, table 58; tables 18 and 19). A read naming widget 2 is an
 * illegal destination, which the Crossbow also records on the port the
 * transfer arrived by, IO4's widget 11; one naming widget 12, legal but
 * empty, is dropped with nothing recorded at the Crossbow.
 */
#define DMA_REG_SOURCE          0x14
#define DMA_REG_SOURCE_HI       0x1c
#define DMA_GO_COPY             2
#define BRIDGE_RESP_ERR_UPPER   0x060
#define BRIDGE_RESP_ERR_LOWER   0x068
#define BRIDGE_INT_STATUS       0x100
#define BRIDGE_INT_ENABLE_REG   0x108
#define BRIDGE_XREAD_TIMEOUT    (1u << 9)
#define XBOW_LINK_STATUS(w)     (SMALL_WINDOW(0) + 0x100 + ((w) - 8) * 0x40 + \
                                 0x10 + REG_DATA)
#define XBOW_ILLEGAL_DEST       (1u << 17)
#define SPARE_XBOW_WIDGET       11

static uint64_t dropped_read_source(unsigned int widget)
{
    return ((uint64_t)widget << DEST_WIDGET_SHIFT) | 0x1000;
}

static QTestState *spare_card_read_from(uint64_t source)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none -device sgi-pci-expansion,"
        "slot=io4 "
        "-device sgi-bridge-dma-test,bus=io4.0,addr=0");

    qtest_sysbus_mmio_map(qts, SPARE_BRIDGE, 0, SPARE_BASE);
    spare_card_start(qts);
    qtest_writel(qts, SPARE_REG(BRIDGE_INT_ENABLE_REG), BRIDGE_XREAD_TIMEOUT);
    qtest_writel(qts, SPARE_WINDOW0 + DMA_REG_SOURCE, (uint32_t)source);
    qtest_writel(qts, SPARE_WINDOW0 + DMA_REG_SOURCE_HI, source >> 32);
    qtest_writel(qts, SPARE_WINDOW0 + DMA_REG_ADDRESS,
                 (uint32_t)DEST_PCI_ADDRESS);
    qtest_writel(qts, SPARE_WINDOW0 + DMA_REG_ADDRESS_HI,
                 DEST_PCI_ADDRESS >> 32);
    qtest_writel(qts, SPARE_WINDOW0 + DMA_REG_LENGTH, sizeof(uint32_t));
    qtest_writel(qts, SPARE_WINDOW0 + DMA_REG_GO, DMA_GO_COPY);
    return qts;
}

static void assert_spare_read_timed_out(QTestState *qts, uint64_t source)
{
    g_assert_cmphex(qtest_readl(qts, SPARE_REG(BRIDGE_INT_STATUS)) &
                    BRIDGE_XREAD_TIMEOUT, ==, BRIDGE_XREAD_TIMEOUT);
    g_assert_cmphex(qtest_readl(qts, SPARE_REG(BRIDGE_RESP_ERR_UPPER)), ==,
                    (CARD_SLOT << 20) | ((source >> 32) & 0xffff));
    g_assert_cmphex(qtest_readl(qts, SPARE_REG(BRIDGE_RESP_ERR_LOWER)), ==,
                    (uint32_t)source);
}

static void test_dropped_read_to_illegal_widget_times_out(const void *data)
{
    uint64_t source = dropped_read_source(2);
    QTestState *qts = spare_card_read_from(source);

    assert_spare_read_timed_out(qts, source);
    g_assert_cmphex(qtest_readl(qts, XBOW_LINK_STATUS(SPARE_XBOW_WIDGET)) &
                    XBOW_ILLEGAL_DEST, ==, XBOW_ILLEGAL_DEST);
    qtest_quit(qts);
}

static void test_dropped_read_to_empty_widget_times_out(const void *data)
{
    uint64_t source = dropped_read_source(12);
    QTestState *qts = spare_card_read_from(source);

    assert_spare_read_timed_out(qts, source);
    g_assert_cmphex(qtest_readl(qts, XBOW_LINK_STATUS(SPARE_XBOW_WIDGET)) &
                    XBOW_ILLEGAL_DEST, ==, 0);
    qtest_quit(qts);
}

/* A dropped write has no response to wait for, so nothing is recorded. */
static void test_dropped_write_records_nothing(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none -device sgi-pci-expansion,"
        "slot=io4 "
        "-device sgi-bridge-dma-test,bus=io4.0,addr=0");

    qtest_sysbus_mmio_map(qts, SPARE_BRIDGE, 0, SPARE_BASE);
    spare_card_start(qts);
    qtest_writel(qts, SPARE_REG(BRIDGE_INT_ENABLE_REG), BRIDGE_XREAD_TIMEOUT);
    spare_card_transfer(qts, dropped_read_source(12), sizeof(uint32_t),
                        DEST_SEED);
    g_assert_cmphex(qtest_readl(qts, SPARE_REG(BRIDGE_INT_STATUS)) &
                    BRIDGE_XREAD_TIMEOUT, ==, 0);
    qtest_quit(qts);
}

/*
 * ---- Inbound addressability and interrupt delivery ----
 *
 * These tests fit a `-device` board at IO4, an empty slot whose widget is
 * crossbow0's 11, outside its startup-links-alive (0x0700 -- BaseIO at
 * widget 8 and the two host nodes at 9 and 10). A board that
 * takes the slot's proxy is addressable at that widget, so its own Bridge
 * answers a processor through crossbow0's real small window for widget
 * 11. The same XIO decode SMALL_WINDOW(BASEIO_WIDGET) reaches the
 * onboard BaseIO. Neither path needs a scratch sysbus map.
 */
#define SPARE_WIDGET            11
#define SPARE_IN                SMALL_WINDOW(SPARE_WIDGET)
#define SPARE_IN_REG(offset)    (SPARE_IN + (offset) + REG_DATA)
#define SPARE_IN_CFG0(slot)     (SPARE_IN + 0x020000 + (slot) * 0x1000)
#define SPARE_IN_WINDOW0        (SPARE_IN + 0x200000)
#define INBOUND_SENTINEL        0x5a5aa5a5u

/* The bus-mastering card's interrupt register (sgi-bridge-dma-test.c). */
#define DMA_REG_INTERRUPT       0x10

/*
 * The expansion Bridge's own interrupt registers (hw/pci-host/sgi-bridge.c).
 * With interrupt-pin INTA the card in slot S maps to Bridge pin S
 * (sgi_bridge_map_irq()), so the card in slot 0 uses pin 0.
 */
#define BRIDGE_INT_DEST_UPPER   0x030
#define BRIDGE_INT_DEST_LOWER   0x038
#define BRIDGE_INT_ENABLE       0x108
#define BRIDGE_INT_HOST(n)      (0x130 + 8 * (n))
#define BRIDGE_DEVICE_REG(n)    (0x200 + 8 * (n))

/*
 * A host node's Hub sits at widget 9 on crossbow0. The Bridge
 * composes a packet address carrying the destination widget
 * in bits 51:48, bit 47 marking packet I/O, the nasid in bits 45:38, and the
 * Hub's own interrupt-modify offset (0x01800000 + 0x90) below. An INT_HOST
 * address field of 0x200 sets bit 47 with nasid 0, so node 0's own Hub,
 * read at HUB_LOCAL, whichever host-node widget the packet enters by, since
 * the Hub's claim layer routes by programmed node number, latches it.
 */
#define HUB_WIDGET_ON_XBOW      9
#define HUB_INT_MODIFY_OFFSET   0x01800090
#define INT_HOST_PACKET_IO_NASID0 0x200
#define SPARE_INT_NUMBER        42

/*
 * A board that takes the spare fabric proxy is addressable at its own
 * widget: crossbow0's real small window for widget 11 reaches the board's
 * own Bridge registers with no scratch sysbus map. BRIDGE_RRB_EVEN (0x280,
 * the same benign fully-writable storage register sn0-bridge-widget-test.c
 * and the through-fabric test above use) round-trips a sentinel only once
 * the spare widget is an inbound destination; before that it reads back the
 * zero an unrouted XIO hole returns.
 */
static void test_pci_expansion_is_inbound_addressable(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none -device sgi-pci-expansion,"
        "slot=io4");

    qtest_writel(qts, SPARE_IN_REG(BASEIO_RRB_EVEN), INBOUND_SENTINEL);
    g_assert_cmphex(qtest_readl(qts, SPARE_IN_REG(BASEIO_RRB_EVEN)), ==,
                    INBOUND_SENTINEL);
    qtest_quit(qts);
}

/*
 * A real interrupt-capable card behind the expansion Bridge delivers to the
 * guest: the processor programs the board's Bridge through the same inbound
 * small window, aims its interrupt pin at node 0's Hub interrupt-modify
 * register, enables the pin, and the card raises INTx. The packet travels
 * out through the expansion Bridge, up through crossbow0 to the host node's
 * Hub, and node 0's Hub PI_INT_PEND0 latches the programmed interrupt
 * number. Without inbound addressability the processor cannot program the
 * Bridge at all, so nothing arms and nothing latches.
 */
static void test_pci_expansion_delivers_card_interrupt_to_hub(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none -device sgi-pci-expansion,"
        "slot=io4,pci-bus-name=xpci "
        "-device sgi-bridge-dma-test,bus=xpci,addr=0");

    /* Nothing has fired yet. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0) &
                    (1ULL << SPARE_INT_NUMBER), ==, 0);

    /* Give the card its register window and let it master the bus. */
    qtest_writel(qts, SPARE_IN_CFG0(CARD_SLOT) + PCI_BAR0, CARD_MMIO);
    qtest_writel(qts, SPARE_IN_CFG0(CARD_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY_MASTER);
    qtest_writel(qts, SPARE_IN_REG(BRIDGE_DEVICE_REG(CARD_SLOT)),
                 DEV_MEMORY_SPACE | (CARD_MMIO >> 20));

    /* Aim pin 0 at node 0's Hub interrupt-modify register and enable it. */
    qtest_writel(qts, SPARE_IN_REG(BRIDGE_INT_DEST_UPPER),
                 HUB_WIDGET_ON_XBOW << 16);
    qtest_writel(qts, SPARE_IN_REG(BRIDGE_INT_DEST_LOWER),
                 HUB_INT_MODIFY_OFFSET);
    qtest_writel(qts, SPARE_IN_REG(BRIDGE_INT_HOST(CARD_SLOT)),
                 (INT_HOST_PACKET_IO_NASID0 << 8) | SPARE_INT_NUMBER);
    qtest_writel(qts, SPARE_IN_REG(BRIDGE_INT_ENABLE), 1u << CARD_SLOT);

    /* The card raises INTx; the packet must reach node 0's Hub. */
    qtest_writel(qts, SPARE_IN_WINDOW0 + DMA_REG_INTERRUPT, 1);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0) &
                    (1ULL << SPARE_INT_NUMBER), ==, 1ULL << SPARE_INT_NUMBER);

    qtest_quit(qts);
}

/*
 * A real modeled interrupt-capable card, the dual-channel QLogic ISP12160
 * SCSI host adapter (hw/scsi/qlogic-isp.c), plugged into the expansion
 * board's own PCI bus is enumerable behind the expansion widget: its PCI
 * configuration space answers its own vendor/device identity (0x1077/0x1216)
 * through crossbow0's small window for the spare widget, the very config
 * accesses a driver's own probe issues. On trunk the spare widget is no
 * inbound destination, so the identity reads back the zero an unrouted
 * XIO hole returns.
 */
#define ISP12160_ID_PAIR        0x12161077u
static void test_expansion_card_enumerates_inbound(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none -device sgi-pci-expansion,"
        "slot=io4,pci-bus-name=xpci "
        "-device qlogic-isp12160,bus=xpci,addr=3");

    g_assert_cmphex(qtest_readl(qts, SPARE_IN_CFG0(3)), ==, ISP12160_ID_PAIR);
    qtest_quit(qts);
}

/*
 * IO4's widget is an empty XIO slot until a board claims it: its
 * Crossbow port reports no widget and the failed-initialization state, so
 * firmware passes over it. A board taking the proxy is a widget physically
 * attached to that port, which then reads present and alive (link auxiliary
 * status bit 5 and link status bit 31). Crossbow 0's own registers are
 * widget 0 of its small windows.
 */
#define SPARE_LINK_STATUS       (SMALL_WINDOW(0) + 0x110 + \
                                (SPARE_WIDGET - 8) * 0x40 + REG_DATA)
#define SPARE_LINK_AUX          (SMALL_WINDOW(0) + 0x138 + \
                                (SPARE_WIDGET - 8) * 0x40 + REG_DATA)
#define LINK_ALIVE              (1u << 31)
#define LINK_MAX_RETRY          (1u << 5)
#define AUX_FAILURE_MODE        (1u << 6)
#define AUX_PRESENT             (1u << 5)

static void test_spare_widget_present_only_when_claimed(const void *data)
{
    QTestState *qts = qtest_init("-M origin2000 -display none");

    g_assert_cmphex(qtest_readl(qts, SPARE_LINK_AUX), ==, AUX_FAILURE_MODE);
    g_assert_cmphex(qtest_readl(qts, SPARE_LINK_STATUS), ==, LINK_MAX_RETRY);
    qtest_quit(qts);

    qts = qtest_init(
        "-M origin2000 -display none -device sgi-pci-expansion,"
        "slot=io4");
    g_assert_cmphex(qtest_readl(qts, SPARE_LINK_AUX), ==, AUX_PRESENT);
    g_assert_cmphex(qtest_readl(qts, SPARE_LINK_STATUS), ==, LINK_ALIVE);
    qtest_quit(qts);
}

/*
 * The stock PROM resets every link it finds attached and expects one that
 * was alive before the reset to come back after the pulse; the machine gives
 * every topology link that restore. A board claimed at an empty slot is
 * attached the same way, so a link reset of its port must bring it back
 * present and alive rather than leave it initializing.
 */
#define SPARE_LINK_RESET        (SMALL_WINDOW(0) + 0x130 + \
                                (SPARE_WIDGET - 8) * 0x40 + REG_DATA)
#define LINK_RESET_NS           20

static void test_claimed_spare_widget_survives_link_reset(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none -device sgi-pci-expansion,"
        "slot=io4");

    qtest_writel(qts, SPARE_LINK_RESET, 0);
    g_assert_cmphex(qtest_readl(qts, SPARE_LINK_STATUS), ==, 0);
    qtest_clock_step(qts, LINK_RESET_NS);
    g_assert_cmphex(qtest_readl(qts, SPARE_LINK_AUX), ==, AUX_PRESENT);
    g_assert_cmphex(qtest_readl(qts, SPARE_LINK_STATUS), ==, LINK_ALIVE);
    qtest_quit(qts);
}

static void test_board_selection_refusals(const void *data)
{
    g_autofree char *data_path = create_sized_temp_file(
        "sn-builder-board-refusal-data-XXXXXX", 0x800);
    g_autofree char *clock_path = create_sized_temp_file(
        "sn-builder-board-refusal-clock-XXXXXX", 16);
    g_autofree char *data_drive = NULL;
    g_autofree char *clock_drive = NULL;
    /* Existing CPU identities only let the fixtures reach selector checks. */
    const char *ip53[] = {
        qtest_qemu_binary(NULL), "-M",
        "sn-builder-bedrock-test,construction-fault=ip53",
        "-m", "128M", "-smp", "2", "-cpu", "R14000",
        "-bios", "/dev/null", "-drive", NULL, "-drive", NULL,
        "-display", "none", NULL
    };
    const char *non_bedrock[] = {
        qtest_qemu_binary(NULL), "-M",
        "sn-builder-console-test,construction-fault="
        "board-selector-on-non-bedrock",
        "-m", "128M", "-smp", "4", "-cpu", "R10000",
        "-bios", "/dev/null", "-display", "none", NULL
    };
    const char *direct_ip53[] = {
        qtest_qemu_binary(NULL), "-M",
        "sn-builder-bedrock-nasid-test,construction-fault=ip53",
        "-smp", "2", "-cpu", "R14000", "-display", "none", NULL
    };

    data_drive = g_strdup_printf(
        "if=none,id=sgi-ds1742-data-fixture-io9,file=%s,format=raw",
        data_path);
    clock_drive = g_strdup_printf(
        "if=none,id=sgi-ds1742-clock-fixture-io9,file=%s,format=raw",
        clock_path);
    ip53[12] = data_drive;
    ip53[14] = clock_drive;
    assert_qemu_rejects(ip53, "IP53 construction requires the runtime "
                        "experiment permission or a qtest fixture");
    g_unlink(data_path);
    g_unlink(clock_path);
    assert_qemu_rejects(non_bedrock,
                        "topology 'sn-builder-console-test' node 'node0-hub' "
                        "has board-kind on a non-Bedrock node");
    assert_qemu_rejects(direct_ip53,
                        "sgi-sn1-node cannot realize board kind 'ip53'");
}

static void test_pic_io9_descriptor_refusals(const void *data)
{
    static const struct {
        const char *fault;
        const char *message;
    } cases[] = {
        { "pic-io9", "has unsupported PIC/IO9 descriptors" },
        { "pic-host-only", "has unsupported PIC/IO9 descriptors" },
        { "io9-with-xio-reference", "has inconsistent XIO and PCI references" },
        { "io9-missing-pci-head", "has inconsistent XIO and PCI references" },
        { "io9-bad-pci-host",
          "gives I/O board 'fixture-io9' a PCI host index out of range" },
        { "io9-bad-pci-head", "has out-of-range PCI head 2" },
        { "io9-duplicate-host-head",
          "assigns PCI host/head pair 0/0 to multiple IO9 boards" },
        { "pic-duplicate-xio",
          "topology 'sn-builder-console-test' assigns XIO attachment 2 to "
          "more than one PCI host" },
        { "pic-xio-out-of-range",
          "topology 'sn-builder-console-test' gives PCI host 'fixture-pic' "
          "an XIO attachment index out of range" },
        { "io9-empty-host-array",
          "topology 'sn-builder-console-test' gives I/O board 'fixture-io9' "
          "a PCI host index out of range" },
        { "legacy-without-attachment",
          "has inconsistent XIO and PCI references" },
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *machine = g_strdup_printf(
            "sn-builder-console-test,construction-fault=%s", cases[i].fault);
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M", machine, "-m", "128M",
            "-smp", "4", "-cpu", "R10000", "-bios", "/dev/null",
            "-display", "none", NULL
        };

        assert_qemu_rejects(args, cases[i].message);
    }
}

static void test_unsupported_smp_population_rejected(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-smp", "3",
        "-display", "none", NULL
    };

    assert_qemu_rejects(args,
                        "topology 'origin200' accepts 1 or 2 CPUs, not 3");
}

static void test_topology_from_another_product_refused(const void *data)
{
    /*
 * The compiled catalogue holds every product's topologies in one list;
 * topology= names one by string alone, so sgi_machine_topology() also checks
 * the resolved entry against this machine's own product
 * (sgi_machine_topology_matches_product) before handing it back. Origin
 * 2000's "origin2000-rack" descriptor exists and resolves by name just as
 * well for Origin 200's product class, so without that check Origin 200
 * would build Origin 2000's four-node wiring under its own product. The
 * product mismatch is refused the way any other unknown name is:
 * sgi_machine_topology() returns NULL either way.
 */
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200,topology=origin2000-rack",
        "-display", "none", NULL
    };

    assert_qemu_rejects(args, "unknown topology 'origin2000-rack'");
}

/*
 * Every accepted -smp/-m population actually builds, for every topology that
 * ships today. These are the accepted counterparts to the rejection tests.
 * qtest_enabled() waives the stock-PROM/-bios requirement, so a bare
 * qtest_init() that returns at all, without QEMU exiting first, is the pass:
 * sgi_machine_init ran its validation and built every node.
 */
static void test_origin200_accepts_its_cpu_populations(const void *data)
{
    static const unsigned int cpus[] = { 1, 2 };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(cpus); i++) {
        g_autofree char *args = g_strdup_printf(
            "-M origin200 -smp %u -display none", cpus[i]);
        QTestState *qts = qtest_init(args);

        qtest_quit(qts);
    }
}

static void test_origin200_accepts_its_memory_populations(const void *data)
{
    static const unsigned int mib[] = { 1664, 512, 256, 192, 128, 64 };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(mib); i++) {
        g_autofree char *args = NULL;
        QTestState *qts;

        /* The quick setup builds the largest and smallest only. */
        if (i != 0 && i != ARRAY_SIZE(mib) - 1 && !g_test_slow()) {
            continue;
        }
        args = g_strdup_printf("-M origin200 -m %uM -display none", mib[i]);
        qts = qtest_init(args);

        qtest_quit(qts);
    }
}

static void test_origin200_dual_accepts_its_populations(const void *data)
{
    /*
 * default_ram_size is origin200's own shipped total (one node), so -m must
 * be named explicitly here: left out, it would divide across this topology's
 * two real nodes and fail, not fall back to a value this topology accepts.
 */
    static const unsigned int mib_per_node[] = { 1664, 512, 256, 192, 128, 64 };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(mib_per_node); i++) {
        g_autofree char *args = NULL;
        QTestState *qts;

        /* The quick setup builds the largest and smallest only. */
        if (i != 0 && i != ARRAY_SIZE(mib_per_node) - 1 && !g_test_slow()) {
            continue;
        }
        args = g_strdup_printf("-M origin200,topology=origin200-dual -smp 4 "
                               "-m %uM -display none", mib_per_node[i] * 2);
        qts = qtest_init(args);

        qtest_quit(qts);
    }
}

/* Processors per node, from query-cpus-fast's node-id properties. */
static void count_cpus_per_node(QTestState *qts, unsigned int *counts,
                                unsigned int nodes)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
                                          "{'execute':'query-cpus-fast'}");
    QList *cpus = qdict_get_qlist(response, "return");
    const QListEntry *entry;

    memset(counts, 0, nodes * sizeof(*counts));
    QLIST_FOREACH_ENTRY(cpus, entry) {
        QDict *cpu = qobject_to(QDict, qlist_entry_obj(entry));
        int64_t node = qdict_get_int(qdict_get_qdict(cpu, "props"), "node-id");

        g_assert_cmpint(node, <, nodes);
        counts[node]++;
    }
}

static void test_origin300_direct_v12_builds_device(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    g_autofree char *path = NULL;
    g_autofree char *node = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn-builder-o300-v12-prom-XXXXXX.bin", &path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);

    qts = qtest_initf("-M origin300 -cpu R14000 -smp 2 -m 512M "
                      "-bios %s -S", path);
    node = qom_child_type(qts, "/machine", "node0");
    g_assert_cmpstr(node, ==, "sgi-sn1-node");
    qtest_quit(qts);
    g_clear_pointer(&node, g_free);

    qts = qtest_initf("-M origin300,topology=origin300-v12-direct "
                      "-cpu R14000 -smp 2 -m 512M -bios %s -S", path);
    node = qom_child_type(qts, "/machine/module0", "rear-panel");
    g_assert_cmpstr(node, ==, "sgi-odyssey");
    g_assert_null(qom_child_type(qts, "/machine", "module1"));
    qtest_quit(qts);
    g_unlink(path);
}

/* The two-chip experiment gives the remote XBridge and V12 distinct PIO. */
static void test_origin300_vbrick_projects_v12(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    g_autofree char *path = NULL;
    g_autofree char *type = NULL;
    const uint64_t port = UINT64_C(0x20000000000) +
                          11 * UINT64_C(0x1000000);
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn-builder-o300-vbrick-prom-XXXXXX.bin", &path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);

    qts = qtest_initf("-M origin300,topology=origin300-vbrick "
                      "-cpu R14000 -smp 2 -m 512M -bios %s -S", path);
    type = qom_child_type(qts, "/machine/module1", "vbrick-xbridge");
    g_assert_cmpstr(type, ==, "sgi-xbridge");
    g_clear_pointer(&type, g_free);
    type = qom_child_type(qts, "/machine/module1", "vbrick-l1");
    g_assert_cmpstr(type, ==, "container");
    g_clear_pointer(&type, g_free);
    type = qom_child_type(qts, "/machine/module1", "vbrick-v12");
    g_assert_cmpstr(type, ==, "sgi-odyssey");
    /* The node cable and internal V12 link both report a live widget. */
    g_assert_cmphex(qtest_readl(qts, UINT64_C(0x20000000000) +
                                0x100 + (11 - 8) * 0x40 + 0x38 + 4) &
                    (1U << 5), ==, 1U << 5);
    g_assert_cmphex(qtest_readl(qts, UINT64_C(0x20000000000) +
                                0x100 + (11 - 8) * 0x40 + 0x10 + 4) &
                    (1U << 31), ==, 1U << 31);
    g_assert_cmphex(qtest_readl(qts, port + 0x800000 +
                                0x100 + 0x38 + 4) & (1U << 5), ==,
                    1U << 5);
    g_assert_cmphex(qtest_readl(qts, port + 0x800000 +
                                0x100 + 0x10 + 4) & (1U << 31), ==,
                    1U << 31);
    /* The Odyssey device's default identity: revision 0xa, part 0xc013. */
    g_assert_cmphex(qtest_readl(qts, port + 4), ==, 0xac013047);
    g_assert_cmphex(qtest_readl(qts, UINT64_C(0x20000000000) + 0x5c), ==,
                    2);
    qtest_writel(qts, port + 0x800000 + 0x5c, 0x1f);
    g_assert_cmphex(qtest_readl(qts, port + 0x800000 + 0x5c), ==, 0x1f);
    g_assert_cmphex(qtest_readl(qts, UINT64_C(0x20000000000) + 0x5c), ==,
                    2);
    g_assert_cmphex(qtest_readl(qts, port + 0x800000 + 4), ==, 0x2d000001);
    qtest_writel(qts, port + 0x24, 0x0f003208);
    g_assert_cmphex(qtest_readl(qts, port + 0x24), ==, 0x0f003208);
    g_assert_cmphex(qtest_readl(qts, port + 0x800000 + 4), ==, 0x2d000001);
    qtest_quit(qts);
    g_unlink(path);
}

/*
 * A two-chassis Origin 300 fits six processors as 4:2 or 2:4, so -smp 6
 * alone is refused and population= names the one wanted; -smp 4 and 8 have
 * one population each and need no name.
 */
static void test_origin300_two_chassis_population(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    g_autofree char *path = NULL;
    g_autofree char *bios = NULL;
    unsigned int counts[2];
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn-builder-o300-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    bios = g_strdup_printf("-bios %s", path);

    {
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M",
            "origin300,topology=origin300-two-chassis", "-cpu", "R14000",
            "-smp", "6", "-m", "1024", "-bios", path, "-display", "none",
            NULL
        };

        assert_qemu_rejects(args, "name one with population=");
    }
    qts = qtest_initf("-M origin300,topology=origin300-two-chassis,"
                      "population=4:2 -cpu R14000 -smp 6 -m 1024 %s "
                      "-display none", bios);
    count_cpus_per_node(qts, counts, 2);
    g_assert_cmpuint(counts[0], ==, 4);
    g_assert_cmpuint(counts[1], ==, 2);
    qtest_quit(qts);
    qts = qtest_initf("-M origin300,topology=origin300-two-chassis,"
                      "population=2:4 -cpu R14000 -smp 6 -m 1024 %s "
                      "-display none", bios);
    count_cpus_per_node(qts, counts, 2);
    g_assert_cmpuint(counts[0], ==, 2);
    g_assert_cmpuint(counts[1], ==, 4);
    qtest_quit(qts);
    qts = qtest_initf("-M origin300,topology=origin300-two-chassis "
                      "-cpu R14000 -smp 8 -m 1024 %s -display none", bios);
    count_cpus_per_node(qts, counts, 2);
    g_assert_cmpuint(counts[0], ==, 4);
    g_assert_cmpuint(counts[1], ==, 4);
    qtest_quit(qts);
    g_unlink(path);
}

static void test_origin2000_rack_accepts_its_populations(const void *data)
{
    QTestState *qts = qtest_init("-M origin2000 -smp 8 -display none");

    qtest_quit(qts);
    qts = qtest_init("-M origin2000 -smp 8 -m 256M -display none");
    qtest_quit(qts);
}

static void test_origin2000_one_node_accepts_its_populations(const void *data)
{
    /*
 * default_ram_size is origin2000's own shipped default (the four-node
 * module's total), so -m must be named explicitly here the same way
 * test_origin200_dual_accepts_its_populations names it for origin200's own
 * two-node topology: left out, it would try this one node against the
 * four-node module's default total and fail, not fall back to a value
 * origin2000-deskside itself accepts.
 */
    QTestState *qts = qtest_init(
        "-M origin2000,topology=origin2000-deskside,nodes=1 -smp 2 -m 64M "
        "-display none");

    qtest_quit(qts);
}

/*
 * origin2000-deskside's own node/CPU-count contract: the topology builds
 * exactly the one node and two CPUs its own supported-cpu-populations entry
 * names. No node1 QOM child exists, and query-cpus-fast reports both
 * sockets on node 0. sn0-origin2000-test.c's assert_cpu_inventory checks the
 * same shape for the four-node module's own eight CPUs; this is that check
 * scaled down to this topology's own population.
 */
static void test_origin2000_one_node_has_two_cpus_on_node_zero(
    const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000,topology=origin2000-deskside,nodes=1 -smp 2 -m 64M "
        "-display none");
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'query-cpus-fast'}");
    QList *cpus = qdict_get_qlist(response, "return");
    const QListEntry *entry;
    g_autofree char *node1 = qom_child_type(qts, "/machine", "node1");
    unsigned int seen = 0;

    g_assert_null(node1);
    g_assert_cmpuint(qlist_size(cpus), ==, 2);
    QLIST_FOREACH_ENTRY(cpus, entry) {
        QDict *cpu = qobject_to(QDict, qlist_entry_obj(entry));
        QDict *props = qdict_get_qdict(cpu, "props");
        unsigned int index = qdict_get_int(cpu, "cpu-index");

        g_assert_cmpuint(index, <, 2);
        g_assert_false(seen & (1U << index));
        seen |= 1U << index;
        g_assert_cmpint(qdict_get_int(props, "node-id"), ==, 0);
        /* Every processor is its own single-core socket. */
        g_assert_cmpint(qdict_get_int(props, "socket-id"), ==, index);
    }
    g_assert_cmphex(seen, ==, 0x3);

    qtest_quit(qts);
}

/*
 * sn-builder-console-test (sn-builder-fault-test.c) is a two-node topology
 * with console=1: node 1's IOC3 port A, not node 0's, must land on serial
 * line 0. Give the machine exactly one backend, at line 0, and read it back
 * off each node's own onboard sgi-baseio's own IOC3 "chardev-a" property
 * (sn0.c creates the Bridge and the sgi-baseio behind it directly, as QOM
 * children of the node object, since neither is a removable board). A
 * chardev property reports the backend's own id once bound
 * (qdev-properties-system.c's get_chr), empty string unbound, so this fails
 * the way the removed boot-and-quit version could not: node 0 answering
 * "probe" instead of node 1 would still exit 0.
 */
static void test_console_field_puts_its_node_on_serial_line_zero(
    const void *data)
{
    QTestState *qts = qtest_init(
        "-M sn-builder-console-test,activate=on -m 128M "
        "-smp 4 -cpu R10000 "
        "-chardev null,id=probe -serial chardev:probe -display none");
    g_autofree char *node0_chardev_a =
        qom_get_str(qts, "/machine/node0/baseio/ioc3", "chardev-a");
    g_autofree char *node1_chardev_a =
        qom_get_str(qts, "/machine/node1/baseio/ioc3", "chardev-a");

    g_assert_cmpstr(node1_chardev_a, ==, "probe");
    g_assert_cmpstr(node0_chardev_a, ==, "");

    qtest_quit(qts);
}

/*
 * tests/unit/test-sn-model.c checks xio_attachments/io_boards against
 * node_count once, for the compiled descriptor; this fixture corrupts a copy
 * at runtime instead, so a node missing its own XIO attachment only
 * fails once sgi_machine_index_xio_attachments looks it up during real
 * construction. This must surface as sgi_machine_ init's own reported error
 * and a clean exit, not an aborting assertion or a memory-region collision
 * reaching the guest as a signal.
 */
static void test_node_without_xio_attachment_rejected(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M",
        "sn-builder-console-test,"
        "construction-fault=node-without-xio-attachment",
        "-m", "128M", "-smp", "4",
        "-cpu", "R10000",
        /*
 * The fault is reported before flash content is ever read, so this only
 * needs to clear the "-bios or pflash" precondition, not name a real image.
 */
        "-bios", "/dev/null", "-display", "none", NULL
    };

    assert_qemu_rejects(args, "node 1 names no direct-attach I/O board or "
                        "XIO attachment");
}

/*
 * The builder dispatches io_boards[] entries on the descriptor's own
 * board_kind alone (sgi_machine_instantiate_io_board), not on how the board
 * attaches: Origin 200's onboard I/O and Origin 2000's BaseIO card both,
 * historically, attached at widget 8 on their own module, and both happen to
 * also differ by attachment kind (direct vs. Crossbow), which is exactly
 * what let a board_kind data defect go unnoticed before. Checking what each
 * carrier actually builds, not merely that something builds, is what a
 * heuristic that happened to agree on both shipped products could not catch:
 * Origin 200's onboard sgi-baseio must carry the "ip29" preset, and Origin
 * 2000's onboard board must be the sgi-xio-baseio card class, not merely
 * something at widget 8.
 */
static void test_origin200_onboard_board_is_ip29_preset(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -display none");
    g_autofree char *preset =
        qom_get_str(qts, "/machine/node0/baseio", "preset");

    g_assert_cmpstr(preset, ==, "ip29");
    qtest_quit(qts);
}

/*
 * The default four-node module keeps the plain BaseIO: only the one-node
 * topology below declares BaseIO-G, so without baseio-preset= the default
 * must still build io6.
 */
static void test_origin2000_onboard_board_is_xio_baseio(const void *data)
{
    QTestState *qts = qtest_init("-M origin2000 -display none");
    g_autofree char *type = qom_child_type(qts, "/machine/module0", "io1");
    g_autofree char *preset = NULL;

    g_assert_cmpstr(type, ==, "sgi-xio-baseio");
    preset = qom_get_str(qts, "/machine/module0/io1", "preset");
    g_assert_cmpstr(preset, ==, "io6");
    qtest_quit(qts);
}

/*
 * Every XIO slot a deskside's Crossbows reach takes a board. The one-node
 * deskside's lone node controls the whole first bank, IO1 to IO6
 * (007-3453-002 Table 2-2), including IO4, which the four-node module
 * gives to node 3. On the two-node deskside node 2 sits on Crossbow 1, as
 * in the four-node module, and brings IO7 to IO12 with it, IO8 included,
 * a node-4 position there; IO4 stays with node 1, as the two-node Origin
 * 2200 inventory model-toad shows an MSCSI working in it.
 */
static void test_origin2000_deskside_slots(const void *data)
{
    static const struct {
        const char *args;
        const char *slot;
        const char *crossbow;
    } cases[] = {
        { "-M origin2000,topology=origin2000-deskside,nodes=1 -smp 2 -m 64M "
          "-display none -device sgi-xio-baseio,preset=mscsi,slot=io4",
          "io4", "crossbow0" },
        { "-M origin2000,topology=origin2000-deskside,nodes=2 -smp 4 "
          "-m 128M -display none "
          "-device sgi-xio-baseio,preset=mscsi,slot=io4",
          "io4", "crossbow0" },
        { "-M origin2000,topology=origin2000-deskside,nodes=2 -smp 4 "
          "-m 128M -display none "
          "-device sgi-xio-baseio,preset=menet,slot=io8",
          "io8", "crossbow1" },
    };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        QTestState *qts = qtest_init(cases[i].args);
        g_autofree char *type = qom_child_type(qts, "/machine/module0",
                                               cases[i].slot);
        g_autofree char *crossbow = qom_child_type(qts, "/machine",
                                                   cases[i].crossbow);

        g_assert_cmpstr(type, ==, "sgi-xio-baseio");
        g_assert_nonnull(crossbow);
        qtest_quit(qts);
    }
}

/*
 * The stock deskside fits a plain BaseIO in IO1 and nothing in IO3; the
 * machine's baseio-preset=io6g asks for the BaseIO-G instead, whose
 * MediaIO daughtercard fits the second IOC3 with the keyboard and mouse.
 */
static void test_origin2000_deskside_baseio_follows_the_preset(
    const void *data)
{
    static const struct {
        const char *args;
        const char *preset;
    } cases[] = {
        { "-M origin2000,topology=origin2000-deskside,nodes=1 -smp 2 -m 64M "
          "-display none", "io6" },
        { "-M origin2000,topology=origin2000-deskside,nodes=1,"
          "baseio-preset=io6g -smp 2 -m 64M -display none", "io6g" },
    };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        QTestState *qts = qtest_init(cases[i].args);
        g_autofree char *type = qom_child_type(qts, "/machine/module0",
                                               "io1");
        g_autofree char *preset = NULL;
        g_autofree char *head = qom_child_type(qts, "/machine/module0",
                                               "io3");

        g_assert_cmpstr(type, ==, "sgi-xio-baseio");
        preset = qom_get_str(qts, "/machine/module0/io1", "preset");
        g_assert_cmpstr(preset, ==, cases[i].preset);
        g_assert_null(head);
        qtest_quit(qts);
    }
}

/*
 * MGRAS is an ordinary XIO board: no shipped topology fits one, and a
 * board fitted at launch at an empty slot answers there. The GIGAchannel
 * cabinet is a second product whose slots a board reaches the same way.
 */
#define MGRAS_WIDGET 15
#define IO_SPACE UINT64_C(0x20000000000)
#define XIO_WIDGET_SIZE UINT64_C(0x1000000)
#define WIDGET_ID_DATA 4
#define MGRAS_WIDGET_ID ((0xc003u << 12) | (0x2aau << 1) | 1)

/*
 * An MGRAS is an XIO board named by its slot, under its module: io3 on an
 * Origin 2000, io6 in a GIGAchannel cabinet.
 */
static void assert_mgras_at_widget(QTestState *qts, const char *module,
                                   const char *slot, unsigned int widget)
{
    g_autofree char *head = qom_child_type(qts, module, slot);
    uint64_t bit = 1u << widget;
    uint8_t bytes[4];

    g_assert_cmpstr(head, ==, "sgi-mgras");
    /* A board fitted at launch is attached live, not at startup. */
    g_assert_cmphex(crossbow_uint(qts, "crossbow0",
                                  "startup-links-alive") & bit, ==, 0);
    /* The widget identity register, read through CPU 0's Hub. */
    g_assert_true(qtest_memread_cpu(qts, 0, IO_SPACE +
                                    widget * XIO_WIDGET_SIZE +
                                    WIDGET_ID_DATA, bytes, sizeof(bytes)));
    g_assert_cmphex(ldl_be_p(bytes), ==, MGRAS_WIDGET_ID);
}

static void test_mgras_fitted_in_io3(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none -device sgi-mgras,slot=io3");

    assert_mgras_at_widget(qts, "/machine/module0", "io3", MGRAS_WIDGET);
    qtest_quit(qts);
}

/*
 * An SI board fitted at launch at an empty slot, as a -device: it lands
 * under its module by slot name and answers its widget identity through
 * the Crossbow, IO5 being crossbow0's widget 14.
 */
static void test_mgras_fitted_at_a_named_slot(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none -device sgi-mgras,slot=io5");
    g_autofree char *head = qom_child_type(qts, "/machine/module0", "io5");
    uint8_t bytes[4];

    g_assert_cmpstr(head, ==, "sgi-mgras");
    g_assert_true(qtest_memread_cpu(qts, 0, IO_SPACE +
                                    14 * XIO_WIDGET_SIZE + WIDGET_ID_DATA,
                                    bytes, sizeof(bytes)));
    g_assert_cmphex(ldl_be_p(bytes), ==, MGRAS_WIDGET_ID);
    qtest_quit(qts);
}

/*
 * Two boards fitted at launch never share a one-wire identity: each
 * part's ROM serial is unique, and a board left at its class's shared
 * placeholder takes one offset by its slot.
 */
static void test_fitted_boards_have_distinct_serials(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none "
        "-device sgi-xio-baseio,preset=menet,slot=io4 "
        "-device sgi-xio-baseio,preset=menet,slot=io5");
    uint64_t io4_board, io5_board, io4_ioc3, io5_ioc3;

    io4_board = qom_get_uint(qts, "/machine/module0/io4", "onewire-serial");
    io5_board = qom_get_uint(qts, "/machine/module0/io5", "onewire-serial");
    io4_ioc3 = qom_get_uint(qts, "/machine/module0/io4",
                            "ioc3-onewire-serial");
    io5_ioc3 = qom_get_uint(qts, "/machine/module0/io5",
                            "ioc3-onewire-serial");
    g_assert_cmphex(io4_board, !=, io5_board);
    g_assert_cmphex(io4_ioc3, !=, io5_ioc3);
    qtest_quit(qts);
}

/*
 * Whether any object anywhere in the machine's QOM tree is of type
 * @type: info qom-tree lists every object under /machine, one per line
 * as "/name (type)", at any depth, the root's own children included. The
 * closing parenthesis keeps a longer type name from matching.
 */
static bool machine_has_object_of_type(QTestState *qts, const char *type)
{
    g_autofree char *tree = qtest_hmp(qts, "info qom-tree");
    g_autofree char *wanted = g_strdup_printf(" (%s)", type);

    g_assert_nonnull(strstr(tree, "/machine ("));
    return strstr(tree, wanted) != NULL;
}

/*
 * A stock configuration fits no MGRAS anywhere: not in a module's slot,
 * and not at /machine/mgras, where the builder used to put one.
 */
static void test_no_mgras_without_a_catalogue_record(const void *data)
{
    static const char *const machines[] = {
        "-M origin2000 -display none",
        "-M origin2000,topology=origin2000-deskside,nodes=1 -smp 2 -m 64M "
        "-display none",
        "-M onyx2 -display none",
        "-M onyx2,topology=onyx2-deskside -smp 4 -m 128M -display none",
        "-M origin200,topology=origin200-gigachannel -display none",
    };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(machines); i++) {
        QTestState *qts = qtest_init(machines[i]);

        g_assert_false(machine_has_object_of_type(qts, "sgi-mgras"));
        qtest_quit(qts);
    }
}

static void test_mgras_in_a_gigachannel_cabinet(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200,topology=origin200-gigachannel -display none "
        "-device sgi-mgras,slot=io6");

    assert_mgras_at_widget(qts, "/machine/module1", "io6",
                           MGRAS_WIDGET);
    /* The search the stock configurations pass finds a fitted one. */
    g_assert_true(machine_has_object_of_type(qts, "sgi-mgras"));
    qtest_quit(qts);
}

/*
 * Odyssey is an ordinary XIO board too. Named-slot placement resolves
 * "fabric" itself inside the parent realize (sgi_xio_board_place_by_name()),
 * so an Odyssey realize that checked "fabric" before calling its parent
 * could never be placed this way; io5 is the same widget 14 MGRAS answers
 * at above, a topology fact that does not depend on which board claims it.
 */
#define ODSY_WIDGET_ID 0xac013047

static void test_odyssey_fitted_at_a_named_slot(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000 -display none -device sgi-odyssey,slot=io5");
    g_autofree char *head = qom_child_type(qts, "/machine/module0", "io5");
    uint8_t bytes[4];

    g_assert_cmpstr(head, ==, "sgi-odyssey");
    g_assert_true(qtest_memread_cpu(qts, 0, IO_SPACE +
                                    14 * XIO_WIDGET_SIZE + WIDGET_ID_DATA,
                                    bytes, sizeof(bytes)));
    g_assert_cmphex((uint32_t)ldl_be_p(bytes), ==, ODSY_WIDGET_ID);
    qtest_quit(qts);
}

/*
 * sgi-odyssey realizes onto the plain system bus like every sgi-xio-board
 * (sgi_xio_board_reparent(), hw/mips/sgi/xio-board.c), which has no hotplug
 * handler; tests/qtest/sn0-origin2000-kona-test.c's own
 * test_onewire_other_identity_parts_empty() documents that QMP device_add
 * cannot substitute for a command-line "-device" here, for exactly that
 * reason. A board whose realize fails takes the whole process down as a
 * fatal command-line error (sgi-board-test.c's test_slot_taken_twice() is
 * the same pattern for a plain test board), so there is no way to keep the
 * process alive afterward to show a later memory transaction is unaffected.
 * This test is therefore narrower than the dangling-AddressSpace failure
 * mode itself: it pins that the specific guest-reachable failure (an
 * out-of-range "nic-serial") still fails with its own documented message
 * and plain exit, rather than the wrong signal a broken unwind -- say, a
 * bad address_space_destroy() call -- would produce instead.
 *
 * Placed by name, like test_odyssey_fitted_at_a_named_slot() above: io5's
 * slot link hands the board its "fabric" during oc->parent_realize(), so
 * this needs no standalone MemoryRegion of its own (and, unlike the
 * module+slot form, must not set "fabric" itself -- the board rejects a
 * pre-supplied link once it resolves one from the slot). "nic-serial" is
 * read after "fabric" already has its address space (address_space_init()
 * runs unconditionally once the parent board has claimed the slot), so this
 * exercises the exact ordering finding 2 describes: odyssey_attach_nic()
 * rejects the value, and the fix this test accompanies destroys that
 * address space before returning instead of leaving it registered.
 */
static void test_odyssey_bad_nic_serial_does_not_leak_fabric(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*the sgi-odyssey one-wire serial*does not fit in 48 bits*");
        return;
    }
    s = qtest_init(
        "-M origin2000 -display none -device sgi-odyssey,slot=io5,"
        "nic-serial=0x1000000000000 -S");
    qtest_quit(s);
}

/*
 * The builder builds a Bedrock topology of more than one node. This drives
 * it over sn-builder-fault-test.c's own synthetic two-node fixture: two
 * Bedrock nodes, each with its own populated IO-8 board at its own
 * attachment, no controllers and no cable. Population and memory validation,
 * the node loop's per-node board and widget selection, and each node's
 * realize all run for two nodes, and the machine comes up with both nodes as
 * devices. With no cable each node reaches only itself; sn1-cross-node-test
 * covers the cabled pair.
 */
static void test_bedrock_topology_accepts_two_nodes(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(0x100000);
    g_autofree char *path = NULL;
    g_autofree char *node1 = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn-builder-bedrock-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, 0x100000), ==, 0x100000);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf("-M sn-builder-bedrock-test,activate=on -m 128M "
                      "-smp 2,sockets=2,cores=1,threads=1 -bios %s "
                      "-display none", path);
    node1 = qom_child_type(qts, "/machine", "node1");
    g_assert_cmpstr(node1, ==, "sgi-sn1-node");
    qtest_quit(qts);
    g_unlink(path);
}

static bool nasid_test_cpu_readq(QTestState *qts, unsigned int cpu,
                                 uint64_t address, uint64_t *value);
static bool nasid_test_cpu_readl(QTestState *qts, unsigned int cpu,
                                 uint64_t address, uint32_t *value);

#define IP53_IO_SPACE UINT64_C(0x20000000000)
#define IP53_BOARD_ID_LOCAL UINT64_C(0x10014000000)
#define IP53_BOARD_ID_REMOTE UINT64_C(0x10034000000)
#define IP53_TEST_PROM_SIZE 1476264
#define IP53_BTE0 (IP53_IO_SPACE + UINT64_C(0x1000000) + UINT64_C(0x410000))
#define IP53_BTE_LENGTH_BUSY (UINT64_C(1) << 20)
#define IP53_BTE_NOTIFY_ENABLE (UINT64_C(1) << 4)
#define IP53_BTE_SOURCE UINT64_C(0x1eca8000)
#define IP53_BTE_DESTINATION UINT64_C(0x1fd10000)
#define IP53_BTE_NOTIFICATION UINT64_C(0x30000)
#define IP53_RESET_PROM UINT64_C(0x1fc00000)
#define IP53_PIC_HEAD0 (IP53_IO_SPACE + UINT64_C(9) * UINT64_C(0x1000000))
#define IP53_PI_PENDING (IP53_IO_SPACE + UINT64_C(0x1800098))
#define IP53_PI_LEVEL 0x34
#define IP53_PI_PENDING_BIT (UINT64_C(1) << IP53_PI_LEVEL)
#define IP53_PI_INTERRUPT_ADDRESS UINT64_C(0x800001800090)

#define IP53_BCM_RUNTIME_PIC_HEAD0 \
    (IP53_IO_SPACE + UINT64_C(15) * UINT64_C(0x1000000))
#define IP53_BCM_CFG (IP53_BCM_RUNTIME_PIC_HEAD0 + UINT64_C(0x24000))
#define IP53_BCM_BAR (IP53_IO_SPACE + UINT64_C(0x20000000) + \
                      UINT64_C(0x01000000))
#define IP53_BCM_DMA_BASE UINT64_C(0x80000000)
#define IP53_BCM_TX_RING_RAM UINT64_C(0x02200000)
#define IP53_BCM_TX_BUFFER_RAM UINT64_C(0x02210000)
#define IP53_BCM_STATUS_RAM UINT64_C(0x02220000)
#define IP53_BCM_RX_RING_RAM UINT64_C(0x02230000)
#define IP53_BCM_RX_RETURN_RAM UINT64_C(0x02240000)
#define IP53_BCM_RX_BUFFER_RAM UINT64_C(0x02250000)
#define IP53_BCM_TX_RING (IP53_BCM_DMA_BASE + IP53_BCM_TX_RING_RAM)
#define IP53_BCM_TX_BUFFER (IP53_BCM_DMA_BASE + IP53_BCM_TX_BUFFER_RAM)
#define IP53_BCM_STATUS (IP53_BCM_DMA_BASE + IP53_BCM_STATUS_RAM)
#define IP53_BCM_RX_RING (IP53_BCM_DMA_BASE + IP53_BCM_RX_RING_RAM)
#define IP53_BCM_RX_RETURN (IP53_BCM_DMA_BASE + IP53_BCM_RX_RETURN_RAM)
#define IP53_BCM_RX_BUFFER (IP53_BCM_DMA_BASE + IP53_BCM_RX_BUFFER_RAM)
#define IP53_BCM_FRAME_SIZE 60

static void ip53_bcm_put32le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = value;
    buffer[1] = value >> 8;
    buffer[2] = value >> 16;
    buffer[3] = value >> 24;
}

static void ip53_bcm_put16le(uint8_t *buffer, unsigned int offset,
                             uint16_t value)
{
    buffer[offset] = value;
    buffer[offset + 1] = value >> 8;
}

static uint32_t ip53_bcm_get32le(const uint8_t *buffer)
{
    return ((uint32_t)buffer[3] << 24) | ((uint32_t)buffer[2] << 16) |
           ((uint32_t)buffer[1] << 8) | buffer[0];
}

static void ip53_bcm_sram_write(QTestState *qts, uint32_t offset,
                                uint32_t value)
{
    qtest_writel(qts, IP53_BCM_CFG + 0x7c, offset);
    qtest_writel(qts, IP53_BCM_CFG + 0x84, value);
}

static bool ip53_bcm_sram_read_cpu(QTestState *qts, uint32_t offset,
                                   uint32_t *value)
{
    qtest_writel(qts, IP53_BCM_CFG + 0x7c, offset);
    return nasid_test_cpu_readl(qts, 0, IP53_BCM_CFG + 0x84, value);
}

static uint16_t ip53_bcm_get16le(const uint8_t *buffer)
{
    return ((uint16_t)buffer[1] << 8) | buffer[0];
}

static void ip53_bcm_send_frame(int fd, const uint8_t *frame, size_t length)
{
    uint32_t header = htonl(length);

    g_assert_cmpint(send(fd, &header, sizeof(header), 0), ==, sizeof(header));
    g_assert_cmpint(send(fd, frame, length, 0), ==, length);
}

static size_t ip53_bcm_receive_frame(int fd, uint8_t *frame, size_t capacity)
{
    uint32_t header;
    uint32_t length;

    g_assert_cmpint(recv(fd, &header, sizeof(header), MSG_WAITALL), ==,
                    sizeof(header));
    length = ntohl(header);
    g_assert_cmpuint(length, <=, capacity);
    g_assert_cmpint(recv(fd, frame, length, MSG_WAITALL), ==, length);
    return length;
}

static void ip53_bcm_assert_static_device(QTestState *qts)
{
    g_autofree char *type = qom_child_type(
        qts, "/machine", "io9-io9-d4-f0");
    g_autofree char *model = qom_get_str(
        qts, "/machine/io9-io9-d4-f0", "model");
    unsigned int bcm5701_count = 0;
    unsigned int device;
    uint32_t identity;

    g_assert_cmpstr(type, ==, "bcm570x");
    g_assert_cmpstr(model, ==, "bcm5701");
    g_assert_true(nasid_test_cpu_readl(qts, 0, IP53_BCM_CFG, &identity));
    g_assert_cmphex(identity, ==, 0x164514e4);
    for (device = 0; device < 32; device++) {
        uint64_t config = IP53_BCM_RUNTIME_PIC_HEAD0 + 0x20000 +
                          device * 0x1000;

        g_assert_true(nasid_test_cpu_readl(qts, 0, config, &identity));
        if ((identity & 0xffff) == 0x14e4 &&
            (identity >> 16) == 0x1645) {
            bcm5701_count++;
            g_assert_cmpuint(device, ==, 4);
        }
    }
    g_assert_cmpuint(bcm5701_count, ==, 1);
}

static void ip53_set_interrupt(QTestState *qts, unsigned head, bool asserted)
{
    g_autofree char *path = g_strdup_printf(
        "/machine/pic-fixture-pic/head-%u", head);

    qtest_set_irq_in(qts, path, NULL, 0, asserted);
}

static void ip53_route_interrupt_to_input(QTestState *qts,
                                          unsigned head, unsigned input)
{
    uint64_t pic_head = IP53_PIC_HEAD0 + head * UINT64_C(0x800000);
    uint64_t destination = ((uint64_t)IP53_PI_LEVEL << 48) |
                          IP53_PI_INTERRUPT_ADDRESS;
    uint64_t input_bit = UINT64_C(1) << input;

    g_assert_cmpuint(input, <, 8);
    /* Widget 9 is the synthetic PIC source qualifier. */
    qtest_writeq(qts, pic_head + 0x20, 9);
    qtest_writeq(qts, pic_head + 0x130 + input * sizeof(uint64_t),
                 destination);
    qtest_writeq(qts, pic_head + 0x108, input_bit);
    qtest_writeq(qts, pic_head + 0x118, input_bit);
}

static void ip53_route_interrupt(QTestState *qts, unsigned head)
{
    ip53_route_interrupt_to_input(qts, head, 0);
}

static void test_ip53_builds_linked_pic_and_io9(const void *data)
{
    const char *fault = data;
    unsigned int head = strstr(fault, "-h1") ? 1 : 0;
    unsigned int device = strstr(fault, "-d4-") ? 4 : 1;
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(0x100000);
    g_autofree char *path = NULL;
    g_autofree char *bedrock = NULL;
    g_autofree char *pic = NULL;
    g_autofree char *ioc4 = NULL;
    g_autofree char *isp12160 = NULL;
    g_autofree char *bcm5701 = NULL;
    g_autofree char *rtc = NULL;
    g_autofree char *data_path = NULL;
    g_autofree char *clock_path = NULL;
    g_autofree char *construction = NULL;
    g_autofree char *child_name = NULL;
    const uint64_t io_space = UINT64_C(0x20000000000);
    const uint64_t io_registers = io_space + UINT64_C(0x1000000) +
                                  UINT64_C(0x400000);
    const uint64_t pic_widget = io_space + 9 * UINT64_C(0x1000000);
    const uint64_t dma_ram = UINT64_C(0x80000000);
    QTestState *qts;
    g_autofree char *data_image = NULL;
    gsize data_image_size;
    uint64_t value = 0;
    uint32_t config = 0;
    uint64_t pic_head;
    uint64_t ioc4_memory;
    uint64_t ioc4_config;
    uint64_t itte;
    uint8_t tx_entry[8] = { 0x52, 0, 0, 0, 0x40, 0, 0, 0 };
    uint8_t tx_byte = 0;
    struct pollfd serial_poll = { .events = POLLIN };
    int serial_fds[2];
    int fd, data_fd, clock_fd;

    fd = g_file_open_tmp("sn-builder-ip53-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, 0x100000), ==, 0x100000);
    g_assert_cmpint(close(fd), ==, 0);
    data_fd = g_file_open_tmp("sn-builder-ip53-data-XXXXXX", &data_path,
                              &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(data_fd, 0x800), ==, 0);
    g_assert_cmpint(close(data_fd), ==, 0);
    clock_fd = g_file_open_tmp("sn-builder-ip53-clock-XXXXXX", &clock_path,
                               &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(clock_fd, 16), ==, 0);
    g_assert_cmpint(close(clock_fd), ==, 0);
    construction = g_strdup_printf("construction-fault=%s", fault);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, serial_fds), ==, 0);
    qts = qtest_initf("-M sn-builder-bedrock-test,%s -m 64M -smp 1 "
                      "-chardev socket,id=ip53-serial,fd=%d "
                      "-serial null -serial chardev:ip53-serial "
                      "-cpu R14000 -d guest_errors,unimp -bios %s "
                      "-drive if=none,id=sgi-ds1742-data-fixture-io9,"
                      "file=%s,format=raw "
                      "-drive if=none,id=sgi-ds1742-clock-fixture-io9,"
                      "file=%s,format=raw -display none -S", construction,
                      serial_fds[1], path, data_path, clock_path);
    close(serial_fds[1]);
    child_name = g_strdup_printf("io9-fixture-io9-d%u-f0", device);
    bedrock = qom_child_type(qts, "/machine/node0", "bedrock");
    pic = qom_child_type(qts, "/machine", "pic-fixture-pic");
    ioc4 = qom_child_type(qts, "/machine", child_name);
    isp12160 = qom_child_type(qts, "/machine",
                              "io9-fixture-io9-d2-f0");
    bcm5701 = qom_child_type(qts, "/machine",
                             "io9-fixture-io9-d3-f0");
    g_assert_cmpstr(bedrock, ==, "sgi-bedrock");
    g_assert_cmpstr(pic, ==, "sgi-pic");
    g_assert_cmpstr(ioc4, ==, "sgi-ioc4");
    g_assert_cmpstr(isp12160, ==, "qlogic-isp12160");
    g_assert_cmpstr(bcm5701, ==, "bcm570x");
    rtc = qom_child_type(qts, "/machine", "ds1742-fixture-io9");
    g_assert_cmpstr(rtc, ==, "ds1742");
    g_assert_null(qom_child_type(qts, "/machine/node0", "xbridge"));
    g_assert_true(nasid_test_cpu_readq(qts, 0, pic_widget, &value));
    g_assert_cmphex((value >> 28) & 0xf, ==, 3);
    g_assert_cmphex((value >> 12) & 0xffff, ==, 0xd102);
    g_assert_cmphex((value & 0xffe) >> 1, ==, 0x24);
    g_assert_true(nasid_test_cpu_readq(qts, 0, pic_widget + 0x800000,
                                       &value));
    g_assert_cmphex((value >> 28) & 0xf, ==, 3);
    g_assert_cmphex((value >> 12) & 0xffff, ==, 0xd112);
    g_assert_cmphex((value & 0xffe) >> 1, ==, 0x24);
    ioc4_config = pic_widget + head * 0x800000 + 0x20000 +
                  device * 0x1000;
    pic_head = pic_widget + head * 0x800000;
    qtest_writeq(qts, pic_head + 0x20, 9);
    g_assert_cmphex(qtest_readq(qts, pic_head + 0x20), ==, 9);
    qtest_writeq(qts, pic_head + 0x80, (UINT64_C(15) << 20) | 1);
    g_assert_cmphex(qtest_readq(qts, pic_head + 0x80), ==,
                    (UINT64_C(15) << 20) | 1);
    itte = (UINT64_C(9) << 8) | (head ? 6 : 2);
    qtest_writeq(qts, io_registers + 0x160, itte);
    g_assert_true(nasid_test_cpu_readq(qts, 0, io_registers + 0x160,
                                       &value));
    g_assert_cmphex(value, ==, itte);
    ioc4_memory = io_space + 0x20000000 + 0x01000000;
    g_assert_true(nasid_test_cpu_readl(qts, 0, ioc4_config, &config));
    g_assert_cmphex(config, ==, 0x100a10a9);

    qtest_writel(qts, ioc4_config + 4,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, ioc4_config + 0x10, 0x01000000);
    qtest_writel(qts, ioc4_memory + 0x10, bswap32(0x40));
    g_assert_cmphex(bswap32(qtest_readl(qts, ioc4_memory + 0x10)), ==,
                    0x40);
    qtest_writeb(qts, ioc4_memory + 0x87ff8, 0x80);
    qtest_writeb(qts, ioc4_memory + 0x87ff9, 0x92);
    qtest_writeb(qts, ioc4_memory + 0x87ffa, 0x23);
    qtest_writeb(qts, ioc4_memory + 0x87ffb, 0x10);
    qtest_writeb(qts, ioc4_memory + 0x87ffc, 0x02);
    qtest_writeb(qts, ioc4_memory + 0x87ffd, 0x25);
    qtest_writeb(qts, ioc4_memory + 0x87ffe, 0x09);
    qtest_writeb(qts, ioc4_memory + 0x87fff, 0x26);
    qtest_writeb(qts, ioc4_memory + 0x87ff8, 0x20);
    g_assert_cmphex(qtest_readb(qts, ioc4_memory + 0x87ff9), ==, 0x92);
    qtest_writeb(qts, ioc4_memory + 0x380 + 3, 3);
    qtest_writeb(qts, ioc4_memory + 0x380 + 2, 1);
    qtest_writeb(qts, ioc4_memory + 0x380 + 4, 3);
    qtest_writeb(qts, ioc4_memory + 0x380 + 1, 2);
    qtest_writeb(qts, ioc4_memory + 0x380, 0x40);
    serial_poll.fd = serial_fds[0];
    g_assert_cmpint(poll(&serial_poll, 1, 1000), ==, 1);
    g_assert_cmpint(read(serial_fds[0], &tx_byte, 1), ==, 1);
    g_assert_cmphex(tx_byte, ==, 0x40);
    qtest_writeq(qts, UINT64_C(0x20001000000) + 0x780008,
                 0x1100);
    qtest_memwrite(qts, dma_ram, tx_entry, sizeof(tx_entry));
    {
        uint8_t observed[sizeof(tx_entry)];

        qtest_memread(qts, dma_ram, observed, sizeof(observed));
        g_assert_cmpmem(observed, sizeof(observed), tx_entry,
                        sizeof(tx_entry));
    }
    qtest_writel(qts, ioc4_memory + 0x304, 0);
    qtest_writel(qts, ioc4_memory + 0x300, bswap32(0x80000000));
    qtest_writel(qts, ioc4_memory + 0x310, bswap32(1U << 28));
    qtest_writel(qts, ioc4_memory + 0x314, bswap32(8));
    {
        int ready = poll(&serial_poll, 1, 1000);

        if (ready != 1) {
            g_test_message("IOC4 DMA TX stalled: base=%08x:%08x control=%08x "
                           "produce=%08x consume=%08x",
                           bswap32(qtest_readl(qts, ioc4_memory + 0x304)),
                           bswap32(qtest_readl(qts, ioc4_memory + 0x300)),
                           bswap32(qtest_readl(qts, ioc4_memory + 0x310)),
                           bswap32(qtest_readl(qts, ioc4_memory + 0x314)),
                           bswap32(qtest_readl(qts, ioc4_memory + 0x318)));
        }
        g_assert_cmpint(ready, ==, 1);
    }
    g_assert_cmpint(read(serial_fds[0], &tx_byte, 1), ==, 1);
    g_assert_cmphex(tx_byte, ==, 0x52);
    g_assert_true(nasid_test_cpu_readq(qts, 0, pic_head + 0x100, &value));
    g_assert_cmphex(value & (1U << 3), !=, 0);
    qtest_writeq(qts, pic_head + 0x110, 1U << 3);
    g_assert_true(nasid_test_cpu_readq(qts, 0, pic_head + 0x100, &value));
    g_assert_cmphex(value & (1U << 3), !=, 0);
    ip53_route_interrupt(qts, head);
    /* Match the public PIC helper's direct-map target widget. */
    qtest_writeq(qts, pic_head + 0x80, (UINT64_C(15) << 20) | 1);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    IP53_PI_PENDING_BIT, ==, 0);
    ip53_set_interrupt(qts, head, true);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    IP53_PI_PENDING_BIT, ==, IP53_PI_PENDING_BIT);
    ip53_set_interrupt(qts, head, false);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    IP53_PI_PENDING_BIT, ==, 0);
    qtest_writeb(qts, ioc4_memory + 0x380 + 1, 0);
    qtest_writel(qts, ioc4_memory + 0x08, bswap32(0x40));
    g_assert_true(nasid_test_cpu_readq(qts, 0, pic_head + 0x100, &value));
    g_assert_cmphex(value & (1U << 3), !=, 0);
    qtest_writeq(qts, pic_head + 0x110, 1U << 3);
    g_assert_true(nasid_test_cpu_readq(qts, 0, pic_head + 0x100, &value));
    g_assert_cmphex(value & (1U << 3), ==, 0);
    qtest_writeb(qts, ioc4_memory + 0x380 + 3, 3);
    qtest_writeb(qts, ioc4_memory + 0x380 + 2, 1);
    qtest_writeb(qts, ioc4_memory + 0x380 + 4, 3);
    ip53_route_interrupt_to_input(qts, head, 3);
    qtest_writeb(qts, ioc4_memory + 0x380 + 1, 2);
    g_assert_true(nasid_test_cpu_readq(qts, 0, pic_head + 0x100, &value));
    g_assert_cmphex(value & (1U << 3), !=, 0);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    IP53_PI_PENDING_BIT, ==, IP53_PI_PENDING_BIT);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_true(nasid_test_cpu_readq(qts, 0, pic_head + 0x100, &value));
    g_assert_cmphex(value & (1U << 3), ==, 0);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    IP53_PI_PENDING_BIT, ==, 0);
    qtest_quit(qts);
    close(serial_fds[0]);
    g_assert_true(g_file_get_contents(data_path, &data_image,
                                      &data_image_size, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(data_image_size, ==, 0x800);
    g_assert_cmphex((uint8_t)data_image[0x7f8], ==, 0x20);
    g_assert_cmphex((uint8_t)data_image[0x7f9], ==, 0x92);
    g_assert_cmphex((uint8_t)data_image[0x7fa], ==, 0x23);
    g_assert_cmphex((uint8_t)data_image[0x7fb], ==, 0x10);
    qts = qtest_initf("-M sn-builder-bedrock-test,%s -m 64M -smp 1 "
                      "-cpu R14000 -bios %s "
                      "-drive if=none,id=sgi-ds1742-data-fixture-io9,"
                      "file=%s,format=raw "
                      "-drive if=none,id=sgi-ds1742-clock-fixture-io9,"
                      "file=%s,format=raw -display none -S", construction,
                      path, data_path, clock_path);
    rtc = qom_child_type(qts, "/machine", "ds1742-fixture-io9");
    g_assert_cmpstr(rtc, ==, "ds1742");
    qtest_writeq(qts, pic_head + 0x80, (UINT64_C(15) << 20) | 1);
    g_assert_cmphex(qtest_readq(qts, pic_head + 0x80), ==,
                    (UINT64_C(15) << 20) | 1);
    qtest_writeq(qts, io_registers + 0x160, itte);
    g_assert_true(nasid_test_cpu_readq(qts, 0, io_registers + 0x160,
                                       &value));
    g_assert_cmphex(value, ==, itte);
    g_assert_true(nasid_test_cpu_readl(qts, 0, ioc4_config, &config));
    qtest_writel(qts, ioc4_config + 4,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, ioc4_config + 0x10, 0x01000000);
    g_assert_cmphex(qtest_readb(qts, ioc4_memory + 0x87ff9), ==, 0x92);
    qtest_quit(qts);
    g_unlink(path);
    g_unlink(data_path);
    g_unlink(clock_path);
}

static void test_ip53_fingerprint_migration(const void *opaque)
{
    const char *destination_fault = opaque;
    bool mismatch = strcmp(destination_fault, "ip53-pic-io9") != 0;
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn-builder-ip53-migrate-XXXXXX",
                                           &error);
    g_autofree char *prom = g_build_filename(dir, "prom.bin", NULL);
    g_autofree char *from_data = g_build_filename(dir, "from-data.bin", NULL);
    g_autofree char *from_clock = g_build_filename(dir, "from-clock.bin", NULL);
    g_autofree char *to_data = g_build_filename(dir, "to-data.bin", NULL);
    g_autofree char *to_clock = g_build_filename(dir, "to-clock.bin", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *blank_prom = g_malloc0(0x100000);
    g_autofree char *blank_data = g_malloc0(0x800);
    g_autofree char *blank_clock = g_malloc0(16);
    QTestState *from, *to;
    uint64_t io_registers = IP53_IO_SPACE + UINT64_C(0x1400000);
    uint64_t ioc4_config = IP53_PIC_HEAD0 + UINT64_C(0x21000);
    uint64_t ioc4_memory = IP53_IO_SPACE + UINT64_C(0x21000000);
    uint64_t itte = (UINT64_C(9) << 8) | 2;
    uint64_t value;
    g_autofree char *from_data_image = NULL;
    g_autofree char *to_data_image = NULL;
    gsize from_data_size, to_data_size;

    if (mismatch && !g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 30 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr("*IP53 assembly fingerprint mismatch*");
        return;
    }

    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(prom, blank_prom, 0x100000, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(from_data, blank_data, 0x800, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(to_data, blank_data, 0x800, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(from_clock, blank_clock, 16, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(to_clock, blank_clock, 16, &error));
    g_assert_no_error(error);
    from = qtest_initf(
        "-M sn-builder-bedrock-test,construction-fault=ip53-pic-io9 "
        "-m 64M -smp 1 -cpu R14000 -bios %s "
        "-drive if=none,id=sgi-ds1742-data-fixture-io9,file=%s,format=raw "
        "-drive if=none,id=sgi-ds1742-clock-fixture-io9,file=%s,format=raw "
        "-nodefaults -display none -S", prom, from_data, from_clock);
    to = qtest_initf(
        "-M sn-builder-bedrock-test,construction-fault=%s "
        "-m 64M -smp 1 -cpu R14000 -bios %s "
        "-drive if=none,id=sgi-ds1742-data-fixture-io9,file=%s,format=raw "
        "-drive if=none,id=sgi-ds1742-clock-fixture-io9,file=%s,format=raw "
        "-nodefaults -display none -S -incoming defer", destination_fault,
        prom, to_data, to_clock);
    if (g_str_equal(destination_fault, "ip53-alt-clock")) {
        g_printerr("IP53 fingerprint descriptor clocks: source="
                   "200000000 Hz destination=150000000 Hz\n");
    }
    if (mismatch) {
        qtest_set_expected_status(to, EXIT_FAILURE);
    }
    if (!mismatch) {
        qtest_writeq(from, io_registers + 0x160, itte);
        g_assert_true(nasid_test_cpu_readq(from, 0, io_registers + 0x160,
                                           &value));
        g_assert_cmphex(value, ==, itte);
        ip53_route_interrupt_to_input(from, 0, 3);
        qtest_writel(from, ioc4_config + 4,
                     PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
        qtest_writel(from, ioc4_config + 0x10, 0x01000000);
        qtest_writel(from, ioc4_memory + 0x10, bswap32(0x40));
        qtest_writel(from, ioc4_memory + 0x300, bswap32(0x80000000));
        qtest_writel(from, ioc4_memory + 0x304, 0);
        qtest_writeb(from, ioc4_memory + 0x87ff8, 0x80);
        qtest_writeb(from, ioc4_memory + 0x87ff9, 0x92);
        qtest_writeb(from, ioc4_memory + 0x87ffa, 0x23);
        qtest_writeb(from, ioc4_memory + 0x87ffb, 0x10);
        qtest_writeb(from, ioc4_memory + 0x87ffc, 0x02);
        qtest_writeb(from, ioc4_memory + 0x87ffd, 0x25);
        qtest_writeb(from, ioc4_memory + 0x87ffe, 0x09);
        qtest_writeb(from, ioc4_memory + 0x87fff, 0x26);
        qtest_writeb(from, ioc4_memory + 0x87ff8, 0x20);
        qtest_writeb(from, ioc4_memory + 0x380 + 3, 3);
        qtest_writeb(from, ioc4_memory + 0x381, 0x02);
        g_assert_cmphex(bswap32(qtest_readl(from, ioc4_memory + 0x008)) &
                        0x40, ==, 0x40);
        g_assert_cmphex(qtest_readq(from, IP53_PI_PENDING) &
                        IP53_PI_PENDING_BIT, ==, IP53_PI_PENDING_BIT);
        g_assert_cmphex(qtest_readb(from, ioc4_memory + 0x87ff9), ==, 0x92);
        g_assert_cmphex(qtest_readl(from, ioc4_memory + 0x300), ==,
                        bswap32(0x80000000));
        g_assert_cmphex(qtest_readb(from, ioc4_memory + 0x380 + 3), ==, 3);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    if (mismatch) {
        int64_t deadline = g_get_monotonic_time() +
                           10 * G_TIME_SPAN_SECOND;

        while (qtest_probe_child(to)) {
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(1000);
        }
        g_assert_false(qtest_probe_child(to));
    } else {
        sgi_wait_migration_status(to, "completed", 30);
        g_assert_cmphex(qtest_readq(to, IP53_PI_PENDING) &
                        IP53_PI_PENDING_BIT, ==, IP53_PI_PENDING_BIT);
        g_assert_cmphex(bswap32(qtest_readl(to, ioc4_memory + 0x008)) &
                        0x40, ==, 0x40);
        g_assert_true(nasid_test_cpu_readq(to, 0, io_registers + 0x160,
                                           &value));
        g_assert_cmphex(value, ==, itte);
        g_assert_cmphex(qtest_readb(to, ioc4_memory + 0x87ff9), ==, 0x92);
        g_assert_cmphex(qtest_readl(to, ioc4_memory + 0x300), ==,
                        bswap32(0x80000000));
        g_assert_cmphex(qtest_readb(to, ioc4_memory + 0x380 + 3), ==, 3);
        g_assert_cmphex(qtest_readb(to, ioc4_memory + 0x382) & 0x0f,
                        ==, 0x02);
        qtest_writel(to, ioc4_memory + 0x008, bswap32(0x40));
        g_assert_cmphex(bswap32(qtest_readl(to, ioc4_memory + 0x008)) &
                        0x40, ==, 0);
        g_assert_cmphex(qtest_readq(to, IP53_PI_PENDING) &
                        IP53_PI_PENDING_BIT, ==, 0);
    }
    if (mismatch && qtest_probe_child(from)) {
        int64_t deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;

        qtest_qmp_assert_success(from, "{'execute':'quit'}");
        while (qtest_probe_child(from)) {
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(1000);
        }
    } else if (!mismatch) {
        {
            g_autoptr(QDict) status = qtest_qmp_assert_success_ref(to,
                "{'execute':'query-status'}");

            g_test_message("destination state before activation: %s",
                           qdict_get_str(status, "status"));
            g_assert_false(qdict_get_bool(status, "running"));
        }
        qtest_qmp_assert_success(to, "{'execute':'cont'}");
        {
            g_autoptr(QDict) status = qtest_qmp_assert_success_ref(to,
                "{'execute':'query-status'}");

            g_assert_true(qdict_get_bool(status, "running"));
            g_assert_cmpstr(qdict_get_str(status, "status"), ==, "running");
        }
        qtest_qmp_assert_success(to, "{'execute':'quit'}");
        qtest_wait_qemu(to);
        qtest_qmp_assert_success(from, "{'execute':'quit'}");
        qtest_wait_qemu(from);
    }
    qtest_quit(to);
    qtest_quit(from);
    if (!mismatch) {
        g_assert_true(g_file_get_contents(from_data, &from_data_image,
                                          &from_data_size, &error));
        g_assert_no_error(error);
        g_assert_cmpuint(from_data_size, ==, 0x800);
        g_assert_cmphex((uint8_t)from_data_image[0x7f9], ==, 0x92);
        g_assert_cmphex((uint8_t)from_data_image[0x7fa], ==, 0x23);
        g_assert_true(g_file_get_contents(to_data, &to_data_image,
                                          &to_data_size, &error));
        g_assert_no_error(error);
        g_assert_cmpuint(to_data_size, ==, 0x800);
        g_assert_cmphex((uint8_t)to_data_image[0x7f9], ==, 0x92);
        g_assert_cmphex((uint8_t)to_data_image[0x7fa], ==, 0x23);
    }
    g_unlink(socket);
    g_unlink(to_clock);
    g_unlink(to_data);
    g_unlink(from_clock);
    g_unlink(from_data);
    g_unlink(prom);
    g_rmdir(dir);
}

static void test_ip53_runtime_permission_refusals(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *blank = g_malloc0(0x100000);
    g_autofree char *bios = NULL;
    g_autofree char *data_path = NULL;
    g_autofree char *clock_path = NULL;
    int bios_fd, data_fd, clock_fd;
    static const struct {
        const char *machine;
        const char *memory;
        const char *cpus;
        const char *cpu;
        const char *message;
        bool qtest;
    } cases[] = {
        { "origin350,topology=origin350-ip53-experiment",
          "2G", "4", "R16000",
          "origin350-ip53-experiment requires ip53-experimental=on", false },
        { "origin300,ip53-experimental=on", "512M", "2", "R14000",
          "ip53-experimental=on requires the origin350 machine and "
          "origin350-ip53-experiment topology", false },
        { "origin350,topology=origin300-pci-expansion-experiment,"
          "ip53-experimental=on", "2G", "4", "R16000",
          "unknown topology", false },
        { "sn-builder-bedrock-test,construction-fault=ip53-pic-io9,"
          "ip53-experimental=on", "64M", "1", "R14000",
          "qtest fixture topology cannot enable the IP53 runtime experiment",
          true },
        { "sn-builder-bedrock-test,construction-fault=ip53-extra-attachment",
          "64M", "1", "R14000",
          "IP53 construction has unowned XIO attachment", true },
        { "sn-builder-bedrock-test,construction-fault=ip53-extra-crossbar",
          "64M", "1", "R14000",
          "IP53 construction does not support XIO crossbars", true },
    };

    bios_fd = g_file_open_tmp("sn-builder-ip53-refusal-prom-XXXXXX.bin",
                              &bios, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(bios_fd, blank, 0x100000), ==, 0x100000);
    g_assert_cmpint(close(bios_fd), ==, 0);
    data_fd = g_file_open_tmp("sn-builder-ip53-refusal-data-XXXXXX",
                              &data_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(data_fd, 0x800), ==, 0);
    g_assert_cmpint(close(data_fd), ==, 0);
    clock_fd = g_file_open_tmp("sn-builder-ip53-refusal-clock-XXXXXX",
                               &clock_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(clock_fd, 16), ==, 0);
    g_assert_cmpint(close(clock_fd), ==, 0);

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        if (cases[i].qtest) {
            const char *args[] = {
                qtest_qemu_binary(NULL), "-M", cases[i].machine,
                "-m", cases[i].memory, "-smp", cases[i].cpus,
                "-cpu", cases[i].cpu, "-bios", bios,
                "-drive", NULL, "-drive", NULL,
                "-qtest", "stdio", "-accel", "qtest", "-display",
                "none", NULL,
            };
            g_autofree char *data_drive = g_strdup_printf(
                "if=none,id=sgi-ds1742-data-fixture-io9,file=%s,format=raw",
                data_path);
            g_autofree char *clock_drive = g_strdup_printf(
                "if=none,id=sgi-ds1742-clock-fixture-io9,file=%s,format=raw",
                clock_path);

            args[12] = data_drive;
            args[14] = clock_drive;
            assert_qemu_rejects(args, cases[i].message);
        } else {
            const char *args[] = {
                qtest_qemu_binary(NULL), "-M", cases[i].machine,
                "-m", cases[i].memory, "-smp", cases[i].cpus,
                "-cpu", cases[i].cpu, "-display", "none", NULL,
            };

            assert_qemu_rejects(args, cases[i].message);
        }
    }
    g_unlink(bios);
    g_unlink(data_path);
    g_unlink(clock_path);
}

static void test_ip53_runtime_permission_defaults_false_and_immutable(void)
{
    static const uint8_t debug_probe[] = {
        0x31, 0x42, 0x53, 0x64, 0x75, 0x86, 0x97, 0xa8,
    };
    g_autoptr(GError) err = NULL;
    g_autofree uint8_t *blank = g_malloc0(IP53_TEST_PROM_SIZE);
    g_autofree char *bios = NULL;
    uint8_t debug_bytes[sizeof(debug_probe)];
    QTestState *qts;
    int fd;
    QDict *error;

    memcpy(blank + 0x100000, debug_probe, sizeof(debug_probe));
    fd = g_file_open_tmp("sn-builder-ip53-permission-prom-XXXXXX.bin",
                         &bios, &err);
    g_assert_no_error(err);
    g_assert_cmpint(write(fd, blank, IP53_TEST_PROM_SIZE), ==,
                    IP53_TEST_PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf("-M origin300 -m 512M -smp 2 -cpu R14000 "
                      "-bios %s -display none", bios);

    g_assert_false(qom_get_bool(qts, "/machine", "ip53-experimental"));
    g_assert_true(qtest_memread_cpu_debug(
        qts, 0, UINT64_C(0xffffffffbfd00000), debug_bytes,
        sizeof(debug_bytes)));
    g_assert_cmpmem(debug_bytes, sizeof(debug_bytes), debug_probe,
                    sizeof(debug_probe));
    error = qtest_qmp_assert_failure_ref(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'ip53-experimental','value':true}}");
    g_assert_nonnull(strstr(qdict_get_str(error, "desc"),
                            "ip53-experimental cannot change"));
    qobject_unref(error);
    qtest_quit(qts);
    g_unlink(bios);
}


static void test_ip53_runtime_construction(void)
{
    g_autoptr(GError) err = NULL;
    g_autofree uint8_t *blank = g_malloc0(IP53_TEST_PROM_SIZE);
    g_autofree char *bios = NULL;
    g_autofree char *flash_path = NULL;
    g_autofree char *bedrock = NULL;
    g_autofree char *boot_flash = NULL;
    g_autofree char *pic = NULL;
    g_autofree char *ioc4 = NULL;
    g_autofree char *scsi = NULL;
    g_autofree char *ethernet = NULL;
    g_autofree char *rtc = NULL;
    g_autofree char *data_path = NULL;
    g_autofree char *clock_path = NULL;
    int fd;
    int flash_fd, data_fd, clock_fd;
    QTestState *qts;
    QDict *response;
    QList *cpus;
    const QListEntry *entry;
    unsigned int cpu_count = 0;
    uint64_t board_id;

    fd = g_file_open_tmp("sn-builder-origin350-ip53-prom-XXXXXX.bin",
                         &bios, &err);
    g_assert_no_error(err);
    g_assert_cmpint(write(fd, blank, IP53_TEST_PROM_SIZE), ==,
                    IP53_TEST_PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    flash_fd = g_file_open_tmp("sn-builder-origin350-ip53-flash-XXXXXX",
                               &flash_path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(ftruncate(flash_fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(flash_fd), ==, 0);
    data_fd = g_file_open_tmp("sn-builder-origin350-ip53-data-XXXXXX",
                              &data_path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(ftruncate(data_fd, 0x800), ==, 0);
    g_assert_cmpint(close(data_fd), ==, 0);
    clock_fd = g_file_open_tmp("sn-builder-origin350-ip53-clock-XXXXXX",
                               &clock_path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(ftruncate(clock_fd, 16), ==, 0);
    g_assert_cmpint(close(clock_fd), ==, 0);
    qts = qtest_initf(
        "-M origin350,topology=origin350-ip53-experiment,"
        "ip53-experimental=on -m 2G -smp 4 "
        "-cpu R16000,r16000-experimental=on,"
        "r16000-reset-mode=0x009cad85,r16000-scache-bytes=0x800000 "
        "-bios %s "
        "-drive if=pflash,index=0,file=%s,format=raw "
        "-drive if=none,id=sgi-ds1742-data-io9,file=%s,format=raw "
        "-drive if=none,id=sgi-ds1742-clock-io9,file=%s,format=raw "
        "-display none -S", bios, flash_path, data_path, clock_path);

    /* A source-field assertion alone would miss a stale runtime projection. */
    g_assert_true(nasid_test_cpu_readq(qts, 0, IP53_BOARD_ID_LOCAL,
                                       &board_id));
    g_assert_cmphex(board_id, ==, 0x2001);
    g_assert_cmphex((board_id >> 12) & 0xf, ==, 2);
    g_assert_cmphex(board_id & 0xfff, ==, 1);
    g_assert_true(nasid_test_cpu_readq(qts, 0, IP53_BOARD_ID_REMOTE,
                                       &board_id));
    g_assert_cmphex(board_id, ==, 0x2001);

    g_assert_true(qom_get_bool(qts, "/machine", "ip53-experimental"));
    bedrock = qom_child_type(qts, "/machine/node0", "bedrock");
    boot_flash = qom_child_type(qts, "/machine/node0", "boot-flash");
    pic = qom_child_type(qts, "/machine", "pic-pic");
    ioc4 = qom_child_type(qts, "/machine", "io9-io9-d1-f0");
    scsi = qom_child_type(qts, "/machine", "io9-io9-d3-f0");
    ethernet = qom_child_type(qts, "/machine", "io9-io9-d4-f0");
    rtc = qom_child_type(qts, "/machine", "ds1742-io9");
    g_assert_cmpstr(bedrock, ==, "sgi-bedrock");
    g_assert_cmpstr(boot_flash, ==, "sgi-bedrock-flash");
    g_assert_cmpstr(pic, ==, "sgi-pic");
    g_assert_cmpstr(ioc4, ==, "sgi-ioc4");
    g_assert_cmpstr(scsi, ==, "qlogic-isp12160");
    g_assert_cmpstr(ethernet, ==, "bcm570x");
    ip53_bcm_assert_static_device(qts);
    g_assert_cmpstr(rtc, ==, "ds1742");
    g_assert_null(qom_child_type(qts, "/machine/node0", "xbridge"));

    response = qtest_qmp(qts, "{'execute':'query-cpus-fast'}");
    cpus = qdict_get_qlist(response, "return");
    for (entry = qlist_first(cpus); entry; entry = qlist_next(entry)) {
        QDict *cpu = qobject_to(QDict, qlist_entry_obj(entry));
        const char *path = qdict_get_str(cpu, "qom-path");

        g_assert_true(qom_get_bool(qts, path, "r16000-experimental"));
        g_assert_cmphex(qom_get_uint(qts, path, "r16000-reset-mode"), ==,
                        0x009cad85);
        g_assert_cmphex(qom_get_uint(qts, path, "r16000-scache-bytes"), ==,
                        0x800000);
        cpu_count++;
    }
    qobject_unref(response);
    g_assert_cmpuint(cpu_count, ==, 4);
    {
        QDict *failure = qtest_qmp_assert_failure_ref(qts,
            "{'execute':'qom-set','arguments':{'path':'/machine',"
            "'property':'ip53-experimental','value':false}}");

        g_assert_nonnull(strstr(qdict_get_str(failure, "desc"),
                                "ip53-experimental cannot change"));
        qobject_unref(failure);
    }
    qtest_quit(qts);
    g_unlink(bios);
    g_unlink(flash_path);
    g_unlink(data_path);
    g_unlink(clock_path);
}

typedef struct IP53RuntimeTestFixture {
    QTestState *qts;
    char *bios;
    char *flash_path;
    char *data_path;
    char *clock_path;
} IP53RuntimeTestFixture;

static IP53RuntimeTestFixture *ip53_runtime_test_start_with_binary(
    const char *binary_var, const char *machine, const char *extra)
{
    g_autoptr(GError) err = NULL;
    g_autofree uint8_t *blank = g_malloc0(IP53_TEST_PROM_SIZE);
    g_autofree char *arguments = NULL;
    IP53RuntimeTestFixture *fixture = g_new0(IP53RuntimeTestFixture, 1);
    int fd, flash_fd, data_fd, clock_fd;

    fd = g_file_open_tmp("sn-builder-origin350-ip53-test-prom-XXXXXX",
                         &fixture->bios, &err);
    g_assert_no_error(err);
    g_assert_cmpint(write(fd, blank, IP53_TEST_PROM_SIZE), ==,
                    IP53_TEST_PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    flash_fd = g_file_open_tmp("sn-builder-origin350-ip53-test-flash-XXXXXX",
                               &fixture->flash_path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(ftruncate(flash_fd, 16 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(flash_fd), ==, 0);
    data_fd = g_file_open_tmp("sn-builder-origin350-ip53-test-data-XXXXXX",
                              &fixture->data_path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(ftruncate(data_fd, 0x800), ==, 0);
    g_assert_cmpint(close(data_fd), ==, 0);
    clock_fd = g_file_open_tmp("sn-builder-origin350-ip53-test-clock-XXXXXX",
                               &fixture->clock_path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(ftruncate(clock_fd, 16), ==, 0);
    g_assert_cmpint(close(clock_fd), ==, 0);
    arguments = g_strdup_printf(
        "-M %s -m 2G -smp 4 "
        "-cpu R16000,r16000-experimental=on,"
        "r16000-reset-mode=0x009cad85,r16000-scache-bytes=0x800000 "
        "-bios %s "
        "-drive if=pflash,index=0,file=%s,format=raw "
        "-drive if=none,id=sgi-ds1742-data-io9,file=%s,format=raw "
        "-drive if=none,id=sgi-ds1742-clock-io9,file=%s,format=raw "
        "-display none -S %s", machine, fixture->bios, fixture->flash_path,
        fixture->data_path, fixture->clock_path, extra ? extra : "");
    fixture->qts = qtest_init_ext(binary_var, arguments, NULL, true);

    return fixture;
}

static IP53RuntimeTestFixture *ip53_runtime_test_start(const char *machine,
                                                       const char *extra)
{
    return ip53_runtime_test_start_with_binary(NULL, machine, extra);
}

static void ip53_runtime_test_stop(IP53RuntimeTestFixture *fixture)
{
    qtest_quit(fixture->qts);
    g_unlink(fixture->bios);
    g_unlink(fixture->flash_path);
    g_unlink(fixture->data_path);
    g_unlink(fixture->clock_path);
    g_free(fixture->bios);
    g_free(fixture->flash_path);
    g_free(fixture->data_path);
    g_free(fixture->clock_path);
    g_free(fixture);
}

static void test_ip53_integrated_bte_reset_overlap(void)
{
    uint8_t source[128];
    uint8_t source_readback[sizeof(source)];
    uint8_t destination[sizeof(source)];
    uint64_t status;
    uint64_t notification = UINT64_C(0xfeedfacecafef00d);
    IP53RuntimeTestFixture *fixture =
        ip53_runtime_test_start("origin350,ip53-experimental=on", NULL);
    QTestState *qts = fixture->qts;

    for (size_t i = 0; i < sizeof(source); i++) {
        source[i] = (i * 37 + 11) & 0xff;
    }
    memset(destination, 0xa5, sizeof(destination));
    qtest_writeq(qts, IP53_IO_SPACE + UINT64_C(0x1000000) + 0x780008,
                 UINT64_C(0x53));
    g_assert_cmphex(qtest_readq(
                        qts, IP53_IO_SPACE + UINT64_C(0x1000000) + 0x780008),
                    ==, UINT64_C(0x53));
    qtest_memwrite(qts, IP53_BTE_SOURCE, source, sizeof(source));
    qtest_memread(qts, IP53_BTE_SOURCE, source_readback,
                  sizeof(source_readback));
    g_assert_cmpmem(source_readback, sizeof(source_readback), source,
                    sizeof(source));
    qtest_memwrite(qts, IP53_BTE_DESTINATION, destination,
                   sizeof(destination));
    qtest_writeq(qts, IP53_BTE_NOTIFICATION, notification);

    /* BTE0 copies one 128-byte line into RAM under the reset carrier range. */
    qtest_writeq(qts, IP53_BTE0 + 0x08, IP53_BTE_SOURCE);
    qtest_writeq(qts, IP53_BTE0 + 0x10, IP53_BTE_DESTINATION);
    qtest_writeq(qts, IP53_BTE0 + 0x20, IP53_BTE_NOTIFICATION);
    qtest_writeq(qts, IP53_BTE0, IP53_BTE_LENGTH_BUSY | 1);
    g_assert_cmphex(qtest_readq(qts, IP53_BTE0 + 0x08), ==,
                    IP53_BTE_SOURCE);
    g_assert_cmphex(qtest_readq(qts, IP53_BTE0 + 0x10), ==,
                    IP53_BTE_DESTINATION);
    g_assert_cmphex(qtest_readq(qts, IP53_BTE0 + 0x20), ==,
                    IP53_BTE_NOTIFICATION);
    g_assert_cmphex(qtest_readq(qts, IP53_BTE0), ==,
                    IP53_BTE_LENGTH_BUSY | 1);
    qtest_writeq(qts, IP53_BTE0 + 0x18, IP53_BTE_NOTIFY_ENABLE);

    status = qtest_readq(qts, IP53_BTE0);
    g_assert_cmphex(status, ==, 0);
    g_assert_cmphex(qtest_readq(qts, IP53_BTE0 + 0x08), ==,
                    IP53_BTE_SOURCE + sizeof(source));
    g_assert_cmphex(qtest_readq(qts, IP53_BTE0 + 0x10), ==,
                    IP53_BTE_DESTINATION + sizeof(destination));
    g_assert_cmphex(qtest_readq(qts, IP53_BTE0 + 0x20), ==,
                    IP53_BTE_NOTIFICATION);
    qtest_memread(qts, IP53_BTE_DESTINATION, destination,
                  sizeof(destination));
    g_assert_cmpmem(destination, sizeof(destination), source, sizeof(source));
    g_assert_cmphex(qtest_readq(qts, IP53_BTE_NOTIFICATION), ==, 0);

    /*
     * The global map exposes copied RAM beneath the CPU-only reset view;
     * KSEG1 debug reads still select the unchanged synthetic PROM bytes.
     */
    g_assert_true(qtest_memread_cpu_debug(
        qts, 0,
        UINT64_C(0xffffffffbfc00000) +
        (IP53_BTE_DESTINATION - IP53_RESET_PROM),
        destination, sizeof(destination)));
    for (size_t i = 0; i < sizeof(destination); i++) {
        g_assert_cmphex(destination[i], ==, 0);
    }

    ip53_runtime_test_stop(fixture);
}


static void test_ip53_static_bcm5701_nic_none(void)
{
    IP53RuntimeTestFixture *fixture = ip53_runtime_test_start(
        "origin350,topology=origin350-ip53-experiment,ip53-experimental=on",
        "-nic none");

    ip53_bcm_assert_static_device(fixture->qts);
    ip53_runtime_test_stop(fixture);
}

static void test_ip53_static_bcm5701_network_backend(void)
{
    static const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0xbc, 0x57, 0x01 };
    uint8_t tx_frame[IP53_BCM_FRAME_SIZE];
    uint8_t rx_frame[IP53_BCM_FRAME_SIZE] = { 0 };
    uint8_t received[IP53_BCM_FRAME_SIZE];
    uint8_t tx_descriptor[16] = { 0 };
    uint8_t rx_descriptor[32] = { 0 };
    uint8_t completed[32];
    struct timeval timeout = { .tv_sec = 5 };
    IP53RuntimeTestFixture *fixture;
    g_autofree char *nic = NULL;
    int sockets[2];
    uint32_t value;
    unsigned int i;
    size_t received_length;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    g_assert_cmpint(setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO,
                               &timeout, sizeof(timeout)), ==, 0);
    nic = g_strdup_printf("-nic socket,fd=%d,mac=52:54:00:bc:57:01",
                          sockets[1]);
    fixture = ip53_runtime_test_start(
        "origin350,topology=origin350-ip53-experiment,ip53-experimental=on",
        nic);
    close(sockets[1]);
    ip53_bcm_assert_static_device(fixture->qts);
    {
        g_autoptr(QDict) response = qtest_qmp(fixture->qts,
            "{ 'execute': 'query-status' }");
        QDict *status = qdict_get_qdict(response, "return");

        g_assert_cmpstr(qdict_get_str(status, "status"), ==, "prelaunch");
        g_assert_false(qdict_get_bool(status, "running"));
    }
    qtest_qmp_assert_success(fixture->qts, "{ 'execute': 'cont' }");
    {
        g_autoptr(QDict) response = qtest_qmp(fixture->qts,
            "{ 'execute': 'query-status' }");
        QDict *status = qdict_get_qdict(response, "return");

        g_test_message("IP53 QMP status after cont: %s running=%u",
                       qdict_get_str(status, "status"),
                       qdict_get_bool(status, "running"));
        g_assert_true(qdict_get_bool(status, "running"));
    }
    {
        g_autofree char *network = qtest_hmp(fixture->qts, "info network");

        g_test_message("IP53 network clients:\n%s", network);
    }
    {
        g_autoptr(QDict) response = qtest_qmp(fixture->qts,
            "{ 'execute': 'human-monitor-command', 'arguments': "
            "{ 'command-line': 'info mtree' } }");
        const char *mtree = qdict_get_str(response, "return");
        g_auto(GStrv) lines = g_strsplit(mtree, "\n", -1);
        g_autoptr(GString) ram_map = g_string_new(NULL);
        unsigned int count = 0;

        for (unsigned int line = 0; lines[line] && count < 12; line++) {
            if (strstr(lines[line], "ram") || strstr(lines[line], "RAM")) {
                g_string_append_printf(ram_map, "%s\n", lines[line]);
                count++;
            }
        }
        g_test_message("IP53 RAM map:\n%s", ram_map->str);
    }

    qtest_writeq(fixture->qts, IP53_BCM_RUNTIME_PIC_HEAD0 + 0x20, 9);
    qtest_writeq(fixture->qts, IP53_BCM_RUNTIME_PIC_HEAD0 + 0x80,
                 UINT64_C(15) << 20);
    qtest_writeq(fixture->qts, IP53_IO_SPACE + 0x1400000 + 0x160,
                 (UINT64_C(15) << 8) | 2);
    qtest_writeq(fixture->qts, IP53_IO_SPACE + 0x1000000 + 0x780008,
                 UINT64_C(0x53));
    {
        uint64_t pic_widget = 0, pic_dir_map = 0, pic_route = 0;
        uint64_t md_memory_config = 0;
        bool pic_widget_readable, pic_dir_map_readable;
        bool pic_route_readable, md_memory_config_readable;

        pic_widget_readable = nasid_test_cpu_readq(
            fixture->qts, 0, IP53_BCM_RUNTIME_PIC_HEAD0 + 0x20,
            &pic_widget);
        pic_dir_map_readable = nasid_test_cpu_readq(
            fixture->qts, 0, IP53_BCM_RUNTIME_PIC_HEAD0 + 0x80,
            &pic_dir_map);
        pic_route_readable = nasid_test_cpu_readq(
            fixture->qts, 0, IP53_IO_SPACE + 0x1400000 + 0x160,
            &pic_route);
        md_memory_config_readable = nasid_test_cpu_readq(
            fixture->qts, 0, IP53_IO_SPACE + 0x1000000 + 0x780008,
            &md_memory_config);
        g_test_message("BCM setup registers: PIC widget=%u/%016" PRIx64
                       " dir-map=%u/%016" PRIx64
                       " route=%u/%016" PRIx64
                       " MD config=%u/%016" PRIx64,
                       pic_widget_readable, pic_widget,
                       pic_dir_map_readable, pic_dir_map,
                       pic_route_readable, pic_route,
                       md_memory_config_readable, md_memory_config);
    }
    {
        static const uint8_t sentinel[4] = { 0x78, 0x56, 0x34, 0x12 };
        uint8_t readback[sizeof(sentinel)];
        bool cpu_visible;

        qtest_memwrite(fixture->qts, IP53_BCM_TX_RING_RAM, sentinel,
                       sizeof(sentinel));
        qtest_memread(fixture->qts, IP53_BCM_TX_RING_RAM, readback,
                      sizeof(readback));
        cpu_visible = nasid_test_cpu_readl(fixture->qts, 0,
                                           IP53_BCM_TX_RING_RAM, &value);
        g_test_message("BCM RAM sentinel: write=%02x%02x%02x%02x "
                       "qtest=%02x%02x%02x%02x cpu-visible=%u cpu=%08x",
                       sentinel[0], sentinel[1], sentinel[2], sentinel[3],
                       readback[0], readback[1], readback[2], readback[3],
                       cpu_visible, value);
        g_assert_cmpmem(readback, sizeof(readback), sentinel,
                        sizeof(sentinel));
        g_assert_true(cpu_visible);
        g_assert_cmphex(value, ==, 0x78563412);
    }
    qtest_writel(fixture->qts, IP53_BCM_CFG + 4,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(fixture->qts, IP53_BCM_CFG + 0x10, 0x01000000);
    g_assert_true(nasid_test_cpu_readl(fixture->qts, 0,
                                       IP53_BCM_CFG + 0x10, &value));
    g_assert_cmphex(value, ==, 0x01000000);
    g_assert_true(nasid_test_cpu_readl(fixture->qts, 0,
                                       IP53_BCM_BAR + 0x0410, &value));
    g_assert_cmphex(value, ==, 0x00005254);
    g_assert_true(nasid_test_cpu_readl(fixture->qts, 0,
                                       IP53_BCM_BAR + 0x0414, &value));
    g_assert_cmphex(value, ==, 0x00bc5701);

    for (i = 0; i < sizeof(tx_frame); i++) {
        tx_frame[i] = i * 7 + 1;
    }
    ip53_bcm_put32le(tx_descriptor, 0);
    ip53_bcm_put32le(tx_descriptor + 4, IP53_BCM_TX_BUFFER);
    ip53_bcm_put16le(tx_descriptor, 8, 4);
    ip53_bcm_put16le(tx_descriptor, 10, sizeof(tx_frame));
    qtest_memwrite(fixture->qts, IP53_BCM_TX_RING_RAM, tx_descriptor,
                   sizeof(tx_descriptor));
    qtest_memwrite(fixture->qts, IP53_BCM_TX_BUFFER_RAM, tx_frame,
                   sizeof(tx_frame));
    {
        uint8_t descriptor[sizeof(tx_descriptor)];
        uint32_t address_word;
        bool cpu_visible;

        qtest_memread(fixture->qts, IP53_BCM_TX_RING_RAM, descriptor,
                      sizeof(descriptor));
        cpu_visible = nasid_test_cpu_readl(fixture->qts, 0,
                                           IP53_BCM_TX_RING_RAM + 4,
                                           &address_word);
        g_test_message("BCM TX before kick: descriptor=%08x:%08x "
                       "CPU visible=%u address word=%08x",
                       ip53_bcm_get32le(descriptor),
                       ip53_bcm_get32le(descriptor + 4), cpu_visible,
                       address_word);
    }
    qtest_memset(fixture->qts, IP53_BCM_STATUS_RAM, 0, 0x50);
    ip53_bcm_sram_write(fixture->qts, 0x100, IP53_BCM_TX_RING >> 32);
    ip53_bcm_sram_write(fixture->qts, 0x104,
                        IP53_BCM_TX_RING & 0xffffffff);
    ip53_bcm_sram_write(fixture->qts, 0x108, 4 << 16);
    qtest_writel(fixture->qts, IP53_BCM_BAR + 0x3c38,
                 IP53_BCM_STATUS >> 32);
    qtest_writel(fixture->qts, IP53_BCM_BAR + 0x3c3c,
                 IP53_BCM_STATUS & 0xffffffff);
    qtest_writel(fixture->qts, IP53_BCM_BAR + 0x3c14, 1);
    qtest_writel(fixture->qts, IP53_BCM_BAR + 0x3c00, 2);
    qtest_writel(fixture->qts, IP53_BCM_BAR + 0x0304, 1);
    {
        uint32_t rcb_hi, rcb_lo, rcb_len, producer;
        uint8_t descriptor[sizeof(tx_descriptor)];
        uint8_t status[4];

        g_assert_true(ip53_bcm_sram_read_cpu(fixture->qts, 0x100, &rcb_hi));
        g_assert_true(ip53_bcm_sram_read_cpu(fixture->qts, 0x104, &rcb_lo));
        g_assert_true(ip53_bcm_sram_read_cpu(fixture->qts, 0x108, &rcb_len));
        g_assert_true(nasid_test_cpu_readl(fixture->qts, 0,
                                           IP53_BCM_BAR + 0x0304, &producer));
        qtest_memread(fixture->qts, IP53_BCM_TX_RING_RAM, descriptor,
                      sizeof(descriptor));
        qtest_memread(fixture->qts, IP53_BCM_STATUS_RAM + 16, status,
                      sizeof(status));
        g_test_message("BCM TX: RCB=%08x:%08x len=%08x producer=%08x "
                       "descriptor=%08x:%08x flags=%04x length=%u "
                       "status=%02x%02x%02x%02x",
                       rcb_hi, rcb_lo, rcb_len, producer,
                       ip53_bcm_get32le(descriptor),
                       ip53_bcm_get32le(descriptor + 4),
                       ip53_bcm_get16le(descriptor + 8),
                       ip53_bcm_get16le(descriptor + 10), status[0],
                       status[1], status[2], status[3]);
    }
    received_length = ip53_bcm_receive_frame(sockets[0], received,
                                             sizeof(received));
    g_assert_cmpuint(received_length, ==, sizeof(tx_frame));
    g_assert_cmpmem(received, received_length, tx_frame, sizeof(tx_frame));

    memcpy(rx_frame, mac, sizeof(mac));
    rx_frame[6] = 0x52;
    rx_frame[7] = 0x54;
    rx_frame[12] = 0x08;
    rx_frame[13] = 0x00;
    for (i = 14; i < sizeof(rx_frame); i++) {
        rx_frame[i] = i;
    }
    ip53_bcm_put32le(rx_descriptor, 0);
    ip53_bcm_put32le(rx_descriptor + 4, IP53_BCM_RX_BUFFER);
    qtest_memwrite(fixture->qts, IP53_BCM_RX_RING_RAM, rx_descriptor,
                   sizeof(rx_descriptor));
    qtest_memset(fixture->qts, IP53_BCM_RX_RETURN_RAM, 0,
                 sizeof(completed));
    qtest_memset(fixture->qts, IP53_BCM_RX_BUFFER_RAM, 0, sizeof(rx_frame));
    qtest_writel(fixture->qts, IP53_BCM_BAR + 0x2450,
                 IP53_BCM_RX_RING >> 32);
    qtest_writel(fixture->qts, IP53_BCM_BAR + 0x2454,
                 IP53_BCM_RX_RING & 0xffffffff);
    qtest_writel(fixture->qts, IP53_BCM_BAR + 0x2458, 1536 << 16);
    ip53_bcm_sram_write(fixture->qts, 0x200, IP53_BCM_RX_RETURN >> 32);
    ip53_bcm_sram_write(fixture->qts, 0x204,
                        IP53_BCM_RX_RETURN & 0xffffffff);
    ip53_bcm_sram_write(fixture->qts, 0x208, 4 << 16);
    qtest_writel(fixture->qts, IP53_BCM_BAR + 0x026c, 1);
    qtest_writel(fixture->qts, IP53_BCM_BAR + 0x3c10, 1);
    qtest_writel(fixture->qts, IP53_BCM_BAR + 0x0468, 2);

    ip53_bcm_send_frame(sockets[0], rx_frame, sizeof(rx_frame));
    for (i = 0; i < 200; i++) {
        qtest_clock_step(fixture->qts, 1000000);
        qtest_memread(fixture->qts, IP53_BCM_RX_RETURN_RAM, completed,
                      sizeof(completed));
        value = ip53_bcm_get32le(completed + 8);
        if (value) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmphex(value, !=, 0);
    qtest_memread(fixture->qts, IP53_BCM_RX_BUFFER_RAM, received,
                  sizeof(received));
    g_assert_cmpmem(received, sizeof(received), rx_frame, sizeof(rx_frame));

    ip53_runtime_test_stop(fixture);
    close(sockets[0]);
}

#define IP53_RUNTIME_PI_LEVEL_IOC4 0x34
#define IP53_RUNTIME_PI_LEVEL_ISP  0x35
#define IP53_RUNTIME_PI_LEVEL_BCM  0x36
#define IP53_RUNTIME_PIC_STATUS    (IP53_BCM_RUNTIME_PIC_HEAD0 + 0x100)
#define IP53_RUNTIME_PIC_ENABLE    (IP53_BCM_RUNTIME_PIC_HEAD0 + 0x108)
#define IP53_RUNTIME_PIC_CLEAR     (IP53_BCM_RUNTIME_PIC_HEAD0 + 0x110)
#define IP53_RUNTIME_PIC_MODE      (IP53_BCM_RUNTIME_PIC_HEAD0 + 0x118)
#define IP53_RUNTIME_PIC_DEST(pin) \
    (IP53_BCM_RUNTIME_PIC_HEAD0 + 0x130 + (pin) * sizeof(uint64_t))
#define IP53_RUNTIME_ISP_CONFIG \
    (IP53_BCM_RUNTIME_PIC_HEAD0 + 0x23000)
#define IP53_RUNTIME_ISP_CPU_VIEW \
    (IP53_BCM_RUNTIME_PIC_HEAD0 + UINT64_C(0x200000))
#define IP53_RUNTIME_BCM_CPU_VIEW \
    (IP53_BCM_RUNTIME_PIC_HEAD0 + UINT64_C(0x400000))
#define IP53_RUNTIME_IOC4_CPU_VIEW \
    (IP53_BCM_RUNTIME_PIC_HEAD0 + UINT64_C(0x600000))
#define IP53_RUNTIME_IOC4_BAR \
    (IP53_IO_SPACE + UINT64_C(0x21000000))
#define IP53_RUNTIME_BCM_BAR \
    (IP53_IO_SPACE + UINT64_C(0x22000000))

static void ip53_runtime_program_interrupt_routes(QTestState *qts,
                                                  uint64_t enabled)
{
    const unsigned int pins[] = { 0, 2, 3 };
    const unsigned int levels[] = {
        IP53_RUNTIME_PI_LEVEL_IOC4,
        IP53_RUNTIME_PI_LEVEL_ISP,
        IP53_RUNTIME_PI_LEVEL_BCM,
    };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(pins); i++) {
        uint64_t destination = ((uint64_t)levels[i] << 48) |
                               IP53_PI_INTERRUPT_ADDRESS;

        qtest_writeq(qts, IP53_RUNTIME_PIC_DEST(pins[i]), destination);
        g_assert_cmphex(qtest_readq(qts, IP53_RUNTIME_PIC_DEST(pins[i])), ==,
                        destination);
    }
    qtest_writeq(qts, IP53_RUNTIME_PIC_MODE, (1U << 0) | (1U << 2) |
                 (1U << 3));
    qtest_writeq(qts, IP53_RUNTIME_PIC_ENABLE, enabled);
}

static uint64_t ip53_runtime_pi_bit(unsigned int level)
{
    return UINT64_C(1) << level;
}

static void ip53_runtime_assert_pic_input(QTestState *qts, unsigned int pin,
                                          unsigned int level)
{
    uint64_t status = qtest_readq(qts, IP53_RUNTIME_PIC_STATUS);
    uint64_t pending = qtest_readq(qts, IP53_PI_PENDING);

    g_printerr("IO9 interrupt input %u: status=%016" PRIx64
               " PI pending=%016" PRIx64 "\n", pin, status, pending);
    g_assert_cmphex(status & (UINT64_C(1) << pin), !=, 0);
    g_assert_cmphex(pending & ip53_runtime_pi_bit(level), !=, 0);
}

static void ip53_runtime_clear_pic_status(QTestState *qts,
                                          unsigned int pin)
{
    qtest_writeq(qts, IP53_RUNTIME_PIC_CLEAR, UINT64_C(1) << pin);
    g_assert_cmphex(qtest_readq(qts, IP53_RUNTIME_PIC_STATUS) &
                    (UINT64_C(1) << pin), ==, 0);
}

static void ip53_runtime_configure_ioc4(QTestState *qts)
{
    uint64_t config = IP53_BCM_RUNTIME_PIC_HEAD0 + 0x21000;
    uint64_t io_route = (UINT64_C(15) << 8) | 2;

    qtest_writel(qts, config + 4, PCI_COMMAND_MEMORY);
    qtest_writel(qts, config + 0x10, 0x01000000);
    g_assert_cmphex(qtest_readl(qts, config + 4) & 0xffff, ==,
                    PCI_COMMAND_MEMORY);
    g_assert_cmphex(qtest_readl(qts, config + 0x10), ==, 0x01000000);
    qtest_writeq(qts, IP53_IO_SPACE + UINT64_C(0x1400000) + 0x160,
                 io_route);
    g_assert_cmphex(qtest_readq(qts,
                    IP53_IO_SPACE + UINT64_C(0x1400000) + 0x160), ==,
                    io_route);
    qtest_writeq(qts, IP53_IO_SPACE + UINT64_C(0x1000000) + 0x780008,
                 0x53);
    qtest_writeq(qts, IP53_BCM_RUNTIME_PIC_HEAD0 + 0x210,
                 0x1010);
    g_assert_cmphex(qtest_readq(qts,
                    IP53_BCM_RUNTIME_PIC_HEAD0 + 0x210), ==, 0x1010);
    qtest_writel(qts, IP53_RUNTIME_IOC4_CPU_VIEW + 0x08, UINT32_MAX);
    qtest_writel(qts, IP53_RUNTIME_IOC4_CPU_VIEW + 0x0c, UINT32_MAX);
    qtest_writel(qts, IP53_RUNTIME_IOC4_CPU_VIEW + 0x10, 0x40);
    g_assert_cmphex(qtest_readl(qts,
                    IP53_RUNTIME_IOC4_CPU_VIEW + 0x10), ==, 0x40);
}

static void ip53_runtime_raise_ioc4_irq(QTestState *qts)
{
    qtest_writeb(qts, IP53_RUNTIME_IOC4_CPU_VIEW + 0x381, 2);
    g_assert_cmphex(qtest_readl(qts,
                    IP53_RUNTIME_IOC4_CPU_VIEW + 0x08) & 0x40, !=, 0);
}

static void ip53_runtime_clear_ioc4_irq(QTestState *qts)
{
    qtest_writeb(qts, IP53_RUNTIME_IOC4_CPU_VIEW + 0x381, 0);
    g_assert_cmphex(qtest_readb(qts,
                    IP53_RUNTIME_IOC4_CPU_VIEW + 0x381), ==, 0);
    qtest_writel(qts, IP53_RUNTIME_IOC4_CPU_VIEW + 0x08, 0x40);
    g_assert_cmphex(qtest_readl(qts,
                    IP53_RUNTIME_IOC4_CPU_VIEW + 0x08) & 0x40, ==, 0);
}

static void ip53_runtime_configure_isp(QTestState *qts)
{
    qtest_writel(qts, IP53_RUNTIME_ISP_CONFIG + 4, PCI_COMMAND_MEMORY);
    qtest_writel(qts, IP53_RUNTIME_ISP_CONFIG + 0x14, 0x05000000);
    g_assert_cmphex(qtest_readl(qts, IP53_RUNTIME_ISP_CONFIG + 4) & 0xffff,
                    ==, PCI_COMMAND_MEMORY);
    g_assert_cmphex(qtest_readl(qts, IP53_RUNTIME_ISP_CONFIG + 0x14), ==,
                    0x05000000);
    qtest_writeq(qts, IP53_BCM_RUNTIME_PIC_HEAD0 + 0x200, 0x1050);
    g_assert_cmphex(qtest_readq(qts,
                    IP53_BCM_RUNTIME_PIC_HEAD0 + 0x200), ==, 0x1050);
    g_assert_cmphex(qtest_readw(qts, IP53_RUNTIME_ISP_CPU_VIEW + 0x70),
                    ==, 0x4953);
    qtest_writew(qts, IP53_RUNTIME_ISP_CPU_VIEW + 0x0a, 0x0006);
    g_assert_cmphex(qtest_readw(qts,
                    IP53_RUNTIME_ISP_CPU_VIEW + 0x0a), ==, 0x0006);
}

static void ip53_runtime_raise_isp_irq(QTestState *qts)
{
    qtest_writew(qts, IP53_RUNTIME_ISP_CPU_VIEW + 0x72, 0);
    qtest_writew(qts, IP53_RUNTIME_ISP_CPU_VIEW + 0xc2, 0x5000);
    g_assert_cmphex(qtest_readw(qts,
                    IP53_RUNTIME_ISP_CPU_VIEW + 0x08) & 0x0006, ==, 0x0006);
}

static void ip53_runtime_clear_isp_irq(QTestState *qts)
{
    qtest_writew(qts, IP53_RUNTIME_ISP_CPU_VIEW + 0xc2, 0x7000);
    qtest_writew(qts, IP53_RUNTIME_ISP_CPU_VIEW + 0x0e, 0);
}

static void ip53_runtime_configure_bcm(QTestState *qts)
{
    qtest_writel(qts, IP53_BCM_CFG + 4,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, IP53_BCM_CFG + 0x10, 0x02000000);
    g_assert_cmphex(qtest_readl(qts, IP53_BCM_CFG + 4) & 0xffff, ==,
                    PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    g_assert_cmphex(qtest_readl(qts, IP53_BCM_CFG + 0x10), ==, 0x02000000);
    qtest_writeq(qts, IP53_BCM_RUNTIME_PIC_HEAD0 + 0x208, 0x1020);
    g_assert_cmphex(qtest_readq(qts,
                    IP53_BCM_RUNTIME_PIC_HEAD0 + 0x208), ==, 0x1020);
    ip53_bcm_sram_write(qts, 0x100, IP53_BCM_TX_RING >> 32);
    ip53_bcm_sram_write(qts, 0x104, IP53_BCM_TX_RING & 0xffffffff);
    ip53_bcm_sram_write(qts, 0x108, 8U << 16);
    qtest_writel(qts, IP53_RUNTIME_BCM_BAR + 0x3c38,
                 IP53_BCM_STATUS >> 32);
    qtest_writel(qts, IP53_RUNTIME_BCM_BAR + 0x3c3c,
                 IP53_BCM_STATUS & 0xffffffff);
    qtest_memset(qts, IP53_BCM_STATUS_RAM, 0, 0x50);
    qtest_writel(qts, IP53_RUNTIME_BCM_BAR + 0x3c14, 1);
    qtest_writel(qts, IP53_RUNTIME_BCM_BAR + 0x3c00, 2);
    g_assert_cmphex(qtest_readl(qts,
                    IP53_RUNTIME_BCM_BAR + 0x3c00) & 2, ==, 2);
    qtest_writel(qts, IP53_RUNTIME_BCM_BAR + 0x0204, 0);
}

static void ip53_runtime_raise_bcm_irq(QTestState *qts, uint32_t producer)
{
    uint8_t descriptor[16] = { 0 };
    uint8_t descriptor_readback[sizeof(descriptor)];
    uint8_t frame[IP53_BCM_FRAME_SIZE];
    uint8_t status[0x50];
    size_t ring_offset;
    uint32_t pcistate, cpu_pcistate;
    uint64_t pic_status, pi_pending;
    unsigned int i;

    g_assert_cmpuint(producer, >, 0);
    ring_offset = ((producer - 1) % 8) * sizeof(descriptor);
    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = i * 7 + 1;
    }
    ip53_bcm_put32le(descriptor + 4, IP53_BCM_TX_BUFFER);
    ip53_bcm_put16le(descriptor, 8, 4);
    ip53_bcm_put16le(descriptor, 10, sizeof(frame));
    qtest_memwrite(qts, IP53_BCM_TX_RING_RAM + ring_offset, descriptor,
                   sizeof(descriptor));
    qtest_memread(qts, IP53_BCM_TX_RING_RAM + ring_offset,
                  descriptor_readback, sizeof(descriptor_readback));
    g_assert_cmpmem(descriptor_readback, sizeof(descriptor_readback),
                    descriptor, sizeof(descriptor));
    qtest_memwrite(qts, IP53_BCM_TX_BUFFER_RAM, frame, sizeof(frame));

    /* A prior write-one acknowledgement also leaves the mailbox masked. */
    qtest_writel(qts, IP53_RUNTIME_BCM_BAR + 0x0204, 0);
    pcistate = qtest_readl(qts, IP53_BCM_CFG + 0x70);
    g_assert_true(nasid_test_cpu_readl(qts, 0, IP53_BCM_CFG + 0x70,
                                       &cpu_pcistate));
    pic_status = qtest_readq(qts, IP53_RUNTIME_PIC_STATUS);
    pi_pending = qtest_readq(qts, IP53_PI_PENDING);
    g_test_message("BCM before producer=%u PCISTATE=%08x CPU=%08x "
                   "PIC status=%016" PRIx64 " PI pending=%016" PRIx64,
                   producer, pcistate, cpu_pcistate, pic_status,
                   pi_pending);
    g_assert_cmphex(pcistate & 2, !=, 0);
    g_assert_cmphex(cpu_pcistate & 2, !=, 0);
    qtest_writel(qts, IP53_RUNTIME_BCM_BAR + 0x0304, producer);
    g_assert_cmphex(qtest_readl(qts, IP53_RUNTIME_BCM_BAR + 0x0304), ==,
                    producer);
    qtest_memread(qts, IP53_BCM_STATUS_RAM, status, sizeof(status));
    g_assert_cmphex(ip53_bcm_get32le(status), ==, 1);
    g_assert_cmphex(ip53_bcm_get32le(status + 4), !=, 0);
    g_assert_cmphex(ip53_bcm_get16le(status + 18), ==, producer);
    pcistate = qtest_readl(qts, IP53_BCM_CFG + 0x70);
    g_assert_true(nasid_test_cpu_readl(qts, 0, IP53_BCM_CFG + 0x70,
                                       &cpu_pcistate));
    pic_status = qtest_readq(qts, IP53_RUNTIME_PIC_STATUS);
    pi_pending = qtest_readq(qts, IP53_PI_PENDING);
    g_test_message("BCM TX producer=%u PCISTATE=%08x CPU=%08x "
                   "PIC status=%016" PRIx64 " PI pending=%016" PRIx64,
                   producer, pcistate, cpu_pcistate, pic_status,
                   pi_pending);
    g_assert_cmphex(pcistate & 2, ==, 0);
    g_assert_cmphex(cpu_pcistate & 2, ==, 0);
}

static void ip53_runtime_clear_bcm_irq(QTestState *qts)
{
    qtest_writel(qts, IP53_RUNTIME_BCM_BAR + 0x0204, 1);
    g_assert_cmphex(qtest_readl(qts, IP53_BCM_CFG + 0x70) & 2, !=, 0);
}

static void test_ip53_io9_endpoint_interrupt_routes(void)
{
    const uint64_t enabled = (UINT64_C(1) << 0) |
                             (UINT64_C(1) << 2) |
                             (UINT64_C(1) << 3);
    IP53RuntimeTestFixture *fixture = ip53_runtime_test_start(
        "origin350,ip53-experimental=on", "-nic none");
    QTestState *qts = fixture->qts;
    g_autofree char *ioc4 = qom_child_type(qts, "/machine",
                                            "io9-io9-d1-f0");
    g_autofree char *isp = qom_child_type(qts, "/machine",
                                          "io9-io9-d3-f0");
    g_autofree char *bcm = qom_child_type(qts, "/machine",
                                          "io9-io9-d4-f0");
    uint64_t pending;

    g_assert_cmpstr(ioc4, ==, "sgi-ioc4");
    g_assert_cmpstr(isp, ==, "qlogic-isp12160");
    g_assert_cmpstr(bcm, ==, "bcm570x");

    qtest_writeq(qts, IP53_BCM_RUNTIME_PIC_HEAD0 + 0x20, 9);
    qtest_writeq(qts, IP53_BCM_RUNTIME_PIC_HEAD0 + 0x80,
                 UINT64_C(15) << 20);
    ip53_runtime_program_interrupt_routes(qts, enabled);
    ip53_runtime_configure_ioc4(qts);
    ip53_runtime_configure_isp(qts);
    ip53_runtime_configure_bcm(qts);

    ip53_runtime_raise_ioc4_irq(qts);
    ip53_runtime_assert_pic_input(qts, 0, IP53_RUNTIME_PI_LEVEL_IOC4);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    (ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_ISP) |
                     ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_BCM)), ==, 0);
    ip53_runtime_clear_ioc4_irq(qts);
    ip53_runtime_clear_pic_status(qts, 0);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_IOC4), ==, 0);

    ip53_runtime_raise_isp_irq(qts);
    ip53_runtime_assert_pic_input(qts, 2, IP53_RUNTIME_PI_LEVEL_ISP);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    (ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_IOC4) |
                     ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_BCM)), ==, 0);
    ip53_runtime_clear_isp_irq(qts);
    ip53_runtime_clear_pic_status(qts, 2);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_ISP), ==, 0);

    ip53_runtime_raise_bcm_irq(qts, 1);
    ip53_runtime_assert_pic_input(qts, 3, IP53_RUNTIME_PI_LEVEL_BCM);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    (ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_IOC4) |
                     ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_ISP)), ==, 0);
    ip53_runtime_clear_bcm_irq(qts);
    ip53_runtime_clear_pic_status(qts, 3);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_BCM), ==, 0);

    /* A disabled level input retains its source and delivers on re-enable. */
    qtest_writeq(qts, IP53_RUNTIME_PIC_ENABLE,
                 (UINT64_C(1) << 0) | (UINT64_C(1) << 3));
    ip53_runtime_raise_isp_irq(qts);
    g_assert_cmphex(qtest_readq(qts, IP53_RUNTIME_PIC_STATUS) &
                    (UINT64_C(1) << 2), !=, 0);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_ISP), ==, 0);
    qtest_writeq(qts, IP53_RUNTIME_PIC_ENABLE, enabled);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_ISP), !=, 0);
    ip53_runtime_clear_isp_irq(qts);
    ip53_runtime_clear_pic_status(qts, 2);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) &
                    ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_ISP), ==, 0);

    /* Reset with a real endpoint asserted, then prove the routes still work. */
    ip53_runtime_raise_ioc4_irq(qts);
    ip53_runtime_raise_isp_irq(qts);
    ip53_runtime_raise_bcm_irq(qts, 2);
    pending = (ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_IOC4) |
               ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_ISP) |
               ip53_runtime_pi_bit(IP53_RUNTIME_PI_LEVEL_BCM));
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) & pending, ==, pending);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(qts, IP53_RUNTIME_PIC_STATUS), ==, 0);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) & pending, ==, 0);
    g_assert_cmphex(qtest_readw(qts, IP53_RUNTIME_ISP_CPU_VIEW + 0x08) &
                    0x000c, ==, 0);
    g_assert_cmphex(qtest_readl(qts, IP53_BCM_CFG + 0x70) & 0x2, !=, 0);

    ip53_runtime_program_interrupt_routes(qts, enabled);
    ip53_runtime_configure_ioc4(qts);
    ip53_runtime_configure_isp(qts);
    ip53_runtime_configure_bcm(qts);
    ip53_runtime_raise_ioc4_irq(qts);
    ip53_runtime_raise_isp_irq(qts);
    ip53_runtime_raise_bcm_irq(qts, 1);
    g_assert_cmphex(qtest_readq(qts, IP53_RUNTIME_PIC_STATUS) & enabled, ==,
                    enabled);
    g_assert_cmphex(qtest_readq(qts, IP53_PI_PENDING) & pending, ==, pending);
    ip53_runtime_clear_ioc4_irq(qts);
    ip53_runtime_clear_isp_irq(qts);
    ip53_runtime_clear_bcm_irq(qts);
    ip53_runtime_clear_pic_status(qts, 0);
    ip53_runtime_clear_pic_status(qts, 2);
    ip53_runtime_clear_pic_status(qts, 3);
    ip53_runtime_test_stop(fixture);
}

static void test_ip53_runtime_pxbow_migration(const void *opaque)
{
    bool mismatch = g_str_equal(opaque, "revision4");
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp(
        "sn-builder-ip53-pxbow-migrate-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *extra = g_strdup(mismatch ?
        "-global sgi-pic.pxbow-revision=4 -incoming defer" :
        "-incoming defer");
    IP53RuntimeTestFixture *from, *to;
    const char *pxbow = "/machine/pic-pic/pxbow";
    const uint64_t pic_widget = IP53_IO_SPACE +
                                UINT64_C(15) * UINT64_C(0x1000000);
    static const uint8_t reset_ram_seed[] = {
        0x9a, 0x8b, 0x7c, 0x6d, 0x5e, 0x4f, 0x30, 0x21,
    };
    uint8_t reset_ram[sizeof(reset_ram_seed)];
    uint8_t reset_carrier[sizeof(reset_ram_seed)];
    uint8_t restored_ram[sizeof(reset_ram_seed)];
    uint8_t restored_carrier[sizeof(reset_ram_seed)];

    if (mismatch && !g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 30 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr("*IP53 assembly fingerprint mismatch*");
        return;
    }

    g_assert_no_error(error);
    from = ip53_runtime_test_start("origin350,ip53-experimental=on", NULL);
    to = ip53_runtime_test_start("origin350,ip53-experimental=on", extra);
    g_assert_cmpuint(qom_get_uint(from->qts, pxbow, "revision"), ==, 5);
    g_assert_cmpuint(qom_get_uint(to->qts, pxbow, "revision"), ==,
                     mismatch ? 4 : 5);

    if (!mismatch) {
        qtest_writeq(from->qts,
                     IP53_IO_SPACE + UINT64_C(0x1000000) + 0x780008,
                     UINT64_C(0x53));
        g_assert_cmphex(qtest_readq(
            from->qts, IP53_IO_SPACE + UINT64_C(0x1000000) + 0x780008), ==,
            UINT64_C(0x53));
        qtest_memwrite(from->qts, IP53_RESET_PROM, reset_ram_seed,
                       sizeof(reset_ram_seed));
        qtest_memread(from->qts, IP53_RESET_PROM, reset_ram,
                      sizeof(reset_ram));
        g_assert_cmpmem(reset_ram, sizeof(reset_ram), reset_ram_seed,
                        sizeof(reset_ram_seed));
        g_assert_true(qtest_memread_cpu_debug(
            from->qts, 0, UINT64_C(0xffffffffbfc00000), reset_carrier,
            sizeof(reset_carrier)));
        for (unsigned head = 0; head < 2; head++) {
            uint64_t pic_head = pic_widget + head * UINT64_C(0x800000);
            uint64_t destination = UINT64_C(0x1020304000000000) |
                                  ((uint64_t)head << 32) | 0x55667788;

            qtest_writeq(from->qts, pic_head + 0x130, destination);
            qtest_writel(from->qts, pic_head + 0x134, 0xc0ffee00 + head);
            qtest_writel(from->qts, pic_head + 0x214, 0x1010 + head);
        }
    }
    qtest_writel(from->qts, IP53_IO_SPACE + 0x7c, 0x5a);
    qtest_writel(from->qts, IP53_IO_SPACE + 0x94, 0xa5);
    qtest_writel(from->qts, IP53_IO_SPACE + 0xbc, 0x3c);
    g_assert_cmphex(qtest_readl(from->qts, IP53_IO_SPACE + 0x94), ==,
                    0xa5);
    g_assert_cmphex(qtest_readl(from->qts, IP53_IO_SPACE + 0xbc), ==,
                    0x3c);
    qtest_writel(from->qts, IP53_IO_SPACE + 0x1b4, 0);
    g_assert_cmpuint(qom_get_uint(from->qts, pxbow, "links-alive"), ==,
                     0x8000);

    if (mismatch) {
        qtest_set_expected_status(to->qts, EXIT_FAILURE);
    }
    qtest_qmp_assert_success(to->qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from->qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    if (mismatch) {
        int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

        while (qtest_probe_child(to->qts)) {
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(1000);
        }
        g_assert_false(qtest_probe_child(to->qts));
    } else {
        sgi_wait_migration_status(to->qts, "completed", 60);
        g_assert_cmphex(qtest_readq(
            to->qts, IP53_IO_SPACE + UINT64_C(0x1000000) + 0x780008), ==,
            UINT64_C(0x53));
        g_assert_cmphex(qtest_readl(to->qts, IP53_IO_SPACE + 0x7c), ==,
                        0x5a);
        qtest_memread(to->qts, IP53_RESET_PROM, restored_ram,
                      sizeof(restored_ram));
        g_assert_cmpmem(restored_ram, sizeof(restored_ram), reset_ram_seed,
                        sizeof(reset_ram_seed));
        g_assert_true(qtest_memread_cpu_debug(
            to->qts, 0, UINT64_C(0xffffffffbfc00000), restored_carrier,
            sizeof(restored_carrier)));
        g_assert_cmpmem(restored_carrier, sizeof(restored_carrier),
                        reset_carrier, sizeof(reset_carrier));
        g_assert_cmphex(qtest_readl(to->qts, IP53_IO_SPACE + 0x94), ==,
                        0xa5);
        g_assert_cmphex(qtest_readl(to->qts, IP53_IO_SPACE + 0xbc), ==,
                        0x3c);
        g_assert_cmpuint(qom_get_uint(to->qts, pxbow, "links-alive"), ==,
                         0x8000);
        qtest_qmp_assert_success(to->qts, "{'execute':'cont'}");
        qtest_clock_step(to->qts, 20);
        qtest_qmp_assert_success(to->qts, "{'execute':'stop'}");
        g_assert_cmpuint(qom_get_uint(to->qts, pxbow, "links-alive"), ==,
                         0x8400);
        for (unsigned head = 0; head < 2; head++) {
            uint64_t pic_head = pic_widget + head * UINT64_C(0x800000);
            uint64_t destination = 0;
            uint32_t device = 0;

            g_assert_true(nasid_test_cpu_readl(to->qts, 0,
                                                pic_head + 0x214, &device));
            g_assert_cmphex(device, ==, 0x1010 + head);
            g_assert_true(nasid_test_cpu_readq(to->qts, 0,
                                                pic_head + 0x130,
                                                &destination));
            g_assert_cmphex(destination, ==,
                            (UINT64_C(0x10203040) + head) << 32 |
                            (0xc0ffee00 + head));
        }
    }
    g_unlink(socket);
    g_rmdir(dir);
    ip53_runtime_test_stop(to);
    ip53_runtime_test_stop(from);
}

#define NASID_TEST_SPACE_HUB UINT64_C(0x10000000000)
#define NASID_TEST_HUB_LOCAL_REGISTERS \
    (NASID_TEST_SPACE_HUB + UINT64_C(0x10000000))

static void test_ip53_l1_reported_type(void)
{
    IP53RuntimeTestFixture *fixture = ip53_runtime_test_start(
        "origin350,topology=origin350-ip53-experiment,ip53-experimental=on",
        NULL);
    const uint64_t uart = NASID_TEST_HUB_LOCAL_REGISTERS + 0x80;
    const uint8_t request[] = { 0x10, 0, 4, 0x10, 6, 0, 3, 0 };
    const uint8_t expected[] = {
        0x30, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0x41, 0, 0, 0, 0, 0x37,
    };
    uint8_t reply[32];

    sgi_l1_test_send(fixture->qts, uart, request, sizeof(request), false);
    g_assert_cmpuint(sgi_l1_test_receive(fixture->qts, uart, reply,
                                         sizeof(reply)), ==, sizeof(expected));
    g_assert_cmpmem(reply, sizeof(expected), expected, sizeof(expected));
    ip53_runtime_test_stop(fixture);
}

static void test_ip53_legacy_composition_migration(void)
{
    const char *legacy_binary = g_getenv("QTEST_QEMU_BINARY_SRC");
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp(
        "sn-builder-ip53-legacy-migrate-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    IP53RuntimeTestFixture *from, *to;

    if (!legacy_binary) {
        g_test_skip("QTEST_QEMU_BINARY_SRC is needed for the legacy stream");
        return;
    }
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 30 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr("*IP53 assembly fingerprint mismatch*");
        return;
    }

    g_assert_no_error(error);
    from = ip53_runtime_test_start_with_binary(
        "QTEST_QEMU_BINARY_SRC", "origin350,ip53-experimental=on", NULL);
    to = ip53_runtime_test_start("origin350,ip53-experimental=on",
                                 "-incoming defer");
    {
        uint32_t status;

        g_assert_true(nasid_test_cpu_readl(from->qts, 0,
                                           IP53_IO_SPACE + 0x0c, &status));
        g_assert_cmphex((status >> 6) & 0xf, ==, 0);
    }
    qtest_writel(from->qts, IP53_IO_SPACE + 0x7c, 0x5a);
    qtest_writel(from->qts, IP53_IO_SPACE + 0x94, 0xa5);
    qtest_writel(from->qts, IP53_IO_SPACE + 0xbc, 0x3c);
    qtest_set_expected_status(to->qts, EXIT_FAILURE);
    qtest_qmp_assert_success(to->qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from->qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    {
        int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

        while (qtest_probe_child(to->qts)) {
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(1000);
        }
        g_assert_false(qtest_probe_child(to->qts));
    }
    if (qtest_probe_child(from->qts)) {
        qtest_qmp_assert_success(from->qts, "{'execute':'quit'}");
    }
    ip53_runtime_test_stop(to);
    ip53_runtime_test_stop(from);
    g_unlink(socket);
    g_rmdir(dir);
}

static void test_ip53_reset_policy_migration_peer(const void *opaque)
{
    bool prior_is_sender = g_str_equal(opaque, "prior-to-current");
    const char *prior_binary = g_getenv("QTEST_QEMU_BINARY_SRC");
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp(
        "sn-builder-ip53-reset-policy-migrate-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    IP53RuntimeTestFixture *from;
    IP53RuntimeTestFixture *to;

    if (!prior_binary || !g_file_test(prior_binary, G_FILE_TEST_IS_EXECUTABLE)) {
        g_test_skip("QTEST_QEMU_BINARY_SRC must name the retained pre-reset "
                    "policy executable");
        return;
    }
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 30 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr("*IP53 assembly fingerprint mismatch*");
        return;
    }

    g_assert_no_error(error);
    if (prior_is_sender) {
        from = ip53_runtime_test_start_with_binary(
            "QTEST_QEMU_BINARY_SRC", "origin350,ip53-experimental=on", NULL);
        to = ip53_runtime_test_start(
            "origin350,ip53-experimental=on", "-incoming defer");
    } else {
        from = ip53_runtime_test_start(
            "origin350,ip53-experimental=on", NULL);
        to = ip53_runtime_test_start_with_binary(
            "QTEST_QEMU_BINARY_SRC", "origin350,ip53-experimental=on",
            "-incoming defer");
    }
    qtest_set_expected_status(to->qts, EXIT_FAILURE);
    qtest_qmp_assert_success(to->qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from->qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    {
        int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

        while (qtest_probe_child(to->qts)) {
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(1000);
        }
        g_assert_false(qtest_probe_child(to->qts));
    }
    if (qtest_probe_child(from->qts)) {
        qtest_qmp_assert_success(from->qts, "{'execute':'quit'}");
    }
    ip53_runtime_test_stop(to);
    ip53_runtime_test_stop(from);
    g_unlink(socket);
    g_rmdir(dir);
}

static void test_ip53_previous_cpu_pio_migration(void)
{
    const char *legacy_binary = g_getenv("QTEST_QEMU_BINARY_SRC");
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp(
        "sn-builder-ip53-previous-pio-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    IP53RuntimeTestFixture *from, *to;
    uint64_t pic_head = IP53_IO_SPACE + UINT64_C(15) * UINT64_C(0x1000000);
    uint64_t io_registers = IP53_IO_SPACE + UINT64_C(0x1400000);
    uint64_t ioc4_config = pic_head + UINT64_C(0x21000);
    uint64_t ioc4_memory = IP53_IO_SPACE + UINT64_C(0x21000000);
    uint64_t ioc4_short = pic_head + UINT64_C(0x600000);
    uint64_t itte = (UINT64_C(15) << 8) | 2;
    uint32_t pio_value;
    uint32_t raw_value;

    if (!legacy_binary) {
        g_test_skip("QTEST_QEMU_BINARY_SRC is needed for the previous binary");
        return;
    }
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 30 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr("*IP53 assembly fingerprint mismatch*");
        return;
    }

    g_assert_no_error(error);
    from = ip53_runtime_test_start_with_binary(
        "QTEST_QEMU_BINARY_SRC", "origin350,ip53-experimental=on", NULL);
    to = ip53_runtime_test_start("origin350,ip53-experimental=on",
                                 "-incoming defer");

    /* Establish the previous binary's CPU-visible IOC4 PIO placement. */
    qtest_writel(from->qts, ioc4_config + 4, PCI_COMMAND_MEMORY);
    qtest_writel(from->qts, ioc4_config + 0x10, 0x01000000);
    g_assert_cmphex(qtest_readl(from->qts, ioc4_config + 4) & 0xffff, ==,
                    PCI_COMMAND_MEMORY);
    g_assert_cmphex(qtest_readl(from->qts, ioc4_config + 0x10), ==,
                    0x01000000);
    qtest_writeq(from->qts, pic_head + 0x210, 0x1010);
    g_assert_cmphex(qtest_readq(from->qts, pic_head + 0x210), ==, 0x1010);
    qtest_writeq(from->qts, io_registers + 0x160, itte);
    g_assert_cmphex(qtest_readq(from->qts, io_registers + 0x160), ==, itte);
    qtest_writel(from->qts, ioc4_memory + 0x10, bswap32(0x40));
    raw_value = qtest_readl(from->qts, ioc4_memory + 0x10);
    g_assert_cmphex(raw_value, ==, bswap32(0x40));
    g_assert_true(nasid_test_cpu_readl(from->qts, 0,
                                       ioc4_short + 0x10, &pio_value));
    g_assert_cmphex(pio_value, ==, bswap32(0x40));
    g_assert_cmphex(pio_value, ==, raw_value);

    qtest_set_expected_status(to->qts, EXIT_FAILURE);
    qtest_qmp_assert_success(to->qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from->qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    {
        int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

        while (qtest_probe_child(to->qts)) {
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(1000);
        }
        g_assert_false(qtest_probe_child(to->qts));
    }
    if (qtest_probe_child(from->qts)) {
        qtest_qmp_assert_success(from->qts, "{'execute':'quit'}");
    }
    ip53_runtime_test_stop(to);
    ip53_runtime_test_stop(from);
    g_unlink(socket);
    g_rmdir(dir);
}

static void test_ip53_previous_direct64_migration(void)
{
    const char *legacy_binary = g_getenv("QTEST_QEMU_BINARY_SRC");
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp(
        "sn-builder-ip53-previous-direct64-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    IP53RuntimeTestFixture *from, *to;

    if (!legacy_binary) {
        g_test_skip("QTEST_QEMU_BINARY_SRC is needed for the "
                    "pre-direct64 binary");
        return;
    }
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 30 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr("*IP53 assembly fingerprint mismatch*");
        return;
    }

    g_assert_no_error(error);
    from = ip53_runtime_test_start_with_binary(
        "QTEST_QEMU_BINARY_SRC", "origin350,ip53-experimental=on",
        NULL);
    to = ip53_runtime_test_start("origin350,ip53-experimental=on",
                                 "-incoming defer");
    qtest_set_expected_status(to->qts, EXIT_FAILURE);
    qtest_qmp_assert_success(to->qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from->qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    {
        int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

        while (qtest_probe_child(to->qts)) {
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(1000);
        }
        g_assert_false(qtest_probe_child(to->qts));
    }
    if (qtest_probe_child(from->qts)) {
        qtest_qmp_assert_success(from->qts, "{'execute':'quit'}");
    }
    ip53_runtime_test_stop(to);
    ip53_runtime_test_stop(from);
    g_unlink(socket);
    g_rmdir(dir);
}

static void test_ip53_widget_zero_pxbow_identity(void)
{
    IP53RuntimeTestFixture *fixture = ip53_runtime_test_start(
        "origin350,ip53-experimental=on", NULL);
    const char *pxbow = "/machine/pic-pic/pxbow";
    uint64_t pic_widget = IP53_IO_SPACE + UINT64_C(15) * UINT64_C(0x1000000);
    uint32_t identity = 0;
    uint64_t pic_head0 = 0;
    uint64_t pic_head1 = 0;
    uint64_t pic_control = 0;

    g_assert_true(nasid_test_cpu_readl(fixture->qts, 0, IP53_IO_SPACE + 4,
                                       &identity));
    g_assert_cmphex((identity >> 12) & 0xffff, ==, 0xd100);
    g_assert_cmphex((identity >> 1) & 0x7ff, ==, 0);
    g_assert_cmphex(identity & 1, ==, 1);
    g_assert_cmphex((identity >> 28) & 0xf, ==, 5);
    g_assert_cmpuint(qom_get_uint(fixture->qts, pxbow, "revision"), ==, 5);
    g_assert_cmpuint(qom_get_uint(fixture->qts, pxbow, "part"), ==, 0xd100);
    g_assert_cmpuint(qom_get_uint(fixture->qts, pxbow, "manufacturer"), ==,
                     0);
    g_assert_cmpuint(qom_get_uint(fixture->qts, pxbow, "links-present"), ==,
                     0x8400);
    g_assert_cmpuint(qom_get_uint(fixture->qts, pxbow,
                                 "startup-links-alive"), ==, 0x8400);
    g_assert_cmpuint(qom_get_uint(fixture->qts, pxbow,
                                 "link-reset-restore-alive"), ==, 0x8400);
    g_assert_cmpuint(qom_get_uint(fixture->qts, pxbow, "links-alive"), ==,
                     0x8400);
    g_assert_true(nasid_test_cpu_readq(fixture->qts, 0, pic_widget,
                                       &pic_head0));
    g_assert_true(nasid_test_cpu_readq(fixture->qts, 0,
                                       pic_widget + UINT64_C(0x800000),
                                       &pic_head1));
    g_assert_cmphex((pic_head0 >> 28) & 0xf, ==, 3);
    g_assert_cmphex((pic_head0 >> 12) & 0xffff, ==, 0xd102);
    g_assert_cmphex((pic_head0 & 0xffe) >> 1, ==, 0x24);
    g_assert_cmphex((pic_head1 >> 28) & 0xf, ==, 3);
    g_assert_cmphex((pic_head1 >> 12) & 0xffff, ==, 0xd112);
    g_assert_cmphex((pic_head1 & 0xffe) >> 1, ==, 0x24);

    qtest_writeq(fixture->qts, pic_widget + 0x20, 0x0a);
    qtest_writeq(fixture->qts, pic_widget + UINT64_C(0x800000) + 0x20, 0x05);
    g_assert_true(nasid_test_cpu_readq(fixture->qts, 0, pic_widget + 0x20,
                                       &pic_control));
    g_assert_cmphex(pic_control, ==, 0x0a);
    g_assert_true(nasid_test_cpu_readq(fixture->qts, 0,
                                       pic_widget + UINT64_C(0x800000) + 0x20,
                                       &pic_control));
    g_assert_cmphex(pic_control, ==, 0x05);

    {
        static const struct {
            const char *property;
            uint64_t value;
            uint64_t attempted;
        } properties[] = {
            { "pxbow-host-widget", 10, 11 },
            { "pxbow-pic-widget", 15, 14 },
            { "pxbow-revision", 5, 4 },
        };
        size_t i;

        for (i = 0; i < G_N_ELEMENTS(properties); i++) {
            g_autoptr(QDict) failure = qtest_qmp_assert_failure_ref(
                fixture->qts,
                "{'execute':'qom-set','arguments':{'path':'/machine/pic-pic',"
                "'property':%s,'value':%" PRIu64 "}}",
                properties[i].property, properties[i].attempted);

            g_assert_nonnull(failure);
            g_assert_cmpuint(qom_get_uint(fixture->qts, "/machine/pic-pic",
                                           properties[i].property), ==,
                             properties[i].value);
        }
    }

    ip53_runtime_test_stop(fixture);
}

static void test_ip53_widget_zero_status_source(void)
{
    IP53RuntimeTestFixture *fixture = ip53_runtime_test_start(
        "origin350,ip53-experimental=on", NULL);
    uint32_t status = 0;

    /* Seed widget-zero status, then read it through host ingress 10. */
    (void)qtest_readl(fixture->qts, IP53_IO_SPACE + 0x1004);
    g_assert_true(nasid_test_cpu_readl(fixture->qts, 0,
                                       IP53_IO_SPACE + 0x0c, &status));
    g_assert_cmphex((status >> 6) & 0xf, ==, 10);

    g_assert_true(nasid_test_cpu_readl(fixture->qts, 0,
                                       IP53_IO_SPACE + 0x54, &status));
    g_assert_cmphex((status >> 6) & 0xf, ==, 10);
    ip53_runtime_test_stop(fixture);
}

static void test_ip53_pxbow_route_fences_and_interrupt(void)
{
    IP53RuntimeTestFixture *fixture = ip53_runtime_test_start(
        "origin350,ip53-experimental=on", NULL);
    const char *pxbow = "/machine/pic-pic/pxbow";
    uint64_t pic_widget = IP53_IO_SPACE + UINT64_C(15) * UINT64_C(0x1000000);
    uint64_t packet_widget = IP53_IO_SPACE + UINT64_C(10) * UINT64_C(0x1000000);
    uint64_t value = 0;
    uint64_t destination_value = 0;
    uint32_t device_value = 0;
    uint32_t status;
    uint32_t upper = (UINT32_C(0x34) << 24) | (10 << 16) |
                     (uint32_t)((IP53_PI_INTERRUPT_ADDRESS >> 32) & 0xffff);
    uint32_t lower = (uint32_t)IP53_PI_INTERRUPT_ADDRESS;

    /*
     * Non-identity word writes must reach the PIC CPU face on both heads.
     * Widget identity also answers on the packet face, so it cannot prove
     * that this processor route reached the selected endpoint.
     */
    for (unsigned head = 0; head < 2; head++) {
        uint64_t pic_head = pic_widget + head * UINT64_C(0x800000);
        uint64_t destination = UINT64_C(0x1234567800000000) |
                              ((uint64_t)head << 32) | 0x55667788;
        uint64_t observed_destination = 0;
        uint32_t device = 0x1010 + head;
        uint32_t observed_device = 0;

        qtest_writeq(fixture->qts, pic_head + 0x130, destination);
        qtest_writel(fixture->qts, pic_head + 0x134, 0xaabbcc00 + head);
        qtest_writel(fixture->qts, pic_head + 0x214, device);
        g_assert_true(nasid_test_cpu_readl(fixture->qts, 0,
                                            pic_head + 0x214,
                                            &observed_device));
        g_assert_cmphex(observed_device, ==, device);
        g_assert_true(nasid_test_cpu_readq(fixture->qts, 0,
                                            pic_head + 0x130,
                                            &observed_destination));
        g_assert_cmphex(observed_destination, ==,
                        (destination & UINT64_C(0xffffffff00000000)) |
                        (0xaabbcc00 + head));
    }

    /* The experiment exposes revision-5 fences through the CPU widget-zero view. */
    qtest_writel(fixture->qts, IP53_IO_SPACE + 0x7c, 0x5a);
    qtest_writel(fixture->qts, IP53_IO_SPACE + 0x94, 0xa5);
    qtest_writel(fixture->qts, IP53_IO_SPACE + 0xbc, 0x3c);
    g_assert_cmphex(qtest_readl(fixture->qts, IP53_IO_SPACE + 0x7c), ==,
                    0x5a);
    g_assert_cmphex(qtest_readl(fixture->qts, IP53_IO_SPACE + 0x94), ==,
                    0xa5);
    g_assert_cmphex(qtest_readl(fixture->qts, IP53_IO_SPACE + 0xbc), ==,
                    0x3c);

    /* Widget-zero access errors raise a routed packet to Bedrock's PI bank. */
    qtest_writel(fixture->qts, IP53_IO_SPACE + 0x34, upper);
    qtest_writel(fixture->qts, IP53_IO_SPACE + 0x3c, lower);
    qtest_writel(fixture->qts, IP53_IO_SPACE + 0x24, 1 << 5);
    g_assert_cmphex(qtest_readq(fixture->qts, IP53_PI_PENDING) &
                    IP53_PI_PENDING_BIT, ==, 0);
    (void)qtest_readl(fixture->qts, IP53_IO_SPACE + 0x1004);
    status = qtest_readl(fixture->qts, IP53_IO_SPACE + 0x0c);
    g_assert_cmphex(status & (1 << 23), ==, 1 << 23);
    g_assert_cmphex(qtest_readq(fixture->qts, IP53_PI_PENDING) &
                    IP53_PI_PENDING_BIT, ==, IP53_PI_PENDING_BIT);
    (void)qtest_readl(fixture->qts, IP53_IO_SPACE + 0x54);
    g_assert_cmphex(qtest_readq(fixture->qts, IP53_PI_PENDING) &
                    IP53_PI_PENDING_BIT, ==, 0);

    /* Widget 10 is the packet face; the CPU PIO window must refuse it. */
    g_assert_false(nasid_test_cpu_readq(fixture->qts, 0,
                                        packet_widget + 0x20, &value));

    /* The reset pulse drops status but keeps the configured route carrying. */
    g_assert_true(nasid_test_cpu_readq(fixture->qts, 0, pic_widget + 0x20,
                                       &value));
    g_assert_cmphex(value, ==, 0);
    qtest_writel(fixture->qts, IP53_IO_SPACE + 0x1b4, 0);
    g_assert_cmpuint(qom_get_uint(fixture->qts, pxbow, "links-alive"), ==,
                     0x8000);
    g_assert_true(nasid_test_cpu_readq(fixture->qts, 0, pic_widget + 0x20,
                                       &value));
    qtest_qmp_assert_success(fixture->qts, "{'execute':'cont'}");
    qtest_clock_step(fixture->qts, 20);
    qtest_qmp_assert_success(fixture->qts, "{'execute':'stop'}");
    g_assert_true(nasid_test_cpu_readq(fixture->qts, 0, pic_widget + 0x20,
                                       &value));
    g_assert_cmphex(value, ==, 0);
    g_assert_cmpuint(qom_get_uint(fixture->qts, pxbow, "links-alive"), ==,
                     0x8400);

    qtest_qmp_assert_success(fixture->qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'links-alive','value':%u}}", pxbow, 0x8000);
    qtest_writel(fixture->qts, pic_widget + 0x214, 0x101f);
    qtest_writel(fixture->qts, pic_widget + 0x134, 0xdeadbeef);
    g_assert_false(nasid_test_cpu_readl(fixture->qts, 0,
                                        pic_widget + 0x214, &device_value));
    g_assert_false(nasid_test_cpu_readq(fixture->qts, 0,
                                        pic_widget + 0x130,
                                        &destination_value));
    g_assert_false(nasid_test_cpu_readq(fixture->qts, 0, pic_widget + 0x20,
                                        &value));
    qtest_qmp_assert_success(fixture->qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'links-alive','value':%u}}", pxbow, 0x8400);
    g_assert_true(nasid_test_cpu_readq(fixture->qts, 0, pic_widget + 0x20,
                                       &value));
    for (unsigned head = 0; head < 2; head++) {
        uint64_t pic_head = pic_widget + head * UINT64_C(0x800000);

        g_assert_true(nasid_test_cpu_readl(fixture->qts, 0,
                                            pic_head + 0x214, &device_value));
        g_assert_cmphex(device_value, ==, 0x1010 + head);
        g_assert_true(nasid_test_cpu_readq(fixture->qts, 0,
                                            pic_head + 0x130,
                                            &destination_value));
        g_assert_cmphex(destination_value, ==,
                        (UINT64_C(0x12345678) + head) << 32 |
                        (0xaabbcc00 + head));
    }

    qtest_writel(fixture->qts, IP53_IO_SPACE + 0x2f4, 0);
    g_assert_cmpuint(qom_get_uint(fixture->qts, pxbow, "links-alive"), ==,
                     0x400);
    g_assert_true(nasid_test_cpu_readq(fixture->qts, 0, pic_widget + 0x20,
                                       &value));
    qtest_qmp_assert_success(fixture->qts, "{'execute':'cont'}");
    qtest_clock_step(fixture->qts, 20);
    qtest_qmp_assert_success(fixture->qts, "{'execute':'stop'}");
    g_assert_true(nasid_test_cpu_readq(fixture->qts, 0, pic_widget + 0x20,
                                       &value));
    g_assert_cmphex(value, ==, 0);
    g_assert_cmpuint(qom_get_uint(fixture->qts, pxbow, "links-alive"), ==,
                     0x8400);
    qtest_qmp_assert_success(fixture->qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'links-alive','value':%u}}", pxbow, 0x400);
    qtest_writel(fixture->qts, pic_widget + 0x214, 0x101f);
    qtest_writel(fixture->qts, pic_widget + 0x134, 0xdeadbeef);
    g_assert_false(nasid_test_cpu_readl(fixture->qts, 0,
                                        pic_widget + 0x214, &device_value));
    g_assert_false(nasid_test_cpu_readq(fixture->qts, 0,
                                        pic_widget + 0x130,
                                        &destination_value));
    g_assert_false(nasid_test_cpu_readq(fixture->qts, 0, pic_widget + 0x20,
                                        &value));
    qtest_qmp_assert_success(fixture->qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'links-alive','value':%u}}", pxbow, 0x8400);
    g_assert_true(nasid_test_cpu_readq(fixture->qts, 0, pic_widget + 0x20,
                                       &value));
    for (unsigned head = 0; head < 2; head++) {
        uint64_t pic_head = pic_widget + head * UINT64_C(0x800000);

        g_assert_true(nasid_test_cpu_readl(fixture->qts, 0,
                                            pic_head + 0x214, &device_value));
        g_assert_cmphex(device_value, ==, 0x1010 + head);
        g_assert_true(nasid_test_cpu_readq(fixture->qts, 0,
                                            pic_head + 0x130,
                                            &destination_value));
        g_assert_cmphex(destination_value, ==,
                        (UINT64_C(0x12345678) + head) << 32 |
                        (0xaabbcc00 + head));
    }

    qtest_qmp_assert_success(fixture->qts, "{'execute':'system_reset'}");
    g_assert_cmpuint(qom_get_uint(fixture->qts, pxbow, "links-alive"), ==,
                     0x8400);
    g_assert_cmpuint(qom_get_uint(fixture->qts, "/machine/pic-pic",
                                  "pxbow-host-widget"), ==, 10);
    g_assert_cmpuint(qom_get_uint(fixture->qts, "/machine/pic-pic",
                                  "pxbow-pic-widget"), ==, 15);
    for (unsigned head = 0; head < 2; head++) {
        uint64_t pic_head = pic_widget + head * UINT64_C(0x800000);

        g_assert_true(nasid_test_cpu_readl(fixture->qts, 0,
                                            pic_head + 0x214, &device_value));
        g_assert_cmphex(device_value, ==, 0);
        g_assert_true(nasid_test_cpu_readq(fixture->qts, 0,
                                            pic_head + 0x130,
                                            &destination_value));
        g_assert_cmphex(destination_value, ==, 0);
        qtest_writel(fixture->qts, pic_head + 0x214, 0x1010 + head);
        qtest_writeq(fixture->qts, pic_head + 0x130,
                     UINT64_C(0xabcddcba00000000));
        qtest_writel(fixture->qts, pic_head + 0x134, 0x55667700 + head);
        g_assert_true(nasid_test_cpu_readl(fixture->qts, 0,
                                            pic_head + 0x214, &device_value));
        g_assert_cmphex(device_value, ==, 0x1010 + head);
        g_assert_true(nasid_test_cpu_readq(fixture->qts, 0,
                                            pic_head + 0x130,
                                            &destination_value));
        g_assert_cmphex(destination_value, ==,
                        UINT64_C(0xabcddcba00000000) |
                        (0x55667700 + head));
    }
    ip53_runtime_test_stop(fixture);
}

static void test_ip53_pxbow_pic_memory_windows(void)
{
    IP53RuntimeTestFixture *fixture = ip53_runtime_test_start(
        "origin350,ip53-experimental=on",
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=2 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1");
    uint64_t pic_widget = IP53_IO_SPACE + UINT64_C(15) * UINT64_C(0x1000000);
    uint64_t io_registers = IP53_IO_SPACE + UINT64_C(0x1400000);
    const uint64_t pci_offset = UINT64_C(0x01000000);
    const uint32_t pattern[] = { 0xa1b2c3d4, 0x5e6f7081 };
    const uint32_t cfg[] = { 0x22000, 0x21000 };
    const uint64_t window[] = { UINT64_C(0x40000000),
                                UINT64_C(0xc0000000) };

    for (unsigned head = 0; head < G_N_ELEMENTS(pattern); head++) {
        uint64_t pic_head = pic_widget + head * UINT64_C(0x800000);
        uint64_t pci_config = pic_head + cfg[head];
        uint64_t pci_register = IP53_IO_SPACE + UINT64_C(0x20000000) +
                                pci_offset + 0x24;
        uint64_t short_register = pic_head + 0x600000 + 0x24;
        uint64_t itte = (UINT64_C(15) << 8) | (window[head] >> 29);
        uint32_t value = 0;
        uint32_t short_device = 0x1010;
        uint32_t direct_pattern = pattern[head] ^ UINT32_C(0x0f0f00ff);

        qtest_writel(fixture->qts, pci_config + 4, PCI_COMMAND_MEMORY);
        qtest_writel(fixture->qts, pci_config + 0x10, pci_offset);
        qtest_writel(fixture->qts, pic_head + 0x214, short_device);
        g_assert_true(nasid_test_cpu_readl(fixture->qts, 0,
                                            pic_head + 0x214, &value));
        g_assert_cmphex(value, ==, short_device);
        qtest_writeq(fixture->qts, io_registers + 0x160, itte);

        /* BAR0 +0x24 is lane scratch; cross-reads tie both routes to it. */
        qtest_writel(fixture->qts, short_register, pattern[head]);
        g_assert_true(nasid_test_cpu_readl(fixture->qts, 0, pci_register,
                                           &value));
        g_assert_cmphex(value, ==, pattern[head]);
        qtest_writel(fixture->qts, pci_register, direct_pattern);
        g_assert_true(nasid_test_cpu_readl(fixture->qts, 0, short_register,
                                           &value));
        g_assert_cmphex(value, ==, direct_pattern);
    }
    ip53_runtime_test_stop(fixture);
}

static void test_ip53_runtime_cpu_property_refusals(void)
{
    static const struct {
        const char *cpu;
        const char *message;
    } cases[] = {
        { "R16000,r16000-experimental=off,"
          "r16000-reset-mode=0x009cad85,r16000-scache-bytes=0x800000",
          "IP53 CPU property r16000-experimental conflicts with the "
          "authorized descriptor" },
        { "R16000,r16000-experimental=on,"
          "r16000-reset-mode=0x009cad84,r16000-scache-bytes=0x800000",
          "IP53 CPU property r16000-reset-mode conflicts with the "
          "authorized descriptor" },
        { "R16000,r16000-experimental=on,"
          "r16000-reset-mode=0x009cad85,r16000-scache-bytes=0x400000",
          "IP53 CPU property r16000-scache-bytes conflicts with the "
          "authorized descriptor" },
    };
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *blank = g_malloc0(IP53_TEST_PROM_SIZE);
    g_autofree char *bios = NULL;
    g_autofree char *data_path = NULL;
    g_autofree char *clock_path = NULL;
    int bios_fd, data_fd, clock_fd;

    bios_fd = g_file_open_tmp("sn-builder-origin350-ip53-refuse-prom-XXXXXX.bin",
                              &bios, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(bios_fd, blank, IP53_TEST_PROM_SIZE), ==,
                    IP53_TEST_PROM_SIZE);
    g_assert_cmpint(close(bios_fd), ==, 0);
    data_fd = g_file_open_tmp("sn-builder-origin350-ip53-refuse-data-XXXXXX",
                              &data_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(data_fd, 0x800), ==, 0);
    g_assert_cmpint(close(data_fd), ==, 0);
    clock_fd = g_file_open_tmp("sn-builder-origin350-ip53-refuse-clock-XXXXXX",
                               &clock_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(clock_fd, 16), ==, 0);
    g_assert_cmpint(close(clock_fd), ==, 0);

    for (unsigned int i = 0; i < G_N_ELEMENTS(cases); i++) {
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M",
            "origin350,ip53-experimental=on", "-m", "2G", "-smp", "4",
            "-cpu", cases[i].cpu, "-bios", bios,
            "-drive", NULL, "-drive", NULL, "-display", "none", NULL,
        };
        g_autofree char *data_drive = g_strdup_printf(
            "if=none,id=sgi-ds1742-data-io9,file=%s,format=raw", data_path);
        g_autofree char *clock_drive = g_strdup_printf(
            "if=none,id=sgi-ds1742-clock-io9,file=%s,format=raw", clock_path);

        args[12] = data_drive;
        args[14] = clock_drive;
        assert_qemu_rejects(args, cases[i].message);
    }
    g_unlink(bios);
    g_unlink(data_path);
    g_unlink(clock_path);
}

static void test_ip53_fixture_refused_without_qtest(void)
{
    g_autofree char *data_path = create_sized_temp_file(
        "sn-builder-fixture-refusal-data-XXXXXX", 0x800);
    g_autofree char *clock_path = create_sized_temp_file(
        "sn-builder-fixture-refusal-clock-XXXXXX", 16);
    g_autofree char *data_drive = NULL;
    g_autofree char *clock_drive = NULL;
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M",
        "sn-builder-bedrock-test,construction-fault=ip53-pic-io9",
        "-m", "64M", "-smp", "1", "-cpu", "R14000", "-nodefaults",
        "-drive", NULL, "-drive", NULL, "-display", "none", "-S", NULL,
    };

    data_drive = g_strdup_printf(
        "if=none,id=sgi-ds1742-data-fixture-io9,file=%s,format=raw",
        data_path);
    clock_drive = g_strdup_printf(
        "if=none,id=sgi-ds1742-clock-fixture-io9,file=%s,format=raw",
        clock_path);
    args[11] = data_drive;
    args[13] = clock_drive;
    assert_qemu_rejects(args, "IP53 construction requires the runtime "
                              "experiment permission or a qtest fixture");
    g_unlink(data_path);
    g_unlink(clock_path);
}

static void test_ip53_missing_profile_refused_in_qtest(void)
{
    g_autofree char *data_path = create_sized_temp_file(
        "sn-builder-missing-profile-data-XXXXXX", 0x800);
    g_autofree char *clock_path = create_sized_temp_file(
        "sn-builder-missing-profile-clock-XXXXXX", 16);
    g_autofree char *data_drive = g_strdup_printf(
        "if=none,id=sgi-ds1742-data-fixture-io9,file=%s,format=raw",
        data_path);
    g_autofree char *clock_drive = g_strdup_printf(
        "if=none,id=sgi-ds1742-clock-fixture-io9,file=%s,format=raw",
        clock_path);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M",
        "sn-builder-bedrock-test,construction-fault=ip53-missing-profile",
        "-m", "64M", "-smp", "1", "-cpu", "R14000", "-nodefaults",
        "-drive", NULL, "-drive", NULL, "-qtest", "stdio", "-accel",
        "qtest", "-display", "none", "-S", NULL,
    };

    args[11] = data_drive;
    args[13] = clock_drive;
    assert_qemu_rejects(args, "PCI host 'fixture-pic' has no compatibility "
                              "profile");
    g_unlink(data_path);
    g_unlink(clock_path);
}

static void test_ip53_invalid_position_refused_in_qtest(const void *data)
{
    const char *fault = data;
    g_autofree char *machine = g_strdup_printf(
        "sn-builder-bedrock-test,construction-fault=%s", fault);
    g_autofree char *data_path = create_sized_temp_file(
        "sn-builder-invalid-position-data-XXXXXX", 0x800);
    g_autofree char *clock_path = create_sized_temp_file(
        "sn-builder-invalid-position-clock-XXXXXX", 16);
    g_autofree char *data_drive = g_strdup_printf(
        "if=none,id=sgi-ds1742-data-fixture-io9,file=%s,format=raw",
        data_path);
    g_autofree char *clock_drive = g_strdup_printf(
        "if=none,id=sgi-ds1742-clock-fixture-io9,file=%s,format=raw",
        clock_path);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", machine, "-m", "64M", "-smp", "1",
        "-cpu", "R14000", "-nodefaults", "-drive", NULL, "-drive", NULL,
        "-qtest", "stdio", "-accel", "qtest", "-display", "none", "-S",
        NULL,
    };

    args[11] = data_drive;
    args[13] = clock_drive;
    assert_qemu_rejects(args, "IO9 board 'fixture-io9' has incomplete PCI "
                              "position or route");
    g_unlink(data_path);
    g_unlink(clock_path);
}

/*
 * sn-builder-bedrock-nasid-test (sn-builder-fault-test.c) builds two
 * sgi-sn1-node devices directly, at placements 0 and 1, the way
 * sn-machine.c's own per-node loop would, with no cable. Each node's own
 * processor must read its own board-identification register through the Hub
 * special local window, one of the reader-relative forms, and node 0's
 * processor must reach nothing at node field 1: node 1 carries number 0 from
 * reset and is not cabled. Collided placements would abort realize, and a
 * local form shared between the nodes would answer only one of them.
 */
#define NASID_TEST_BOARD_ID_LOCAL \
    (NASID_TEST_HUB_LOCAL_REGISTERS + UINT64_C(0x4000000))
#define NASID_TEST_BOARD_ID_VALUE UINT64_C(0x8001)
/* M mode's node field starts at physical address bit 33. */
#define NASID_TEST_NODE_ADDRESS_SHIFT 33

static bool nasid_test_cpu_readl(QTestState *qts, unsigned int cpu,
                                 uint64_t address, uint32_t *value)
{
    g_autofree char *command = g_strdup_printf("xp/1wx 0x%" PRIx64, address);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'human-monitor-command',"
        "  'arguments': { 'command-line': %s, 'cpu-index': %u } }",
        command, cpu);
    const char *text = qdict_get_str(response, "return");
    const char *colon = strchr(text, ':');
    unsigned int parsed;

    if (strstr(text, "Cannot access memory") || !colon ||
        sscanf(colon + 1, "%x", &parsed) != 1) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool nasid_test_cpu_readq(QTestState *qts, unsigned int cpu,
                                 uint64_t address, uint64_t *value)
{
    g_autofree char *command = g_strdup_printf("xp/1gx 0x%" PRIx64, address);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'human-monitor-command',"
        "  'arguments': { 'command-line': %s, 'cpu-index': %u } }",
        command, cpu);
    const char *text = qdict_get_str(response, "return");
    const char *colon = strchr(text, ':');

    if (strstr(text, "Cannot access memory") || !colon) {
        return false;
    }
    return sscanf(colon + 1, "%" SCNx64, value) == 1;
}

static void test_bedrock_nodes_answer_at_their_own_nasid_window(
    const void *data)
{
    /* -nodefaults: no L1 console backend is needed for addressing alone. */
    QTestState *qts = qtest_init(
        "-M sn-builder-bedrock-nasid-test,activate=on -cpu R14000 "
        "-smp 2 -nodefaults -display none");
    uint64_t node1_offset = UINT64_C(1) << NASID_TEST_NODE_ADDRESS_SHIFT;
    uint64_t value = 0;
    unsigned int cpu;

    for (cpu = 0; cpu < 2; cpu++) {
        g_assert_true(nasid_test_cpu_readq(qts, cpu,
                                           NASID_TEST_BOARD_ID_LOCAL,
                                           &value));
        g_assert_cmphex(value, ==, NASID_TEST_BOARD_ID_VALUE);
    }
    g_assert_false(nasid_test_cpu_readq(qts, 0,
                                        NASID_TEST_BOARD_ID_LOCAL +
                                        node1_offset, &value));

    qtest_quit(qts);
}

/*
 * Each chassis's L1 takes an even share of every identity-record list, in
 * node order; a list that does not divide evenly names no one chassis's
 * parts, so the builder refuses it rather than giving it whole to both.
 */
static void test_origin300_two_chassis_refuses_an_uneven_record_list(
    const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M",
        "origin300,topology=origin300-two-chassis,"
        "chassis-eeprom.0=/nonexistent/a,chassis-eeprom.1=/nonexistent/b,"
        "chassis-eeprom.2=/nonexistent/c",
        "-cpu", "R14000", "-smp", "4", "-m", "1024",
        "-nodefaults", "-display", "none", NULL
    };

    assert_qemu_rejects(args, "chassis-eeprom lists 3 files, which do not "
                        "divide evenly across the 2 chassis L1s");
}

/*
 * The cable between the two chassis also carries their L1s' RS-422 pair, so
 * the builder makes each chassis's L1 the other's peer when it wires the
 * cable.
 */
#define EXPANSION_LB_LOCAL (SPACE_IO + UINT64_C(0x01000000))
#define EXPANSION_LB_REVISION UINT64_C(0x600000)

static uint64_t expansion_vector(QTestState *qts, uint64_t offset,
                                 bool write, uint64_t value)
{
    uint64_t status;

    qtest_readq(qts, EXPANSION_LB_LOCAL + 0x60c038);
    qtest_writeq(qts, EXPANSION_LB_LOCAL + 0x60c008, 0);
    qtest_writeq(qts, EXPANSION_LB_LOCAL + 0x60c010, value);
    qtest_writeq(qts, EXPANSION_LB_LOCAL + 0x60c000,
                 (offset & UINT64_C(0xfffff8)) | (UINT64_C(0x1c8) << 40) |
                 write);
    status = qtest_readq(qts, EXPANSION_LB_LOCAL + 0x60c020);
    g_assert_cmphex(status & (UINT64_C(1) << 63), ==, UINT64_C(1) << 63);
    g_assert_cmphex(status & 7, ==, write);
    return qtest_readq(qts, EXPANSION_LB_LOCAL + 0x60c030);
}

static char *expansion_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd = g_file_open_tmp("sn-builder-expansion-prom-XXXXXX.bin", &path,
                             &error);

    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_expansion_storage_binding_required(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M",
        "origin300,topology=origin300-pci-expansion-configuration-storage-experiment",
        "-cpu", "R14000", "-smp", "2", "-m", "512M", "-S",
        "-nodefaults", "-display", "none", NULL
    };

    assert_qemu_rejects(args,
        "configuration storage selection requires one image binding");
}

#define STORAGE_TOPOLOGY \
    "origin300,topology=origin300-pci-expansion-configuration-storage-experiment"
#define STORAGE_BINDING "expansion-storage.0=expansion-enclosure=cfg"

static char *expansion_storage_image(off_t size)
{
    GError *error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("expansion-storage-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_expansion_storage_input_refusals(void)
{
    static const struct {
        const char *machine;
        const char *message;
    } cases[] = {
        { "origin300,topology=origin300-pci-expansion-experiment,"
          STORAGE_BINDING, "binding requires selection" },
        { STORAGE_TOPOLOGY ",expansion-storage.0=unknown=cfg",
          "names an unselected module" },
        { STORAGE_TOPOLOGY ",expansion-storage.0=compute-module=cfg",
          "names an unselected module" },
        { STORAGE_TOPOLOGY "," STORAGE_BINDING ","
          "expansion-storage.1=expansion-enclosure=cfg",
          "accepts one image binding" },
        { STORAGE_TOPOLOGY ",expansion-storage.0==cfg",
          "expected module-id=backend-id" },
        { STORAGE_TOPOLOGY ",expansion-storage.0=expansion-enclosure=",
          "expected module-id=backend-id" },
        { STORAGE_TOPOLOGY ",expansion-storage.0=expansion-enclosure=cfg=extra",
          "expected module-id=backend-id" },
        { STORAGE_TOPOLOGY "," STORAGE_BINDING,
          "backend 'cfg' does not exist" },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        const char *args[] = { qtest_qemu_binary(NULL), "-M", cases[i].machine,
            "-cpu", "R14000", "-smp", "2", "-m", "512M", "-S",
            "-nodefaults", "-display", "none", NULL };

        assert_qemu_rejects(args, cases[i].message);
    }
}

static void test_expansion_storage_backend_refusals(void)
{
    g_autofree char *small = expansion_storage_image(16 * 1024 * 1024 - 1);
    g_autofree char *large = expansion_storage_image(16 * 1024 * 1024 + 1);
    g_autofree char *raw = expansion_storage_image(16 * 1024 * 1024);
    g_autofree char *qcow = expansion_storage_image(0);
    g_autofree char *prom = expansion_prom();
    const struct {
        const char *path;
        const char *format;
        const char *interface;
        bool readonly;
        const char *message;
    } cases[] = {
        { small, "raw", "none", false, "image of exactly 16 MiB" },
        { large, "raw", "none", false, "image of exactly 16 MiB" },
        { raw, "raw", "none", true, "image of exactly 16 MiB" },
        { qcow, "qcow2", "none", false, "image of exactly 16 MiB" },
        { raw, "raw", "pflash", false, "already in use" },
    };

    g_assert_true(mkimg(qcow, "qcow2", 16));
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_test_message("storage backend refusal: case %u, format %s, "
                       "interface %s, readonly %u", i, cases[i].format,
                       cases[i].interface, cases[i].readonly);
        const char *readonly = "off";

        if (cases[i].readonly) {
            readonly = "on";
        }
        g_autofree char *drive = g_strdup_printf(
            "if=%s,id=cfg,file=%s,format=%s,readonly=%s", cases[i].interface,
            cases[i].path, cases[i].format,
            readonly);
        /* A pflash drive is node 0's boot flash, which then takes no -bios. */
        bool pflash = g_str_equal(cases[i].interface, "pflash");
        const char *args[] = { qtest_qemu_binary(NULL), "-M",
            STORAGE_TOPOLOGY "," STORAGE_BINDING, "-cpu", "R14000",
            "-smp", "2", "-m", "512M", "-S", "-drive", drive,
            "-nodefaults", "-display", "none", pflash ? NULL : "-bios",
            prom, NULL };

        assert_qemu_rejects(args, cases[i].message);
    }
    g_assert_cmpint(unlink(small), ==, 0);
    g_assert_cmpint(unlink(large), ==, 0);
    g_assert_cmpint(unlink(raw), ==, 0);
    g_assert_cmpint(unlink(qcow), ==, 0);
    g_assert_cmpint(unlink(prom), ==, 0);
}

static void test_expansion_storage_provider_refusals(void)
{
    g_autofree char *image = expansion_storage_image(16 * 1024 * 1024);
    g_autofree char *prom = expansion_prom();
    const char *options[] = {
        "driver=raw,file.driver=file,file.filename=%s,offset=0",
        "driver=raw,file.driver=file,file.filename=%s,offset=512",
        "driver=raw,file.driver=file,file.filename=%s,size=16777216",
        "driver=raw,file.driver=blkdebug,file.image.driver=file,"
        "file.image.filename=%s",
        "driver=raw,file.driver=null-co,file.size=16777216",
        "format=raw,file=%s,snapshot=on",
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(options); i++) {
        g_autofree char *provider = g_strdup_printf(options[i], image);
        g_autofree char *drive = g_strdup_printf("if=none,id=cfg,%s", provider);
        const char *args[] = { qtest_qemu_binary(NULL), "-M",
            STORAGE_TOPOLOGY "," STORAGE_BINDING, "-cpu", "R14000",
            "-smp", "2", "-m", "512M", "-S", "-drive", drive,
            "-bios", prom, "-nodefaults", "-display", "none", NULL };

        g_test_message("storage provider refusal: case %u", i);
        assert_qemu_rejects(args, "full regular raw-to-file image");
    }
    g_assert_cmpint(unlink(image), ==, 0);
    g_assert_cmpint(unlink(prom), ==, 0);
}

static void test_expansion_compute_board_identity(void)
{
    static const uint64_t addresses[] = {
        UINT64_C(0x10014000000), UINT64_C(0x10034000000),
    };
    g_autofree char *prom = expansion_prom();
    g_autofree char *log = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    uint8_t bytes[8];
    int fd = g_file_open_tmp("compute-board-identity-XXXXXX", &log, &error);
    QTestState *qts;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf(
        "-M origin300,topology=origin300-pci-expansion-experiment "
        "-cpu R14000 -smp 2 -m 512M -S -bios %s -nodefaults -display none "
        "-d guest_errors -D %s", prom, log);
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        for (unsigned form = 0; form < G_N_ELEMENTS(addresses); form++) {
            uint64_t address = addresses[form];

            g_assert_true(qtest_memread_cpu(qts, cpu, address,
                                           bytes, sizeof(bytes)));
            g_assert_cmphex(ldq_be_p(bytes), ==, 0x8001);
            stq_be_p(bytes, UINT64_MAX);
            g_assert_true(qtest_memwrite_cpu(qts, cpu, address,
                                            bytes, sizeof(bytes)));
            g_assert_true(qtest_memread_cpu(qts, cpu, address,
                                           bytes, sizeof(bytes)));
            g_assert_cmphex(ldq_be_p(bytes), ==, 0x8001);
            for (unsigned width = 1; width <= 4; width *= 2) {
                g_assert_true(qtest_memread_cpu(qts, cpu, address,
                                               bytes, width));
                for (unsigned i = 0; i < width; i++) {
                    g_assert_cmpuint(bytes[i], ==, 0);
                }
                memset(bytes, 0xff, width);
                g_assert_true(qtest_memwrite_cpu(qts, cpu, address,
                                                bytes, width));
            }
            g_assert_true(qtest_memread_cpu(qts, cpu, address + 4, bytes, 4));
            g_assert_cmphex(ldl_be_p(bytes), ==, 0);
            g_assert_true(qtest_memread_cpu(qts, cpu, address + 1,
                                           bytes, sizeof(bytes)));
            g_assert_cmphex(ldq_be_p(bytes), ==, 0);
            g_assert_true(qtest_memread_cpu(qts, cpu, address + 8,
                                           bytes, sizeof(bytes)));
            g_assert_cmphex(ldq_be_p(bytes), ==, 0);
        }
        g_assert_true(qtest_memread_cpu(qts, cpu, UINT64_C(0x10035000000),
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0);
        g_assert_true(qtest_memread_cpu(qts, cpu, UINT64_C(0x10634000000),
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0);
    }
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        for (unsigned form = 0; form < G_N_ELEMENTS(addresses); form++) {
            g_assert_true(qtest_memread_cpu(qts, cpu, addresses[form],
                                           bytes, sizeof(bytes)));
            g_assert_cmphex(ldq_be_p(bytes), ==, 0x8001);
        }
    }
    qtest_writeq(qts, EXPANSION_LB_LOCAL + EXPANSION_LB_REVISION,
                 UINT64_C(2) << 32);
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        g_assert_true(qtest_memread_cpu(qts, cpu, addresses[1],
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0);
        g_assert_true(qtest_memread_cpu(qts, cpu, UINT64_C(0x10434000000),
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0x8001);
        g_assert_true(qtest_memread_cpu(qts, cpu, addresses[0],
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0x8001);
    }
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/expansion-cable',"
        "'property':'down','value':true}}");
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        g_assert_true(qtest_memread_cpu(qts, cpu, UINT64_C(0x10234000000),
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0);
        g_assert_true(qtest_memread_cpu(qts, cpu, UINT64_C(0x10434000000),
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0x8001);
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(contents, "refusing a 8-byte write"));
    g_assert_nonnull(strstr(contents, "refusing 4 bytes at offset 0x4"));
    g_assert_nonnull(strstr(contents,
        "remote board identification refuses 4-byte read at 0x10034000004"));
    g_assert_nonnull(strstr(contents,
        "remote board identification refuses 1-byte read at 0x10034000001"));
    g_assert_nonnull(strstr(contents,
        "unmapped remote Hub register 8-byte read at 0x10034000008"));
    g_assert_nonnull(strstr(contents,
        "unmapped remote Hub register 8-byte read at 0x10035000000"));
    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(unlink(prom), ==, 0);
}

static void test_expansion_compute_board_identity_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *prom = expansion_prom();
    g_autofree char *dir =
        g_dir_make_tmp("compute-board-migrate-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from, *to;
    uint8_t bytes[8];

    g_assert_no_error(error);
    from = qtest_initf(
        "-M origin300,topology=origin300-pci-expansion-experiment "
        "-cpu R14000 -smp 2 -m 512M -S -bios %s -nodefaults -display none",
        prom);
    to = qtest_initf(
        "-M origin300,topology=origin300-pci-expansion-experiment "
        "-cpu R14000 -smp 2 -m 512M -S -bios %s -nodefaults -display none "
        "-incoming defer", prom);
    expansion_vector(from, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    qtest_writeq(from, EXPANSION_LB_LOCAL + EXPANSION_LB_REVISION,
                 UINT64_C(2) << 32);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        const uint64_t addresses[] = {
            UINT64_C(0x10014000000), UINT64_C(0x10434000000),
        };

        for (unsigned i = 0; i < G_N_ELEMENTS(addresses); i++) {
            g_assert_true(qtest_memread_cpu(to, cpu, addresses[i],
                                           bytes, sizeof(bytes)));
            g_assert_cmphex(ldq_be_p(bytes), ==, 0x8001);
        }
        g_assert_true(qtest_memread_cpu(to, cpu, UINT64_C(0x10034000000),
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0);
    }
    qtest_quit(from);
    qtest_quit(to);
    /* QEMU may remove the listener when incoming migration completes. */
    if (unlink(socket) < 0) {
        g_assert_cmpint(errno, ==, ENOENT);
    }
    g_assert_cmpint(rmdir(dir), ==, 0);
    g_assert_cmpint(unlink(prom), ==, 0);
}

static void test_expansion_compute_remote_boot(const void *data)
{
    const bool pflash = data != NULL;
    const uint64_t value = UINT64_C(0x123456789abcdef0);
    const uint64_t remote = UINT64_C(0x10038000078);
    g_autofree char *prom = expansion_prom();
    g_autofree char *flash = pflash ?
        expansion_storage_image(16 * 1024 * 1024) : NULL;
    g_autofree char *drive = pflash ? g_strdup_printf(
        "-drive if=pflash,file=%s,format=raw,readonly=off", flash) :
        g_strdup("");
    g_autoptr(GError) error = NULL;
    g_autofree char *dir =
        g_dir_make_tmp("compute-boot-migrate-XXXXXX", &error);
    g_autofree char *destination_flash = pflash ?
        expansion_storage_image(16 * 1024 * 1024) : NULL;
    g_autofree char *destination_drive = pflash ? g_strdup_printf(
        "-drive if=pflash,file=%s,format=raw,readonly=off", destination_flash) :
        g_strdup("");
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    uint8_t bytes[8];
    int fd = open(pflash ? flash : prom, O_WRONLY);
    QTestState *qts, *to;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    stq_be_p(bytes, value);
    g_assert_cmpint(pwrite(fd, bytes, sizeof(bytes), 0x78), ==, sizeof(bytes));
    if (pflash) {
        stq_be_p(bytes, ~value);
        g_assert_cmpint(pwrite(fd, bytes, sizeof(bytes), 0x800078),
                        ==, sizeof(bytes));
    }
    g_assert_cmpint(close(fd), ==, 0);
    /* A bound flash holds the PROM itself, so it takes no -bios. */
    qts = qtest_initf(
        "-M origin300,topology=origin300-pci-expansion-experiment "
        "-cpu R14000 -smp 2 -m 512M -S %s%s %s "
        "-nodefaults -display none", pflash ? "" : "-bios ",
        pflash ? "" : prom, drive);
    /* Before peer activation, the CPU claim reaches the global boot alias. */
    g_assert_cmphex(qtest_readq(qts, remote), ==, value);
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        g_assert_true(qtest_memread_cpu(qts, cpu, remote,
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, value);
        for (unsigned width = 1; width <= 8; width *= 2) {
            g_assert_true(qtest_memread_cpu(qts, cpu, remote, bytes, width));
            for (unsigned i = 0; i < width; i++) {
                g_assert_cmpuint(bytes[i], ==, value >> (56 - i * 8) & 0xff);
            }
        }
        g_assert_true(qtest_memread_cpu(qts, cpu, remote + 0x2000000,
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, pflash ? ~value : UINT64_MAX);
        g_assert_true(qtest_memread_cpu(qts, cpu, remote + 0x1000000,
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0);
        memset(bytes, 0xff, sizeof(bytes));
        g_assert_true(qtest_memwrite_cpu(qts, cpu, remote,
                                        bytes, sizeof(bytes)));
        g_assert_true(qtest_memread_cpu(qts, cpu, remote,
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, value);
    }
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    qtest_writeq(qts, EXPANSION_LB_LOCAL + EXPANSION_LB_REVISION,
                 UINT64_C(2) << 32);
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        g_assert_true(qtest_memread_cpu(qts, cpu, remote,
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0);
        g_assert_true(qtest_memread_cpu(qts, cpu, remote + (UINT64_C(2) << 33),
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, value);
        g_assert_true(qtest_memread_cpu(qts, cpu, remote + (UINT64_C(1) << 33),
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0);
        g_assert_true(qtest_memread_cpu(qts, cpu, remote + (UINT64_C(3) << 33),
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0);
    }
    /* The private boot views are constructed again, not migrated as state. */
    if (pflash) {
        /* Existing flash command decode: a 64-bit store enters query mode. */
        qtest_writeq(qts, UINT64_C(0x104380002a8), 0x98);
        g_assert_cmphex(qtest_readw(qts, UINT64_C(0x10438000020)), ==, 'Q');
    }
    to = qtest_initf(
        "-M origin300,topology=origin300-pci-expansion-experiment "
        "-cpu R14000 -smp 2 -m 512M -S %s%s %s "
        "-nodefaults -display none -incoming defer", pflash ? "" : "-bios ",
        pflash ? "" : prom, destination_drive);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(qts, uri);
    sgi_wait_migration_status(to, "completed", 30);
    if (pflash) {
        g_assert_cmphex(qtest_readw(to, UINT64_C(0x10438000020)), ==, 'Q');
        qtest_writeq(to, UINT64_C(0x10438002aa8), 0xf0);
    }
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        g_assert_true(qtest_memread_cpu(to, cpu,
            remote + (UINT64_C(2) << 33), bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, value);
        g_assert_true(qtest_memread_cpu(to, cpu, remote, bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0);
    }
    qtest_quit(qts);
    qtest_quit(to);
    if (unlink(socket) < 0) {
        g_assert_cmpint(errno, ==, ENOENT);
    }
    g_assert_cmpint(rmdir(dir), ==, 0);
    g_assert_cmpint(unlink(prom), ==, 0);
    if (pflash) {
        g_assert_cmpint(unlink(flash), ==, 0);
        g_assert_cmpint(unlink(destination_flash), ==, 0);
    }
}

static void test_expansion_processor_absence(void)
{
    static const uint64_t offsets[] = { 0x40, 0x48, 0x200040, 0x200048 };
    g_autofree char *prom = expansion_prom();
    g_autofree char *log = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    uint8_t bytes[8];
    int fd = g_file_open_tmp("expansion-presence-XXXXXX", &log, &error);
    QTestState *qts;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf(
        "-M origin300,topology=origin300-pci-expansion-experiment "
        "-cpu R14000 -smp 2 -m 512M -S -bios %s -nodefaults -display none "
        "-d unimp -D %s", prom, log);
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        for (unsigned word = 0; word < G_N_ELEMENTS(offsets); word++) {
            uint64_t address = UINT64_C(0x20201800000) + offsets[word];

            g_assert_true(qtest_memread_cpu(qts, cpu, address,
                                           bytes, sizeof(bytes)));
            g_assert_cmphex(ldq_be_p(bytes), ==, 0);
            stq_be_p(bytes, 0xfeed0000 + cpu * 4 + word);
            g_assert_true(qtest_memwrite_cpu(qts, cpu, address,
                                            bytes, sizeof(bytes)));
            g_assert_true(qtest_memread_cpu(qts, cpu, address,
                                           bytes, sizeof(bytes)));
            g_assert_cmphex(ldq_be_p(bytes), ==, 0);
            g_assert_true(qtest_memread_cpu(qts, cpu,
                UINT64_C(0x20001800000) + offsets[word], bytes, sizeof(bytes)));
            /* The product populates slots 0 and 2 before slots 1 and 3. */
            g_assert_cmphex(ldq_be_p(bytes), ==, !(word & 1));
        }
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log, &contents, NULL, &error));
    g_assert_no_error(error);
    /* Zero reads alone cannot distinguish qtest's discarded decode result. */
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        for (unsigned word = 0; word < G_N_ELEMENTS(offsets); word++) {
            g_autofree char *expected = g_strdup_printf(
                "sgi-bedrock-pi: write of 0x%x to hardware-set register "
                "0x%" PRIx64 " ignored", 0xfeed0000 + cpu * 4 + word,
                offsets[word] & 0x1fffff);

            g_assert_nonnull(strstr(contents, expected));
        }
    }
    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(unlink(prom), ==, 0);
}

static void test_expansion_six_head_pio(void)
{
    static const unsigned widgets[] = { 8, 9, 0xc, 0xd, 0xe, 0xf };
    const uint64_t remote = SPACE_IO + (UINT64_C(1) << 33);
    g_autofree char *image = expansion_storage_image(16 * 1024 * 1024);
    g_autofree char *prom = expansion_prom();
    g_autofree char *log = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    uint8_t bytes[4];
    int fd = g_file_open_tmp("expansion-six-heads-XXXXXX", &log, &error);
    QTestState *qts;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf(
        "-M origin300,topology="
        "origin300-pci-expansion-unconnected-uart-experiment,%s "
        "-cpu R14000 -smp 2 -m 512M -S "
        "-drive if=none,id=cfg,file=%s,format=raw,readonly=off "
        "-bios %s -nodefaults -display none -d guest_errors -D %s",
        STORAGE_BINDING, image, prom, log);
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    for (unsigned head = 0; head < G_N_ELEMENTS(widgets); head++) {
        uint64_t base = remote + ((uint64_t)widgets[head] << 24);

        stl_be_p(bytes, 0x123450 + head);
        g_assert_true(qtest_memwrite_cpu(qts, 0, base + 0x124,
                                        bytes, sizeof(bytes)));
    }
    /* Distinct maps distinguish six owners from aliases of one head. */
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        for (unsigned head = 0; head < G_N_ELEMENTS(widgets); head++) {
            uint64_t base = remote + ((uint64_t)widgets[head] << 24);

            g_assert_true(qtest_memread_cpu(qts, cpu, base + 4,
                                           bytes, sizeof(bytes)));
            g_assert_cmphex(ldl_be_p(bytes), ==, 0x2d002049);
            g_assert_true(qtest_memread_cpu(qts, cpu, base + 0x124,
                                           bytes, sizeof(bytes)));
            g_assert_cmphex(ldl_be_p(bytes), ==, 0x123450 + head);
            g_assert_true(qtest_memread_cpu(qts, cpu, base + 0x104,
                                           bytes, sizeof(bytes)));
            g_assert_cmphex(ldl_be_p(bytes) & (1U << 24), ==, 0);
        }
    }
    /* An absent U0 widget must not reach another PCI head. */
    g_assert_true(qtest_memread_cpu(qts, 0, remote + (UINT64_C(0xb) << 24) + 4,
                                   bytes, sizeof(bytes)));
    g_assert_cmphex(ldl_be_p(bytes), ==, 0);
    for (unsigned head = 0; head < G_N_ELEMENTS(widgets); head++) {
        uint64_t base = remote + ((uint64_t)widgets[head] << 24);

        g_assert_true(qtest_memread_cpu(qts, 1, base + 0x104,
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldl_be_p(bytes) & (1U << 24), ==, 0);
    }
    /* This undefined register is a head error, not a widget-route refusal. */
    g_assert_true(qtest_memread_cpu(qts, 0,
        remote + (UINT64_C(0xc) << 24) + 0x98, bytes, sizeof(bytes)));
    g_assert_cmphex(ldl_be_p(bytes), ==, 0);
    for (unsigned head = 0; head < G_N_ELEMENTS(widgets); head++) {
        uint64_t base = remote + ((uint64_t)widgets[head] << 24);

        g_assert_true(qtest_memread_cpu(qts, 1, base + 0x104,
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldl_be_p(bytes) & (1U << 24), ==,
                        widgets[head] == 0xc ? 1U << 24 : 0);
        g_assert_true(qtest_memread_cpu(qts, 1, base + 0x124,
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldl_be_p(bytes), ==, 0x123450 + head);
    }
    g_assert_true(qtest_memread_cpu(qts, 1,
        remote + (UINT64_C(0xc) << 24) + 0x1c, bytes, sizeof(bytes)));
    g_assert_cmphex(ldl_be_p(bytes), ==, 0x98);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(contents, "sgi-xbridge: invalid access at 0x98"));
    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_assert_cmpint(unlink(image), ==, 0);
}

static void expansion_card_write(QTestState *qts, uint64_t address,
                                  uint64_t value, unsigned size)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 0, address, bytes + 8 - size, size));
}

static uint64_t expansion_card_read(QTestState *qts, uint64_t address,
                                   unsigned size)
{
    uint8_t bytes[8] = { 0 };

    g_assert_true(qtest_memread_cpu(qts, 1, address, bytes + 8 - size, size));
    return ldq_be_p(bytes);
}

static void test_expansion_composed_adapters(void)
{
    static const unsigned widgets[] = { 0xe, 0xf, 8, 9, 0xc, 0xd };
    const uint64_t remote = SPACE_IO + (UINT64_C(1) << 33);
    const uint64_t pending = SPACE_IO + 0x1800098;
    g_autofree char *image = expansion_storage_image(16 * 1024 * 1024);
    g_autofree char *prom = expansion_prom();
    g_autoptr(GString) cards = g_string_new(NULL);
    g_autofree char *log = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("expansion-adapter-errors-XXXXXX", &log, &error);
    QTestState *qts;
    uint8_t bytes[8];
    uint8_t expected[6][2][8];

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    memset(expected, 0xcc, sizeof(expected));
    /* Device numbers are synthetic positions, not physical slot labels. */
    for (unsigned head = 0; head < G_N_ELEMENTS(widgets); head++) {
        for (unsigned device = 1; device <= 2; device++) {
            g_string_append_printf(cards,
                " -device sgi-bridge-dma-test,bus=expansion0-u%u-head-%c.0,"
                "addr=%u,device-id=%u", head / 2, head % 2 ? 'f' : 'e',
                device, 0xc000 + head * 2 + device);
        }
    }
    qts = qtest_initf(
        "-M origin300,topology="
        "origin300-pci-expansion-unconnected-uart-experiment,%s "
        "-cpu R14000 -smp 2 -m 512M -S "
        "-drive if=none,id=cfg,file=%s,format=raw,readonly=off "
        "-bios %s -nodefaults -display none -d guest_errors,unimp -D %s %s",
        STORAGE_BINDING, image, prom, log, cards->str);
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    /* CPU memory follows the configured MD bank map, as on one chassis. */
    qtest_writeq(qts, UINT64_C(0x20001000000) + 0x780008,
                 (UINT64_C(2) << 32) | 0x3300);
    for (unsigned head = 0; head < G_N_ELEMENTS(widgets); head++) {
        uint64_t base = remote + ((uint64_t)widgets[head] << 24);
        uint64_t target = 0x8000 + head * 0x1000;

        expansion_card_write(qts, base + 0x24,
            (expansion_card_read(qts, base + 0x24, 4) & ~15U) |
            widgets[head], 4);
        expansion_card_write(qts, base + 0x10000,
                             target | 0x21 | (0xa << 8), 8);
        for (unsigned device = 1; device <= 2; device++) {
            uint64_t config = base + 0x20000 + device * 0x1000;
            uint64_t bar = base + (device == 1 ? 0x400000 : 0x600000);
            unsigned identity = 0xc000 + head * 2 + device;

            g_assert_cmphex(expansion_card_read(qts, config, 4) >> 16,
                            ==, identity);
            expansion_card_write(qts, config + PCI_BAR0,
                                 CARD_MMIO + device * 0x200000, 4);
            expansion_card_write(qts, config + PCI_COMMAND,
                                 PCI_COMMAND_MEMORY_MASTER, 4);
            expansion_card_write(qts, base + 0x204 + device * 8,
                                 0x81000 | (0x10 + device * 2), 4);
            expansion_card_write(qts, bar + 0x24, identity, 4);
            qtest_memwrite(qts, target + (device - 1) * 0x100,
                           expected[head][device - 1], sizeof(bytes));
        }
    }
    for (unsigned head = 0; head < G_N_ELEMENTS(widgets); head++) {
        uint64_t base = remote + ((uint64_t)widgets[head] << 24);
        uint64_t target = 0x8000 + head * 0x1000;

        for (unsigned device = 1; device <= 2; device++) {
            uint64_t bar = base + (device == 1 ? 0x400000 : 0x600000);
            uint64_t offset = (device - 1) * 0x100;
            unsigned seed = 0x20 + head * 16 + device;
            unsigned pin = device;
            unsigned number = 16 + head * 2 + device;

            g_test_message("composed head %u widget %x device %u", head,
                           widgets[head], device);
            expansion_card_write(qts, bar + DMA_REG_ADDRESS,
                                 0x40000000 + offset, 4);
            expansion_card_write(qts, bar + DMA_REG_LENGTH, sizeof(bytes), 4);
            expansion_card_write(qts, bar + DMA_REG_SEED, seed, 4);
            expansion_card_write(qts, bar + DMA_REG_GO, 1, 4);
            for (unsigned i = 0; i < sizeof(bytes); i++) {
                expected[head][device - 1][i] = seed + i;
            }
            for (unsigned other = 0; other < G_N_ELEMENTS(widgets); other++) {
                for (unsigned slot = 0; slot < 2; slot++) {
                    qtest_memread(qts, 0x8000 + other * 0x1000 + slot * 0x100,
                                  bytes, sizeof(bytes));
                    g_assert_cmpmem(bytes, sizeof(bytes), expected[other][slot],
                                    sizeof(bytes));
                }
            }
            /* A read then write must use this head's same translation. */
            expansion_card_write(qts, bar + DMA_REG_SOURCE,
                                 0x40000000 + offset, 4);
            expansion_card_write(qts, bar + DMA_REG_ADDRESS,
                                 0x40000200 + offset, 4);
            expansion_card_write(qts, bar + DMA_REG_GO, 2, 4);
            qtest_memread(qts, target + 0x200 + offset, bytes, sizeof(bytes));
            g_assert_cmpmem(bytes, sizeof(bytes), expected[head][device - 1],
                            sizeof(bytes));
            /* A RAM-less BIC destination cannot fall back to compute RAM. */
            expansion_card_write(qts, base + 0x10000,
                (UINT64_C(1) << 33) | target | 0x21 | (0xa << 8), 8);
            expansion_card_write(qts, bar + DMA_REG_SEED, seed + 1, 4);
            expansion_card_write(qts, bar + DMA_REG_GO, 1, 4);
            qtest_memread(qts, target + 0x200 + offset, bytes, sizeof(bytes));
            g_assert_cmpmem(bytes, sizeof(bytes), expected[head][device - 1],
                            sizeof(bytes));
            expansion_card_write(qts, base + 0x10000,
                                 target | 0x21 | (0xa << 8), 8);

            expansion_card_write(qts, base + 0x34, (0xa << 16) | 0x8000, 4);
            expansion_card_write(qts, base + 0x3c, HUB_INT_MODIFY_OFFSET, 4);
            expansion_card_write(qts, base + 0x134 + pin * 8,
                                 (INT_HOST_PACKET_IO_NASID0 << 8) | number, 4);
            expansion_card_write(qts, base + 0x10c, 1U << pin, 4);
            expansion_card_write(qts, base + 0x11c, 1U << pin, 4);
            g_assert_cmphex(expansion_card_read(qts, pending, 8), ==, 0);
            expansion_card_write(qts, bar + DMA_REG_INTERRUPT, 1, 4);
            g_test_message("IRQ raw=%" PRIx64 " held=%" PRIx64
                           " pending=%" PRIx64,
                expansion_card_read(qts, base + 0x104, 4),
                expansion_card_read(qts, bar + DMA_REG_INTERRUPT, 4),
                expansion_card_read(qts, pending, 8));
            g_assert_cmphex(expansion_card_read(qts, pending, 8), ==,
                            UINT64_C(1) << number);
            for (unsigned other = 0; other < G_N_ELEMENTS(widgets); other++) {
                uint64_t owner = remote + ((uint64_t)widgets[other] << 24);

                g_assert_cmphex(expansion_card_read(qts, owner + 0x104, 4) &
                                0xff, ==, other == head ? 1U << pin : 0);
                g_assert_cmphex(expansion_card_read(qts, owner + 0x12004, 4),
                                ==, (0x8000 + other * 0x1000) | 0xa21);
                for (unsigned slot = 1; slot <= 2; slot++) {
                    uint64_t card = owner +
                        (slot == 1 ? 0x400000 : 0x600000);

                    g_assert_cmphex(expansion_card_read(qts, card + 0x24, 4),
                                    ==, 0xc000 + other * 2 + slot);
                }
            }
            expansion_card_write(qts, bar + DMA_REG_INTERRUPT, 0, 4);
            g_assert_cmphex(expansion_card_read(qts, pending, 8), ==, 0);
            g_assert_cmphex(expansion_card_read(qts, base + 0x104, 4) & 0xff,
                            ==, 0);
        }
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log, &contents, NULL, &error));
    g_assert_no_error(error);
    for (unsigned head = 0; head < G_N_ELEMENTS(widgets); head++) {
        for (unsigned device = 1; device <= 2; device++) {
            uint64_t address = (UINT64_C(1) << 33) | (0x8200 + head * 0x1000 +
                                                       (device - 1) * 0x100);
            g_autofree char *refusal = g_strdup_printf(
                "sn1: NI packet at 0x%" PRIx64 " has no destination for node 1 "
                "(source widget %u)", address, widgets[head]);

            g_assert_nonnull(strstr(contents, refusal));
        }
    }
    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(unlink(image), ==, 0);
    g_assert_cmpint(unlink(prom), ==, 0);
}

static void test_expansion_dma_memory_decode(void)
{
    const uint64_t base = SPACE_IO + (UINT64_C(1) << 33) + (0xeULL << 24);
    const uint64_t bar = base + 0x400000;
    const uint64_t target = 0x40008000;
    const uint64_t copy = 0x40008200;
    g_autofree char *image = expansion_storage_image(16 * 1024 * 1024);
    g_autofree char *prom = expansion_prom();
    g_autofree char *log = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    uint8_t bytes[8], expected[8];
    int fd = g_file_open_tmp("expansion-md-dma-XXXXXX", &log, &error);
    QTestState *qts;

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf(
        "-M origin300,topology="
        "origin300-pci-expansion-unconnected-uart-experiment,%s "
        "-cpu R14000 -smp 2 -m 512M -S "
        "-drive if=none,id=cfg,file=%s,format=raw,readonly=off "
        "-bios %s -nodefaults -display none -d guest_errors -D %s "
        "-device sgi-bridge-dma-test,bus=expansion0-u0-head-e.0,addr=1",
        STORAGE_BINDING, image, prom, log);
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    /* Software bank 1 uses dense backing at 256 MiB, not physical 1 GiB. */
    qtest_writeq(qts, UINT64_C(0x20001000000) + 0x780008,
                 (UINT64_C(2) << 32) | 0x3300);
    expansion_card_write(qts, base + 0x24,
                         (expansion_card_read(qts, base + 0x24, 4) & ~15U) |
                         0xe, 4);
    expansion_card_write(qts, base + 0x21010, CARD_MMIO + 0x200000, 4);
    expansion_card_write(qts, base + 0x21004, PCI_COMMAND_MEMORY_MASTER, 4);
    expansion_card_write(qts, base + 0x20c, 0x81012, 4);
    expansion_card_write(qts, base + 0x10000, target | 0x21 | (0xa << 8), 8);
    memset(bytes, 0xcc, sizeof(bytes));
    g_assert_true(qtest_memwrite_cpu(qts, 0, target, bytes, sizeof(bytes)));
    expansion_card_write(qts, bar + DMA_REG_ADDRESS, 0x40000000, 4);
    expansion_card_write(qts, bar + DMA_REG_LENGTH, sizeof(bytes), 4);
    expansion_card_write(qts, bar + DMA_REG_SEED, 0x51, 4);
    expansion_card_write(qts, bar + DMA_REG_GO, 1, 4);
    for (unsigned i = 0; i < sizeof(expected); i++) {
        expected[i] = 0x51 + i;
    }
    g_assert_true(qtest_memread_cpu(qts, 1, target, bytes, sizeof(bytes)));
    g_assert_cmpmem(bytes, sizeof(bytes), expected, sizeof(expected));
    /* Exercise incoming reads as well as writes through the relocated bank. */
    expansion_card_write(qts, bar + DMA_REG_SOURCE, 0x40000000, 4);
    expansion_card_write(qts, bar + DMA_REG_ADDRESS, 0x40000200, 4);
    expansion_card_write(qts, bar + DMA_REG_GO, 2, 4);
    g_assert_true(qtest_memread_cpu(qts, 0, copy, bytes, sizeof(bytes)));
    g_assert_cmpmem(bytes, sizeof(bytes), expected, sizeof(expected));
    /* The first byte beyond this bank is a hole, not dense backing. */
    expansion_card_write(qts, base + 0x10000,
                         UINT64_C(0x50000000) | 0x21 | (0xa << 8), 8);
    expansion_card_write(qts, bar + DMA_REG_ADDRESS, 0x40000000, 4);
    expansion_card_write(qts, bar + DMA_REG_SEED, 0x81, 4);
    expansion_card_write(qts, bar + DMA_REG_GO, 1, 4);
    /* A disabled installed bank also refuses a formerly valid address. */
    expansion_card_write(qts, base + 0x10000, target | 0x21 | (0xa << 8), 8);
    qtest_writeq(qts, UINT64_C(0x20001000000) + 0x780008,
                 (UINT64_C(2) << 32) | 0x300);
    expansion_card_write(qts, bar + DMA_REG_GO, 1, 4);
    qtest_writeq(qts, UINT64_C(0x20001000000) + 0x780008,
                 (UINT64_C(2) << 32) | 0x3300);
    g_assert_true(qtest_memread_cpu(qts, 1, target, bytes, sizeof(bytes)));
    g_assert_cmpmem(bytes, sizeof(bytes), expected, sizeof(expected));
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(contents, "refused NI packet at 0x50000000"));
    g_assert_nonnull(strstr(contents, "refused NI packet at 0x40008000"));
    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_assert_cmpint(unlink(image), ==, 0);
}

static void assert_compute_memory_projection(QTestState *qts, unsigned cpu,
                                             uint64_t address,
                                             const char *kind)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute':'human-monitor-command','arguments':"
        "{'command-line':'info mtree -f'}}");
    const char *text = qdict_get_str(response, "return");
    g_autofree char *root = g_strdup_printf(
        "root: sn1.node0.cpu%u.memory", cpu);
    const char *view = strstr(text, root);
    const char *end;

    g_assert_nonnull(view);
    end = strstr(view, "\nFlatView #");
    for (const char *line = strchr(view, '\n'); line && (!end || line < end);
         line = strchr(line + 1, '\n')) {
        uint64_t first, last;

        if (sscanf(line, " %" SCNx64 "-%" SCNx64, &first, &last) == 2 &&
            address >= first && address <= last) {
            const char *next = strchr(line + 1, '\n');
            g_autofree char *range = g_strndup(line, next ? next - line :
                                                         strlen(line));

            g_test_message("CPU %u address 0x%" PRIx64 "%s", cpu, address,
                           range);
            g_autofree char *type = g_strdup_printf(", %s):", kind);

            g_assert_nonnull(strstr(range, type));
            return;
        }
    }
    g_error("CPU %u address 0x%" PRIx64 " is absent from its flat view",
            cpu, address);
}

static void assert_compute_direct_aliases(QTestState *qts, unsigned cpus,
                                          bool enabled)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute':'human-monitor-command','arguments':"
        "{'command-line':'info mtree -D'}}");
    const char *text = qdict_get_str(response, "return");

    for (unsigned cpu = 0; cpu < cpus; cpu++) {
        for (unsigned memory = 0; memory < 2; memory++) {
            g_autofree char *name = g_strdup_printf(
                "sn1.node0.cpu%u.direct-%s @", cpu,
                memory ? "boot" : "memory");
            const char *alias = strstr(text, name);
            const char *end;
            g_autofree char *line = NULL;

            g_assert_nonnull(alias);
            end = strchr(alias, '\n');
            line = g_strndup(alias, end ? end - alias : strlen(alias));
            g_assert_cmpint(strstr(line, "[disabled]") == NULL, ==, enabled);
        }
    }
}

static void test_compute_memory_projection(gconstpointer opaque)
{
    unsigned selection = GPOINTER_TO_UINT(opaque);
    bool expansion = selection & 1;
    unsigned cpus = selection & 2 ? 4 : 2;
    g_autofree char *image = expansion ?
        expansion_storage_image(16 * 1024 * 1024) : NULL;
    g_autofree char *prom = expansion_prom();
    g_autofree char *drive = expansion ? g_strdup_printf(
        "-drive if=none,id=cfg,file=%s,format=raw,readonly=off", image) :
        g_strdup("");
    QTestState *qts = qtest_initf(
        "-M origin300%s -cpu R14000 -smp %u -m 512M -S %s -bios %s "
        "-nodefaults -display none", expansion ? ",topology="
        "origin300-pci-expansion-pending-register-experiment," STORAGE_BINDING :
        "", cpus, drive, prom);

    if (expansion) {
        expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    }
    for (unsigned remap = 0; remap < 2; remap++) {
        unsigned nasid = remap ? 9 : 0;
        uint64_t node = (uint64_t)nasid << NASID_TEST_NODE_ADDRESS_SHIFT;

        if (remap) {
            uint64_t revision = qtest_readq(qts, EXPANSION_LB_LOCAL +
                                           EXPANSION_LB_REVISION);

            qtest_writeq(qts, EXPANSION_LB_LOCAL + EXPANSION_LB_REVISION,
                (revision & ~(UINT64_C(0xff) << 32)) |
                ((uint64_t)nasid << 32));
        }
        for (unsigned mapping = 0; mapping < 2; mapping++) {
            /* Installed locational banks 2 and 3 have 256 MiB each. */
            unsigned swap = mapping ? 0 : 2;
            uint64_t first = node + ((uint64_t)(2 ^ swap) << 30);
            uint64_t second = node + ((uint64_t)(3 ^ swap) << 30);
            uint8_t bytes[8];
            uint64_t values[] = { UINT64_C(0x1122334455667788),
                                  UINT64_C(0x8877665544332211) };
            uint64_t addresses[] = { first + 0x40000, second + 0x40000 };

            qtest_writeq(qts, UINT64_C(0x20001000000) + 0x780008,
                         ((uint64_t)swap << 32) | 0x3300);
            for (unsigned bank = 0; bank < G_N_ELEMENTS(addresses); bank++) {
                stq_be_p(bytes, values[bank]);
                g_assert_true(qtest_memwrite_cpu(qts, 0, addresses[bank],
                                                bytes, sizeof(bytes)));
            }
            for (unsigned cpu = 0; cpu < cpus; cpu++) {
                for (unsigned bank = 0;
                     bank < G_N_ELEMENTS(addresses); bank++) {
                    assert_compute_memory_projection(qts, cpu,
                                                      addresses[bank], "ram");
                    g_assert_true(qtest_memread_cpu(qts, cpu, addresses[bank],
                                                   bytes, sizeof(bytes)));
                    g_assert_cmphex(ldq_be_p(bytes), ==, values[bank]);
                    assert_compute_memory_projection(qts, cpu,
                        addresses[bank] - 0x40000 + 0xffffff8, "ram");
                }
                /* The uncached attribute-2 window retains fetch-and-op I/O. */
                assert_compute_memory_projection(qts, cpu,
                    UINT64_C(0x30000000000) + node + 0x40000, "i/o");
                assert_compute_memory_projection(qts, cpu,
                    NASID_TEST_SPACE_HUB + node + 0x38000000, "rom");
                assert_compute_memory_projection(qts, cpu, 0x1fc00000,
                                                  "rom");
            }
        }
    }
    if (expansion) {
        uint64_t revision = qtest_readq(qts, EXPANSION_LB_LOCAL +
                                       EXPANSION_LB_REVISION);
        uint64_t peer = expansion_vector(qts, EXPANSION_LB_REVISION, false, 0);
        const uint64_t named = UINT64_C(9) << NASID_TEST_NODE_ADDRESS_SHIFT;

        assert_compute_direct_aliases(qts, cpus, true);
        /* Duplicate numbers retain ordinary self-only claims, not aliases. */
        expansion_vector(qts, EXPANSION_LB_REVISION, true,
            (peer & ~(UINT64_C(0xff) << 32)) | (UINT64_C(9) << 32));
        assert_compute_direct_aliases(qts, cpus, false);
        expansion_vector(qts, EXPANSION_LB_REVISION, true, peer);
        assert_compute_direct_aliases(qts, cpus, true);
        qtest_writeq(qts, EXPANSION_LB_LOCAL + EXPANSION_LB_REVISION,
                     revision | (UINT64_C(0x80) << 32));
        assert_compute_direct_aliases(qts, cpus, false);
        for (unsigned cpu = 0; cpu < cpus; cpu++) {
            assert_compute_memory_projection(qts, cpu,
                                              named + 0x80040000, "i/o");
        }
        qtest_writeq(qts, EXPANSION_LB_LOCAL + EXPANSION_LB_REVISION, revision);
        assert_compute_direct_aliases(qts, cpus, true);
        expansion_vector(qts, EXPANSION_LB_REVISION, true,
                         peer | (UINT64_C(0x80) << 32));
        assert_compute_direct_aliases(qts, cpus, false);
        expansion_vector(qts, EXPANSION_LB_REVISION, true, peer);
        assert_compute_direct_aliases(qts, cpus, true);
        for (unsigned cpu = 0; cpu < cpus; cpu++) {
            assert_compute_memory_projection(qts, cpu,
                                              named + 0x80040000, "ram");
        }
    }
    qtest_quit(qts);
    if (image) {
        g_assert_cmpint(unlink(image), ==, 0);
    }
    g_assert_cmpint(unlink(prom), ==, 0);
}

static void test_compute_uart_with_expansion(gconstpointer opaque)
{
    unsigned selection = GPOINTER_TO_UINT(opaque);
    bool expansion = selection & 1;
    unsigned cpus = selection & 2 ? 4 : 2;
    const uint64_t local = NASID_TEST_HUB_LOCAL_REGISTERS + 0x80;
    const uint64_t named = NASID_TEST_SPACE_HUB + 0x30000080;
    static const struct {
        unsigned offset;
        uint8_t value;
    } setup[] = {
        { 0x18, 0x83 }, { 0x00, 13 }, { 0x08, 0 }, { 0x18, 3 },
        { 0x08, 0 }, { 0x10, 7 }, { 0x20, 0x22 }, { 0x38, 0x5a },
    };
    static const struct {
        unsigned offset;
        uint8_t value;
    } state[] = {
        { 0x08, 0 }, { 0x10, 1 }, { 0x18, 3 }, { 0x20, 0x22 },
        { 0x28, 0x60 }, { 0x30, 0x10 }, { 0x38, 0x5a },
    };
    g_autofree char *image = expansion ?
        expansion_storage_image(16 * 1024 * 1024) : NULL;
    g_autofree char *prom = expansion_prom();
    g_autofree char *machine = expansion ? g_strdup_printf(
        "origin300,topology="
        "origin300-pci-expansion-pending-register-experiment,%s",
        STORAGE_BINDING) : g_strdup("origin300");
    g_autofree char *drive = expansion ? g_strdup_printf(
        "-drive if=none,id=cfg,file=%s,format=raw,readonly=off", image) :
        g_strdup("");
    QTestState *qts = qtest_initf(
        "-M %s -cpu R14000 -smp %u -m 512M -S %s -bios %s "
        "-nodefaults -display none", machine, cpus, drive, prom);
    uint8_t bytes[8];

    if (expansion) {
        expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(setup); i++) {
        stq_be_p(bytes, UINT64_C(0x1234567800000000) | setup[i].value);
        g_assert_true(qtest_memwrite_cpu(qts, 0, local + setup[i].offset,
                                        bytes, sizeof(bytes)));
    }
    for (unsigned number = 0; number < 2; number++) {
        unsigned nasid = number ? 9 : 0;
        uint64_t views[] = {
            local, named + ((uint64_t)nasid << NASID_TEST_NODE_ADDRESS_SHIFT),
        };

        if (number) {
            uint64_t revision = qtest_readq(qts, EXPANSION_LB_LOCAL +
                                           EXPANSION_LB_REVISION);

            qtest_writeq(qts, EXPANSION_LB_LOCAL + EXPANSION_LB_REVISION,
                (revision & ~(UINT64_C(0xff) << 32)) |
                ((uint64_t)nasid << 32));
        }
        for (unsigned cpu = 0; cpu < cpus; cpu++) {
            for (unsigned view = 0; view < G_N_ELEMENTS(views); view++) {
                for (unsigned i = 0; i < G_N_ELEMENTS(state); i++) {
                    g_test_message("compute UART nasid %u CPU %u view %u "
                                   "register 0x%x", nasid, cpu, view,
                                   state[i].offset);
                    g_assert_true(qtest_memread_cpu(qts, cpu,
                        views[view] + state[i].offset, bytes, sizeof(bytes)));
                    g_assert_cmphex(ldq_be_p(bytes), ==, state[i].value);
                }
            }
        }
    }
    /* Named writes and reader-local reads reach the same owned latch. */
    stq_be_p(bytes, 0xa5);
    g_assert_true(qtest_memwrite_cpu(qts, cpus - 1,
        named + (UINT64_C(9) << NASID_TEST_NODE_ADDRESS_SHIFT) + 0x38,
        bytes, sizeof(bytes)));
    for (unsigned cpu = 0; cpu < cpus; cpu++) {
        g_assert_true(qtest_memread_cpu(qts, cpu, local + 0x38,
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0xa5);
    }
    /* An incomplete frame exercises transmit readiness without a reply. */
    stq_be_p(bytes, 0x7e);
    g_assert_true(qtest_memwrite_cpu(qts, 0, local, bytes, sizeof(bytes)));
    g_assert_true(qtest_memread_cpu(qts, cpus - 1,
        named + (UINT64_C(9) << NASID_TEST_NODE_ADDRESS_SHIFT) + 0x28,
        bytes, sizeof(bytes)));
    g_assert_cmphex(ldq_be_p(bytes), ==, 0x60);
    qtest_quit(qts);
    if (image) {
        g_assert_cmpint(unlink(image), ==, 0);
    }
    g_assert_cmpint(unlink(prom), ==, 0);
}

static void test_expansion_pending_registers(gconstpointer opaque)
{
    unsigned cpus = GPOINTER_TO_UINT(opaque);
    g_autofree char *image = expansion_storage_image(16 * 1024 * 1024);
    g_autofree char *prom = expansion_prom();
    uint8_t bytes[8];
    QTestState *qts = qtest_initf(
        "-M origin300,topology="
        "origin300-pci-expansion-pending-register-experiment,%s "
        "-cpu R14000 -smp %u -m 512M -S "
        "-drive if=none,id=cfg,file=%s,format=raw,readonly=off "
        "-bios %s -nodefaults -display none",
        STORAGE_BINDING, cpus, image, prom);

    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    for (unsigned pi = 0; pi < 2; pi++) {
        for (unsigned bank = 0; bank < 2; bank++) {
            uint64_t base = UINT64_C(0x20201800000) + pi * 0x200000;
            unsigned bit = 16 + pi + bank * 2;

            stq_be_p(bytes, 0x100 | bank * 64 | bit);
            g_assert_true(qtest_memwrite_cpu(qts, 0, base + 0x90,
                                            bytes, sizeof(bytes)));
            for (unsigned cpu = 0; cpu < cpus; cpu++) {
                g_assert_true(qtest_memread_cpu(qts, cpu, base + 0x98 +
                                               bank * 8, bytes, sizeof(bytes)));
                g_assert_cmphex(ldq_be_p(bytes), ==, UINT64_C(1) << bit);
                g_assert_true(qtest_memread_cpu(qts, cpu, base + 0x40,
                                               bytes, sizeof(bytes)));
                g_assert_cmphex(ldq_be_p(bytes), ==, 0);
            }
        }
    }
    qtest_quit(qts);
    g_assert_cmpint(unlink(image), ==, 0);
    g_assert_cmpint(unlink(prom), ==, 0);
}

static void test_expansion_unconnected_uart(gconstpointer opaque)
{
    unsigned cpus = GPOINTER_TO_UINT(opaque);
    g_autofree char *image = expansion_storage_image(16 * 1024 * 1024);
    g_autofree char *prom = expansion_prom();
    const uint64_t uart = UINT64_C(0x10230000080);
    uint8_t bytes[8];
    QTestState *qts = qtest_initf(
        "-M origin300,topology="
        "origin300-pci-expansion-unconnected-uart-experiment,%s "
        "-cpu R14000 -smp %u -m 512M -S "
        "-drive if=none,id=cfg,file=%s,format=raw,readonly=off "
        "-bios %s -nodefaults -display none",
        STORAGE_BINDING, cpus, image, prom);

    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    stq_be_p(bytes, UINT64_C(0x1122334455667788));
    g_assert_true(qtest_memwrite_cpu(qts, 0, uart + 7 * 8,
                                    bytes, sizeof(bytes)));
    for (unsigned cpu = 0; cpu < cpus; cpu++) {
        g_assert_true(qtest_memread_cpu(qts, cpu, uart + 7 * 8,
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0x88);
        g_assert_true(qtest_memread_cpu(qts, cpu, uart + 6 * 8,
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0xb0);
        g_assert_true(qtest_memread_cpu(qts, cpu,
                                       UINT64_C(0x102100000b8),
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0);
    }
    qtest_quit(qts);
    g_assert_cmpint(unlink(image), ==, 0);
    g_assert_cmpint(unlink(prom), ==, 0);
}

static void test_expansion_storage_composed_read(void)
{
    g_autofree char *image = expansion_storage_image(16 * 1024 * 1024);
    g_autofree char *prom = expansion_prom();
    g_autofree char *compute = expansion_storage_image(16 * 1024 * 1024);
    uint8_t bytes[8];
    int fd = open(image, O_WRONLY);
    QTestState *qts;

    stq_be_p(bytes, UINT64_C(0xfedcba9876543210));
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pwrite(fd, bytes, sizeof(bytes), 0x9e0000),
                    ==, sizeof(bytes));
    g_assert_cmpint(close(fd), ==, 0);
    fd = open(compute, O_WRONLY);
    g_assert_cmpint(fd, >=, 0);
    stq_be_p(bytes, UINT64_C(0x0123456789abcdef));
    g_assert_cmpint(pwrite(fd, bytes, sizeof(bytes), 0x9e0000),
                    ==, sizeof(bytes));
    g_assert_cmpint(close(fd), ==, 0);
    fd = open(image, O_WRONLY);
    g_assert_cmpint(fd, >=, 0);
    memset(bytes, 0xff, sizeof(bytes));
    g_assert_cmpint(pwrite(fd, bytes, sizeof(bytes), 0x820000),
                    ==, sizeof(bytes));
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf("-M %s,%s -cpu R14000 -smp 2 -m 512M -S "
        "-drive if=none,id=cfg,file=%s,format=raw,readonly=off "
        "-drive if=pflash,file=%s,format=raw,readonly=off "
        "-nodefaults -display none", STORAGE_TOPOLOGY,
        STORAGE_BINDING, image, compute);
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    g_assert_cmphex(qtest_readq(qts, UINT64_C(0x23a1e0000)), ==,
                    UINT64_C(0xfedcba9876543210));
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        g_assert_true(qtest_memread_cpu(qts, cpu, UINT64_C(0x1023a1e0000),
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, UINT64_C(0xfedcba9876543210));
        g_assert_true(qtest_memread_cpu(qts, cpu, UINT64_C(0x1043a1e0000),
                                       bytes, sizeof(bytes)));
        g_assert_cmphex(ldq_be_p(bytes), ==, 0);
    }
    g_assert_cmphex(qtest_readq(qts, UINT64_C(0x1001a1e0000)), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_writeq(qts, UINT64_C(0x23a002aa8), 0xaa);
    qtest_writeq(qts, UINT64_C(0x23a001550), 0x55);
    qtest_writeq(qts, UINT64_C(0x23a002aa8), 0xa0);
    qtest_writeq(qts, UINT64_C(0x23a080000), 0x1234);
    g_assert_cmphex(qtest_readw(qts, UINT64_C(0x23a020000)), ==, 0x1234);
    g_assert_true(qtest_memread_cpu(qts, 0, UINT64_C(0x1023a020000),
                                   bytes, 2));
    g_assert_cmphex(lduw_be_p(bytes), ==, 0x1234);
    g_assert_cmphex(qtest_readq(qts, UINT64_C(0x1001a1e0000)), ==,
                    UINT64_C(0x0123456789abcdef));
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(7) << 32);
    g_assert_true(qtest_memread_cpu(qts, 0, UINT64_C(0x1023a1e0000),
                                   bytes, sizeof(bytes)));
    g_assert_cmphex(ldq_be_p(bytes), ==, 0);
    g_assert_true(qtest_memread_cpu(qts, 0, UINT64_C(0x10e3a1e0000),
                                   bytes, sizeof(bytes)));
    g_assert_cmphex(ldq_be_p(bytes), ==, UINT64_C(0xfedcba9876543210));
    qtest_quit(qts);
    g_assert_cmpint(unlink(image), ==, 0);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_assert_cmpint(unlink(compute), ==, 0);
}

static void test_expansion_controller_inputs(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *prom = expansion_prom();
    g_autofree char *record = NULL;
    uint8_t bytes[32] = { 1, 2, 0, 0, 0, 0, 0, 0xfd };
    int fd = g_file_open_tmp("expansion-record-XXXXXX.bin", &record, &error);
    QTestState *qts;
    QDict *reply;
    QList *files;
    const QListEntry *entry;
    g_autofree char *backend = NULL;

    g_assert_no_error(error);
    bytes[18] = 4;
    bytes[31] = 0xfc;
    g_assert_cmpint(write(fd, bytes, sizeof(bytes)), ==, sizeof(bytes));
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf("-M sn-builder-expansion-test,activate=true,"
        "controller-board-eeprom.0=1/2=%s,controller-chardev.0=1=expansion "
        "-chardev null,id=expansion -cpu R14000 -bios %s "
        "-nodefaults -display none", record, prom);
    reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/module1/l1',"
        "'property':'board-record-eeprom'}}");
    files = qdict_get_qlist(reply, "return");
    g_assert_cmpuint(qlist_size(files), ==, 4);
    entry = qlist_first(files);
    for (unsigned i = 0; i < 4; i++, entry = qlist_next(entry)) {
        const char *value = qstring_get_str(qobject_to(QString,
                                                      qlist_entry_obj(entry)));
        g_assert_cmpstr(value, ==, i == 2 ? record : "");
    }
    qobject_unref(reply);
    backend = qom_get_str(qts, "/machine/module1/l1", "chardev");
    g_assert_cmpstr(backend, ==, "expansion");
    qtest_quit(qts);
    g_unlink(record);
    g_unlink(prom);
}

#define SERIAL_UART_BASE UINT64_C(0x40000000)

static void expansion_serial_send(QTestState *qts, unsigned issuer,
                                  uint32_t destination, uint16_t command,
                                  uint8_t channel, const char *text)
{
    uint8_t request[128] = { channel };
    size_t length = 8;

    stl_be_p(request + 1, destination);
    stw_be_p(request + 5, command);
    if (text) {
        request[7] = 1;
        request[8] = 1;
        memcpy(request + 9, text, strlen(text) + 1);
        length += strlen(text) + 2;
    }
    sgi_l1_test_send(qts, SERIAL_UART_BASE + issuer * 0x1000,
                     request, length, false);
}

static void expansion_serial_expect(QTestState *qts, unsigned issuer,
                                    unsigned serving, uint8_t channel,
                                    const char *serial)
{
    uint8_t reply[131], expected[64] = { 0x20 | channel };
    size_t length = 6;

    expansion_serial_send(qts, issuer,
                          (1u << 18) | ((serving + 1) << 12) | 6,
                          4, channel, NULL);
    if (serial) {
        expected[5] = 1;
        expected[6] = 1;
        memcpy(expected + 7, serial, strlen(serial) + 1);
        length = strlen(serial) + 8;
    } else {
        stl_be_p(expected + 1, -104);
    }
    g_assert_cmpuint(sgi_l1_test_receive(qts,
        SERIAL_UART_BASE + issuer * 0x1000, reply, sizeof(reply)), ==, length);
    g_assert_cmpmem(reply, length, expected, length);
    g_assert_cmphex(qtest_readq(qts,
        SERIAL_UART_BASE + (issuer ^ 1) * 0x1000 + 0x28), ==, 0x60);
}

static void expansion_lifecycle_command(QTestState *qts, const char *text)
{
    uint8_t reply[256];

    expansion_serial_send(qts, 1, 0x0ffff003, 0, 16, text);
    g_assert_cmpuint(sgi_l1_test_receive(qts, SERIAL_UART_BASE + 0x1000,
                                        reply, sizeof(reply)), >=, 6);
    g_assert_cmphex(ldl_be_p(reply + 1), ==, 0);
}

static QTestState *expansion_lifecycle_start(const char *prom,
                                             const char *storage,
                                             const char *compute,
                                             const char *extra)
{
    QTestState *qts = qtest_initf(
        "-M origin300,topology="
        "origin300-pci-expansion-pending-register-experiment," STORAGE_BINDING
        ",controller-brick-serial.0=1=EXP-LIFECYCLE "
        "-device sgi-l1-uart,id=serial-expansion,controller=/machine/module1/l1 "
        "-device sgi-bridge-dma-test,bus=expansion0-u0-head-e.0,addr=1 "
        "-cpu R14000 -smp 2 -m 512M -S "
        "-drive if=none,id=cfg,file=%s,format=raw,readonly=off "
        "-drive if=pflash,file=%s,format=raw,readonly=off "
        "-nodefaults -display none %s", storage, compute, extra);

    qtest_sysbus_mmio_map(qts, "/machine/node0/l1-uart", 0,
                          SERIAL_UART_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/serial-expansion", 0,
                          SERIAL_UART_BASE + 0x1000);
    return qts;
}

static void expansion_lifecycle_seed(const char *path)
{
    const uint8_t erased[] = { 0xff, 0xff };
    int fd = open(path, O_WRONLY);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pwrite(fd, erased, sizeof(erased), 0x800400),
                    ==, sizeof(erased));
    g_assert_cmpint(close(fd), ==, 0);
}

static void expansion_lifecycle_program(QTestState *qts, uint64_t base,
                                         uint16_t value)
{
    qtest_writeq(qts, base + 0x2aa8, 0xaa);
    qtest_writeq(qts, base + 0x1550, 0x55);
    qtest_writeq(qts, base + 0x2aa8, 0xa0);
    qtest_writeq(qts, base + 0x1000, value);
    g_assert_cmphex(qtest_readw(qts, base + 0x400), ==, value);
}

static unsigned expansion_lifecycle_log_count(const char *path,
                                              const char *message)
{
    g_autofree char *contents = NULL;
    const char *cursor;
    unsigned count = 0;

    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    cursor = contents;
    while ((cursor = strstr(cursor, message))) {
        count++;
        cursor += strlen(message);
    }
    return count;
}

static void test_expansion_composed_lifecycle(gconstpointer opaque)
{
    bool migrate = GPOINTER_TO_UINT(opaque);
    g_autofree char *prom = expansion_prom();
    g_autofree char *storage = expansion_storage_image(16 * 1024 * 1024);
    g_autofree char *compute = expansion_storage_image(16 * 1024 * 1024);
    g_autofree char *target_storage = expansion_storage_image(16 * 1024 * 1024);
    g_autofree char *target_compute = expansion_storage_image(16 * 1024 * 1024);
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("expansion-lifecycle-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *source_log = g_build_filename(dir, "source.txt", NULL);
    g_autofree char *target_log = g_build_filename(dir, "target.txt", NULL);
    g_autofree char *source_options = g_strdup_printf(
        "-trace enable=sgi_l1_bic_startup_operation "
        "-trace enable=sgi_xbridge_irq_packet -d guest_errors -D %s",
        source_log);
    g_autofree char *target_options = g_strdup_printf(
        "-incoming defer -trace enable=sgi_l1_bic_startup_operation "
        "-trace enable=sgi_xbridge_irq_packet "
        "-d guest_errors -D %s", target_log);
    const uint64_t expansion = UINT64_C(0xe3a000000);
    const uint64_t local = UINT64_C(0x1001a000000);
    const uint64_t head = SPACE_IO + (UINT64_C(7) << 33) +
                          (UINT64_C(0xe) << 24);
    const uint64_t pending = SPACE_IO + 0x1800098;
    QTestState *from;
    QTestState *qts;

    expansion_lifecycle_seed(storage);
    expansion_lifecycle_seed(compute);
    from = expansion_lifecycle_start(prom, storage, compute, source_options);
    qts = from;

    g_assert_no_error(error);
    expansion_vector(from, EXPANSION_LB_REVISION, true, UINT64_C(7) << 32);
    expansion_lifecycle_program(from, expansion, 0x3456);
    expansion_lifecycle_program(from, local, 0x789a);
    qtest_writeq(from, expansion + 0x2a8, 0x98);
    qtest_writeq(from, local + 0x2a8, 0x98);
    g_assert_cmphex(qtest_readw(from, expansion + 0x20), ==, 'Q');
    g_assert_cmphex(qtest_readw(from, local + 0x20), ==, 'Q');
    expansion_lifecycle_command(from, "brick slot 3");
    g_assert_cmpuint(qom_get_uint(from, "/machine/module1/l1", "bay"), ==, 2);
    g_assert_cmphex(qtest_readw(from, UINT64_C(0xe35000000)), ==, 0x42);
    /* Retain one asserted PCI input across power-down and migration. */
    expansion_card_write(from, head + 0x24,
        (expansion_card_read(from, head + 0x24, 4) & ~15U) | 0xe, 4);
    expansion_card_write(from, head + 0x21010, CARD_MMIO + 0x200000, 4);
    expansion_card_write(from, head + 0x21004, PCI_COMMAND_MEMORY_MASTER, 4);
    expansion_card_write(from, head + 0x20c, 0x81012, 4);
    expansion_card_write(from, head + 0x34, (0xa << 16) | 0x8000, 4);
    expansion_card_write(from, head + 0x3c, HUB_INT_MODIFY_OFFSET, 4);
    expansion_card_write(from, head + 0x13c,
                         (INT_HOST_PACKET_IO_NASID0 << 8) | 16, 4);
    expansion_card_write(from, head + 0x10c, 1U << 1, 4);
    expansion_card_write(from, head + 0x11c, 1U << 1, 4);
    expansion_card_write(from, head + 0x400000 + DMA_REG_INTERRUPT, 1, 4);
    g_assert_cmphex(expansion_card_read(from, pending, 8), ==, 1U << 16);
    g_assert_cmpuint(expansion_lifecycle_log_count(source_log,
        "sgi_xbridge_irq_packet"), ==, 1);
    expansion_lifecycle_command(from, "pwr d");
    expansion_serial_expect(from, 1, 1, 21, "EXP-LIFECYCLE");

    if (migrate) {
        unsigned startup_count, refused_count, irq_count;

        qts = expansion_lifecycle_start(prom, target_storage, target_compute,
                                        target_options);
        startup_count = expansion_lifecycle_log_count(target_log,
                                            "sgi_l1_bic_startup_operation");
        g_assert_cmpuint(startup_count, >=, 2);
        irq_count = expansion_lifecycle_log_count(target_log,
                                                 "sgi_xbridge_irq_packet");
        g_assert_cmpuint(irq_count, ==, 0);
        qtest_qmp_assert_success(qts,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration_status(qts, "completed", 30);
        g_assert_cmpuint(expansion_lifecycle_log_count(target_log,
            "sgi_l1_bic_startup_operation"), ==, startup_count);
        g_assert_cmpuint(expansion_lifecycle_log_count(target_log,
            "sgi_xbridge_irq_packet"), ==, irq_count);
        g_assert_cmphex(expansion_card_read(qts, pending, 8), ==, 1U << 16);
        expansion_lifecycle_command(qts, "pwr d");
        g_assert_cmpuint(expansion_lifecycle_log_count(target_log,
            "sgi_xbridge_irq_packet"), ==, irq_count);
        refused_count = expansion_lifecycle_log_count(target_log,
                                                     "main power off");
        expansion_serial_expect(qts, 1, 1, 21, "EXP-LIFECYCLE");
        g_assert_cmpuint(qom_get_uint(qts, "/machine/module1/l1", "bay"),
                         ==, 2);
        /* Off access cannot reset the retained query state. */
        g_assert_cmphex(qtest_readw(qts, expansion + 0x20), ==, 0);
        qtest_writeq(qts, expansion + 0x2aa8, 0xf0);
        g_assert_cmphex(qtest_readw(qts, UINT64_C(0xe35000000)), ==, 0);
        g_assert_cmphex(qtest_readl(qts, SPACE_IO + (UINT64_C(7) << 33) +
                                    UINT64_C(0x08000004)), ==, 0);
        g_assert_cmpuint(expansion_lifecycle_log_count(target_log,
            "main power off"), >=, refused_count + 4);
        g_assert_cmphex(qtest_readw(qts, local + 0x20), ==, 'Q');
        expansion_lifecycle_command(qts, "pwr u");
        g_assert_cmpuint(expansion_lifecycle_log_count(target_log,
            "sgi_xbridge_irq_packet"), ==, irq_count + 1);
        expansion_lifecycle_command(qts, "pwr u");
        g_assert_cmpuint(expansion_lifecycle_log_count(target_log,
            "sgi_xbridge_irq_packet"), ==, irq_count + 1);
        g_assert_cmphex(expansion_card_read(qts, pending, 8), ==, 1U << 16);
        g_assert_cmpuint(expansion_lifecycle_log_count(target_log,
            "sgi_l1_bic_startup_operation"), ==, startup_count);
        g_assert_cmphex((expansion_vector(qts, EXPANSION_LB_REVISION,
                                          false, 0) >> 32) & 0xff, ==, 7);
        g_assert_cmphex(qtest_readw(qts, UINT64_C(0xe35000000)), ==, 0x42);
        g_assert_cmphex(qtest_readw(qts, expansion + 0x20), ==, 'Q');
        qtest_writeq(qts, expansion + 0x2aa8, 0xf0);
        g_assert_cmphex(qtest_readw(qts, expansion + 0x400), ==, 0x3456);
        g_assert_cmphex(qtest_readw(qts, local + 0x20), ==, 'Q');
        /* Controller reboot applies pending settings while flash stays off. */
        expansion_lifecycle_command(qts, "pwr d");
        expansion_lifecycle_command(qts, "reboot_l1");
        g_assert_cmphex(qtest_readw(qts, expansion + 0x400), ==, 0);
        expansion_lifecycle_command(qts, "pwr u");
        g_assert_cmphex(qtest_readw(qts, UINT64_C(0xe35000000)), ==, 0x43);
        g_assert_cmphex(qtest_readw(qts, expansion + 0x400), ==, 0x3456);
    }
    qtest_system_reset(qts);
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    g_assert_cmphex(qtest_readw(qts, UINT64_C(0x235000000)), ==, 0x43);
    g_assert_cmphex(qtest_readw(qts, UINT64_C(0x23a000400)), ==, 0x3456);
    g_assert_cmphex(qtest_readw(qts, UINT64_C(0x23a000020)), ==, 0);
    g_assert_cmphex(qtest_readw(qts, local + 0x400), ==, 0x789a);
    g_assert_cmphex(qtest_readw(qts, local + 0x20), ==, 0);
    qtest_quit(qts);
    if (migrate) {
        qtest_quit(from);
    }
    g_unlink(socket);
    g_unlink(source_log);
    g_unlink(target_log);
    g_rmdir(dir);
    g_unlink(prom);
    g_unlink(storage);
    g_unlink(compute);
    g_unlink(target_storage);
    g_unlink(target_compute);
}

static void test_expansion_controller_serial(const void *data)
{
    static const struct {
        const char *options;
        const char *compute;
        const char *expansion;
    } cases[] = {
        { "-M origin300,topology=origin300-pci-expansion-experiment,"
          "controller-brick-serial.0=1=EXP-A~} "
          "-global sgi-l1.brick-serial=COMPUTE-A", "COMPUTE-A", "EXP-A~}" },
        { "-M origin300,topology=origin300-pci-expansion-experiment",
          NULL, NULL },
        { "-M origin300,topology=origin300-pci-expansion-experiment "
          "-global sgi-l1.brick-serial=SHARED", "SHARED", "SHARED" },
        { "-M origin300,topology=origin300-pci-expansion-experiment,"
          "controller-brick-serial.0=1=SHARED "
          "-global sgi-l1.brick-serial=SHARED", "SHARED", "SHARED" },
        { "-M origin300,topology=origin300-pci-expansion-experiment,"
          "controller-brick-serial.0=1=12345678901234567890123456789012",
          NULL, "12345678901234567890123456789012" },
        { "-M origin300,topology=origin300-pci-expansion-experiment,"
          "controller-brick-serial.0=1=part=serial", NULL, "part=serial" },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree char *prom = expansion_prom();
        QTestState *qts = qtest_initf(
            "%s -device sgi-l1-uart,id=serial-expansion,"
            "controller=/machine/module1/l1 -cpu R14000 -smp 2 -m 512M "
            "-bios %s -nodefaults -display none", cases[i].options, prom);
        uint8_t reply[131];

        if (cases[i].compute) {
            g_autofree char *serial = qom_get_str(qts, "/machine/node0/l1",
                                                 "brick-serial");

            g_assert_cmpstr(serial, ==, cases[i].compute);
        }
        if (cases[i].expansion) {
            g_autofree char *serial = qom_get_str(qts, "/machine/module1/l1",
                                                 "brick-serial");

            g_assert_cmpstr(serial, ==, cases[i].expansion);
        }
        qtest_sysbus_mmio_map(qts, "/machine/node0/l1-uart", 0,
                              SERIAL_UART_BASE);
        qtest_sysbus_mmio_map(qts, "/machine/peripheral/serial-expansion", 0,
                              SERIAL_UART_BASE + 0x1000);
        expansion_serial_send(qts, 1, 0x0ffff003, 0, 16, "brick slot 2");
        g_assert_cmpuint(sgi_l1_test_receive(qts, SERIAL_UART_BASE + 0x1000,
            reply, sizeof(reply)), >=, 6);
        g_assert_cmphex(ldl_be_p(reply + 1), ==, 0);
        expansion_serial_send(qts, 1, 0x0ffff003, 0, 16, "reboot_l1");
        g_assert_cmpuint(sgi_l1_test_receive(qts, SERIAL_UART_BASE + 0x1000,
            reply, sizeof(reply)), >=, 6);
        g_assert_cmphex(ldl_be_p(reply + 1), ==, 0);
        g_assert_cmpuint(qom_get_uint(qts, "/machine/module1/l1", "bay"),
                         ==, 2);
        for (unsigned issuer = 0; issuer < 2; issuer++) {
            expansion_serial_expect(qts, issuer, 0, 5, cases[i].compute);
            expansion_serial_expect(qts, issuer, 1, 21, cases[i].expansion);
        }
        if (i == 0) {
            QDict *response = qtest_qmp(qts,
                "{'execute':'qom-set','arguments':{'path':'/machine',"
                "'property':'l1-association0-reachable','value':false}}");
            uint8_t expected[] = { 0x25, 0xff, 0xff, 0xff, 0xff,
                                   0, 0, 0, 8 };

            g_assert_true(qdict_haskey(response, "return"));
            qobject_unref(response);
            expansion_serial_send(qts, 0, (1u << 18) | (2u << 12) | 6,
                                  4, 5, NULL);
            g_assert_cmpuint(sgi_l1_test_receive(qts, SERIAL_UART_BASE,
                reply, sizeof(reply)), ==, sizeof(expected));
            g_assert_cmpmem(reply, sizeof(expected),
                            expected, sizeof(expected));
            response = qtest_qmp(qts,
                "{'execute':'qom-set','arguments':{'path':'/machine',"
                "'property':'l1-association0-reachable','value':true}}");
            g_assert_true(qdict_haskey(response, "return"));
            qobject_unref(response);
            expansion_serial_expect(qts, 0, 1, 21, cases[i].expansion);
        }
        qtest_quit(qts);
        g_unlink(prom);
    }
}

static void test_expansion_serial_refusals(const void *data)
{
    static const struct {
        const char *input;
        const char *message;
    } cases[] = {
        { "bad=SER", "expected owner=serial" },
        { "-1=SER", "expected owner=serial" },
        { "4294967296=SER", "expected owner=serial" },
        { "1/0=SER", "expected owner=serial" },
        { "1=", "expected owner=serial" },
        { "2=SER", "not an expansion L1 owner" },
        { "0=SER", "not an expansion L1 owner" },
        { "1=SER,controller-brick-serial.1=01=OTHER",
          "duplicate controller input" },
        { "1=SER,controller-brick-serial.1=1=SER",
          "duplicate controller input" },
        { "1=123456789012345678901234567890123", "exceeds 32 bytes" },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree char *machine = g_strdup_printf(
            "origin300,topology=origin300-pci-expansion-experiment,"
            "controller-brick-serial.0=%s", cases[i].input);
        const char *args[] = { qtest_qemu_binary(NULL), "-M", machine,
            "-cpu", "R14000", "-smp", "2", "-m", "512M",
            "-display", "none", NULL };

        assert_qemu_rejects(args, cases[i].message);
    }
    {
        const char *args[] = { qtest_qemu_binary(NULL), "-M",
            "origin300,controller-brick-serial.0=1=SER", "-cpu", "R14000",
            "-smp", "2", "-m", "512M", "-display", "none", NULL };

        assert_qemu_rejects(args, "not an expansion L1 owner");
    }
}

static void test_expansion_controller_refusals(const void *data)
{
    static const struct {
        const char *input;
        const char *message;
    } cases[] = {
        { "controller-board-eeprom.0=0/0=/unused",
          "controller 0 is not an expansion L1 owner" },
        { "controller-board-eeprom.0=1/4=/unused",
          "invalid controller input" },
        { "controller-board-eeprom.0=1/0=", "invalid controller input" },
        { "controller-board-eeprom.0=1/0=/unused,"
          "controller-board-eeprom.1=1/0=/other",
          "duplicate controller input" },
        { "controller-board-eeprom.0=1/0=/unused,"
          "controller-board-eeprom.1=1/1=/unused", "controller inputs reuse" },
        { "controller-board-eeprom.0=1/0=/unused,board-eeprom.0=/unused",
          "expansion record reuses compute input" },
        { "controller-chardev.0=1=missing", "does not exist" },
        { "controller-chardev.0=1/0=missing", "invalid controller input" },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree char *machine = g_strdup_printf(
            "sn-builder-expansion-test,activate=true,%s", cases[i].input);
        const char *args[] = { qtest_qemu_binary(NULL), "-M", machine,
            "-cpu", "R14000", "-display", "none", NULL };

        assert_qemu_rejects(args, cases[i].message);
    }
}

static void test_expansion_management_migration(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *prom = expansion_prom();
    g_autofree char *dir =
        g_dir_make_tmp("expansion-management-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = qtest_initf("-M sn-builder-expansion-test,activate=true "
        "-cpu R14000 -bios %s -nodefaults -display none", prom);
    QTestState *to = qtest_initf("-M sn-builder-expansion-test,activate=true "
        "-cpu R14000 -bios %s -nodefaults -display none -incoming defer", prom);
    QDict *reply;

    g_assert_no_error(error);
    qtest_qmp_assert_success(from,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'l1-association0-reachable','value':false}}");
    expansion_vector(from, EXPANSION_LB_REVISION, true, UINT64_C(7) << 32);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    reply = qtest_qmp(to,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'l1-association0-reachable'}}");
    g_assert_false(qdict_get_bool(reply, "return"));
    qobject_unref(reply);
    g_assert_cmphex((expansion_vector(to, EXPANSION_LB_REVISION,
                                     false, 0) >> 32) & 0xff, ==, 7);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_rmdir(dir);
    g_unlink(prom);
}

static void expansion_cpu_readq(QTestState *qts, unsigned cpu, uint64_t address,
                                 uint64_t expected)
{
    g_autofree char *command = g_strdup_printf("xp/1gx 0x%" PRIx64, address);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute':'human-monitor-command','arguments':"
        "{'command-line':%s,'cpu-index':%u}}", command, cpu);
    const char *text = qdict_get_str(response, "return");
    const char *colon = strchr(text, ':');
    uint64_t value;

    g_assert_null(strstr(text, "Cannot access memory"));
    g_assert_nonnull(colon);
    g_assert_cmpint(sscanf(colon + 1, "%" SCNx64, &value), ==, 1);
    g_assert_cmphex(value, ==, expected);
}

static void test_expansion_cpu_board_load(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t result = 0;
    int fd;
    uint32_t code[] = {
        0x400c6000, /* mfc0 t4, Status */
        0x358c0080, /* ori t4, t4, KX */
        0x408c6000, /* mtc0 t4, Status */
        0x00000000,
        0x3c089000, /* lui t0, 0x9000 */
        0x0008403c, /* dsll32 t0, t0, 0 */
        0x34090002, /* ori t1, zero, 2 */
        0x00094c38, /* dsll t1, t1, 16 */
        0x35293400, /* ori t1, t1, 0x3400 */
        0x00094c38, /* dsll t1, t1, 16 */
        0x01094025, /* or t0, t0, t1 */
        0xdd0a0000, /* ld t2, 0(t0) */
        0x3c089200, /* lui t0, 0x9200 */
        0x0008403c, /* dsll32 t0, t0, 0 */
        0x3c090100, /* lui t1, 0x0100 */
        0x01094025, /* or t0, t0, t1 */
        0xfd0a0108, /* sd t2, PI_RT_COMPARE_A(t0) */
        0x1000ffff, /* beq zero, zero, self */
        0x00000000,
    };

    uint64_t expected = data ? 0x41 : 0xc000;

    if (data) {
        code[8] = 0x35293500; /* ori t1, t1, 0x3500 */
        code[11] = 0x950a0000; /* lhu t2, 0(t0) */
    }
    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    stl_be_p(image, 0x100000ff);
    for (unsigned offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(code); i++) {
        stl_be_p(image + 0x400 + i * 4, code[i]);
    }
    fd = g_file_open_tmp("expansion-cpu-load-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf(
        "-M origin300,topology=origin300-pci-expansion-experiment "
        "-cpu R14000 -bios %s -accel tcg,thread=multi -no-shutdown -S", path);
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    for (unsigned tries = 0; tries < 1000; tries++) {
        result = qtest_readq(qts, EXPANSION_LB_LOCAL + 0x108);
        if (result) {
            break;
        }
        g_usleep(1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    if (result != expected) {
        g_autofree char *registers = qtest_hmp(qts, "info registers");

        g_test_message("CPU registers: %s", registers);
    }
    g_assert_cmphex(result, ==, expected);
    qtest_quit(qts);
    g_unlink(path);
}

static void test_expansion_physical_board_lookup(const void *data)
{
    g_autofree char *prom = expansion_prom();

    for (unsigned compute_number = 0; compute_number < 2; compute_number++) {
        unsigned peer_number = compute_number ^ 1;
        QTestState *qts = qtest_initf(
            "-M origin300,topology=origin300-pci-expansion-experiment "
            "-cpu R14000 -bios %s -nodefaults -display none", prom);
        uint64_t compute_identity = qtest_readq(qts, UINT64_C(0x10014000000));
        uint64_t compute_revision = qtest_readq(qts, EXPANSION_LB_LOCAL +
                                               EXPANSION_LB_REVISION);
        uint64_t peer_revision = expansion_vector(qts, EXPANSION_LB_REVISION,
                                                  false, 0);
        uint64_t physical = ((uint64_t)peer_number << 33) + 0x34000000;

        g_assert_cmphex((compute_revision >> 32) & 0xff, ==, 0);
        g_assert_cmphex((peer_revision >> 32) & 0xff, ==, 0);
        g_assert_cmphex(compute_identity, !=, 0);
        if (compute_number) {
            qtest_writeq(qts, EXPANSION_LB_LOCAL + EXPANSION_LB_REVISION,
                         compute_revision | (UINT64_C(1) << 32));
        } else {
            expansion_vector(qts, EXPANSION_LB_REVISION, true,
                             peer_revision | (UINT64_C(1) << 32));
        }
        g_assert_cmphex((qtest_readq(qts, EXPANSION_LB_LOCAL +
                                    EXPANSION_LB_REVISION) >> 32) & 0xff,
                        ==, compute_number);
        g_assert_cmphex((expansion_vector(qts, EXPANSION_LB_REVISION,
                                         false, 0) >> 32) & 0xff,
                        ==, peer_number);
        g_assert_cmphex(qtest_readq(qts, UINT64_C(0x10014000000)),
                        ==, compute_identity);
        g_assert_cmphex(qtest_readq(qts, physical), ==, 0xc000);
        expansion_cpu_readq(qts, 0, physical, 0xc000);
        expansion_cpu_readq(qts, 1, physical, 0xc000);
        expansion_cpu_readq(qts, 0, UINT64_C(0x10000000000) + physical,
                             0xc000);
        {
            g_autofree char *command = g_strdup_printf(
                "xp/1hx 0x%" PRIx64,
                UINT64_C(0x10000000000) + physical + 0x1000000);
            g_autofree char *reply = qtest_hmp(qts, "%s", command);

            g_assert_null(strstr(reply, "Cannot access memory"));
            g_assert_nonnull(strstr(reply, "0x0041"));
            g_free(command);
            command = g_strdup_printf("xp/1wx 0x%" PRIx64,
                                       UINT64_C(0x10000000000) + physical);
            g_free(reply);
            reply = qtest_hmp(qts, "%s", command);
            g_assert_nonnull(strstr(reply, "Cannot access memory"));
        }
        qtest_quit(qts);
    }
    g_unlink(prom);
}

/* The explicitly selected controller assigns its own board on startup. */
static void test_expansion_controller_startup(const void *data)
{
    g_autofree char *prom = expansion_prom();
    QTestState *qts = qtest_initf(
        "-M origin300,topology=origin300-pci-expansion-experiment "
        "-cpu R14000 -bios %s -nodefaults -display none", prom);
    uint64_t peer_revision = expansion_vector(qts, EXPANSION_LB_REVISION,
                                              false, 0);

    expansion_vector(qts, EXPANSION_LB_REVISION, true,
                     peer_revision | (UINT64_C(1) << 32));
    g_assert_cmphex(qtest_readw(qts, UINT64_C(0x235000000)), ==, 0x41);
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    expansion_vector(qts, EXPANSION_LB_REVISION, true,
                     peer_revision | (UINT64_C(1) << 32));
    g_assert_cmphex(qtest_readw(qts, UINT64_C(0x235000000)), ==, 0x41);
    qtest_quit(qts);
    g_unlink(prom);
}

/* The attached identity must name the same owner through concrete routing. */
static void test_expansion_controller_locations(void)
{
    g_autofree char *prom = expansion_prom();
    g_autofree char *image = expansion_storage_image(16 * 1024 * 1024);
    const uint64_t uart = NASID_TEST_HUB_LOCAL_REGISTERS + 0x80;
    const uint32_t destinations[] = { 0x00041006, 0x4ffff006, 0x00042006 };
    const uint32_t coordinates[] = { 0x41, 0x42, 0x42 };
    const uint32_t types[] = { 0x34, 0x25, 0x25 };
    QTestState *qts = qtest_initf(
        "-M origin300,topology="
        "origin300-pci-expansion-pending-register-experiment," STORAGE_BINDING
        " -cpu R14000 -bios %s -nodefaults -display none "
        "-drive if=none,id=cfg,file=%s,format=raw,readonly=off", prom, image);

    for (unsigned i = 0; i < G_N_ELEMENTS(destinations); i++) {
        uint8_t request[8] = { 0x10 }, reply[32];
        uint8_t expected[16] = { 0x30, 0, 0, 0, 0, 2 };

        stl_be_p(request + 1, destinations[i]);
        stw_be_p(request + 5, 3);
        stl_be_p(expected + 7, coordinates[i]);
        stl_be_p(expected + 12, types[i]);
        sgi_l1_test_send(qts, uart, request, sizeof(request), false);
        g_assert_cmpuint(sgi_l1_test_receive(qts, uart, reply, sizeof(reply)),
                         ==, sizeof(expected));
        g_test_message("destination 0x%08x: coordinates 0x%x, type 0x%x",
                       destinations[i], ldl_be_p(reply + 7),
                       ldl_be_p(reply + 12));
        g_assert_cmpmem(reply, sizeof(expected), expected, sizeof(expected));
    }
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    g_assert_cmphex(qtest_readw(qts, UINT64_C(0x235000000)), ==, 0x42);
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    expansion_vector(qts, EXPANSION_LB_REVISION, true, UINT64_C(1) << 32);
    g_assert_cmphex(qtest_readw(qts, UINT64_C(0x235000000)), ==, 0x42);
    qtest_quit(qts);
    qts = qtest_initf("-M origin300 -cpu R14000 -bios %s "
                      "-nodefaults -display none", prom);
    {
        uint8_t request[] = { 0x10, 0, 4, 0x10, 6, 0, 3, 0 };
        uint8_t expected[] = { 0x30, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0x41,
                               0, 0, 0, 0, 0x34 };
        uint8_t reply[32];

        sgi_l1_test_send(qts, uart, request, sizeof(request), false);
        g_assert_cmpuint(sgi_l1_test_receive(qts, uart, reply, sizeof(reply)),
                         ==, sizeof(expected));
        g_assert_cmpmem(reply, sizeof(expected), expected, sizeof(expected));
    }
    qtest_quit(qts);
    g_unlink(image);
    g_unlink(prom);
}

static void test_expansion_management_mismatch(const void *data)
{
    const char *option = data;
    g_autoptr(GError) error = NULL;
    g_autofree char *prom = expansion_prom();
    g_autofree char *dir =
        g_dir_make_tmp("expansion-mismatch-XXXXXX", &error);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = qtest_initf("-M sn-builder-expansion-test,activate=true "
        "-cpu R14000 -bios %s -nodefaults -display none", prom);
    QTestState *to = qtest_initf(
        "-M sn-builder-expansion-test,activate=true,%s "
        "-cpu R14000 -bios %s -nodefaults -display none -incoming defer",
        option, prom);

    g_assert_no_error(error);
    qtest_set_expected_status(to, EXIT_FAILURE);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    qtest_wait_qemu(to);
    qtest_quit(to);
    qtest_quit(from);
    g_unlink(socket);
    g_rmdir(dir);
    g_unlink(prom);
}

static void test_expansion_builder(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    g_autofree char *path = NULL;
    QTestState *qts;
    int fd = g_file_open_tmp("sn-builder-expansion-prom-XXXXXX.bin", &path,
                             &error);

    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf("-M %s -cpu R14000 -bios %s "
                      "-nodefaults -display none", data ? (const char *)data :
                      "sn-builder-expansion-test,activate=true", path);
    g_assert_cmpuint(qom_get_uint(qts, "/machine/module1/assembly",
                                 "placement"), ==, 1);
    g_assert_cmpuint(qom_get_uint(qts, "/machine/module1/assembly/bic/lb",
                                 "node-number"), ==, 0);
    {
        uint64_t compute = qtest_readq(qts, EXPANSION_LB_LOCAL +
                                      EXPANSION_LB_REVISION);
        uint64_t peer = expansion_vector(qts, EXPANSION_LB_REVISION, false, 0);
        uint64_t assigned = (peer & ~(UINT64_C(0xff) << 32)) |
                            (UINT64_C(7) << 32);

        g_assert_cmphex((compute >> 32) & 0xff, ==, 0);
        g_assert_cmphex((peer >> 32) & 0xff, ==, 0);
        expansion_vector(qts, EXPANSION_LB_REVISION, true, assigned);
        g_assert_cmphex((expansion_vector(qts, EXPANSION_LB_REVISION,
                                         false, 0) >> 32) & 0xff, ==, 7);
        g_assert_cmphex(qtest_readq(qts, EXPANSION_LB_LOCAL +
                                   EXPANSION_LB_REVISION), ==, compute);
        g_assert_cmphex(qtest_readq(qts, SPACE_IO + (UINT64_C(7) << 33) +
                                   0x01800000 + EXPANSION_LB_REVISION),
                        ==, assigned);
        g_assert_cmphex(qtest_readq(qts, UINT64_C(0x30034000000) +
                                   (UINT64_C(7) << 33)),
                        ==, data ? 0xc000 : UINT64_C(0x123400000000c000));
    }
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'l1-association0-reachable','value':false}}");
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_qmp_eventwait(qts, "RESET");
    {
        QDict *reply = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':{'path':'/machine',"
            "'property':'l1-association0-reachable'}}");

        g_assert_true(qdict_get_bool(reply, "return"));
        qobject_unref(reply);
    }
    g_assert_cmphex((expansion_vector(qts, EXPANSION_LB_REVISION,
                                     false, 0) >> 32) & 0xff, ==, 0);
    qtest_quit(qts);
    g_unlink(path);
}

static void test_origin300_two_chassis_l1s_are_peers(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    g_autofree char *path = NULL;
    g_autofree char *peer0 = NULL;
    g_autofree char *peer1 = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn-builder-o300-pair-prom-XXXXXX.bin", &path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf("-M origin300,topology=origin300-two-chassis "
                      "-cpu R14000 -smp 4 -m 1024 -bios %s -nodefaults "
                      "-display none", path);
    peer0 = qom_get_str(qts, "/machine/node0/l1", "peer");
    peer1 = qom_get_str(qts, "/machine/node1/l1", "peer");
    g_assert_cmpstr(peer0, ==, "/machine/node1/l1");
    g_assert_cmpstr(peer1, ==, "/machine/node0/l1");
    qtest_quit(qts);
    g_unlink(path);
}

/*
 * A node-population row the builder cannot make is refused before any
 * node is built: one leaving out the node that controls an I/O board,
 * and one graded unknown, whose evidence does not settle its wiring.
 */
static void test_unbuildable_rows_rejected(const void *data)
{
    const char *console[] = {
        qtest_qemu_binary(NULL), "-M",
        "sn-builder-console-test,activate=on,nodes=1",
        "-m", "128M", "-smp", "1", "-cpu", "R10000",
        "-bios", "/dev/null", "-display", "none", NULL
    };
    const char *unknown[] = {
        qtest_qemu_binary(NULL), "-M",
        "sn-builder-bedrock-test,activate=on,nodes=1",
        "-m", "128M", "-smp", "1", "-bios", "/dev/null",
        "-display", "none", NULL
    };

    assert_qemu_rejects(console, "topology 'sn-builder-console-test': an "
                        "I/O board needs node 1, which a node-population "
                        "row leaves out");
    assert_qemu_rejects(unknown, "topology 'sn-builder-bedrock-test' with 1 "
                        "node board rests on evidence that does not settle "
                        "its wiring; it is not built");
}

static void test_unsupported_memory_population_rejected(const void *data)
{
    /*
 * -m 1024M (single node, default topology) needs a 512 MiB bank 0, which
 * QEMU does not build, so it is not in the accepted list. The refusal
 * names the totals that are.
 */
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-m", "1024M",
        "-display", "none", NULL
    };

    assert_qemu_rejects(args,
                        "topology 'origin200' does not accept 1024 "
                        "MiB per node, only 64, 128, 192, 256, 320, 384, "
                        "448, 512, 576, 640, 704, 768, 832, 896, 1088, "
                        "1152, 1216, 1280, 1600 or 1664 MiB");
}

/*
 * The builder maps the whole board in one memory transaction, so before
 * the machine's first system reset each address space's flat view is
 * rendered a few times however many devices the board has: when the
 * address space is created, at the builder's commit, and at each of the
 * few commits machine creation makes after it (at most seven on an Onyx2).
 * A commit per realized device instead renders the earliest views once for
 * every device built after them, 166 times on an Origin 200 and 708 on an
 * Origin 2000. The flatview_new trace event names each rendered view's
 * root region, and the system reset begins at the root reset container.
 */
#define MAX_RENDERS_PER_VIEW 16

/* Each system reset begins with the root reset container's enter phase. */
#define RESET_STARTS "(resettable-container)"

/*
 * Start the machine with flat-view renders traced, run act, and count each
 * root's renders. With after zero, count those before the first line
 * naming marker; otherwise count those after the after'th such line.
 * Returns the most renders any one root had.
 */
static unsigned int count_renders(const char *machine,
                                  void (*act)(QTestState *qts),
                                  const char *marker, unsigned int after,
                                  GHashTable *renders)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    unsigned int most = 0;
    unsigned int marks = 0;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn-builder-renders-XXXXXX.log", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf("-M %s -display none -trace flatview_new "
                      "-trace resettable_phase_enter_begin "
                      "-trace memory_region_ops_write -D %s",
                      machine, path);
    if (act) {
        act(qts);
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_unlink(path);

    lines = g_strsplit(contents, "\n", -1);
    for (char **line = lines; *line; line++) {
        const char *root;
        unsigned int count;

        if (strstr(*line, marker)) {
            marks++;
            if (!after) {
                break;
            }
            continue;
        }
        if (marks < after || !strstr(*line, "flatview_new ")) {
            continue;
        }
        root = strstr(*line, "(root ");
        g_assert_nonnull(root);
        count = GPOINTER_TO_UINT(g_hash_table_lookup(renders, root)) + 1;
        g_hash_table_insert(renders, g_strdup(root), GUINT_TO_POINTER(count));
        most = MAX(most, count);
    }
    g_assert_cmpuint(marks, >=, MAX(after, 1));
    return most;
}

static GHashTable *new_render_table(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

static void test_one_memory_transaction(const void *data)
{
    g_autoptr(GHashTable) renders = new_render_table();
    unsigned int most;

#ifndef CONFIG_TRACE_LOG
    g_test_skip("flat view renders are traced only by the log backend");
    return;
#endif
    most = count_renders(data, NULL, RESET_STARTS, 0, renders);
    g_assert_cmpuint(g_hash_table_size(renders), >, 1);
    g_assert_cmpuint(most, <=, MAX_RENDERS_PER_VIEW);
}

/*
 * A system reset renders each view once per commit the reset makes, so
 * these cases count the renders that follow the second system-reset start
 * (the first is machine creation's). Each prepares state the reset must
 * undo first, because a region already where reset puts it commits
 * nothing.
 */
typedef struct ResetRenders {
    const char *machine;
    void (*prepare_and_reset)(QTestState *qts);
    unsigned int most;
} ResetRenders;

#define NI_STATUS               0x600000
#define NI_REGION_SIZE          (UINT64_C(1) << 17)

/* Number every Hub of a four-node Origin 2000, then reset. */
static void number_hubs_then_reset(QTestState *qts)
{
    for (unsigned int node = 0; node < 4; node++) {
        uint8_t bytes[8];

        stq_be_p(bytes, NI_REGION_SIZE | ((uint64_t)(0x11 + node) << 8));
        g_assert_true(qtest_memwrite_cpu(qts, 2 * node, HUB_LOCAL + NI_STATUS,
                                         bytes, sizeof(bytes)));
    }
    qtest_system_reset(qts);
}

/* Turn every slot of an Origin 200's Bridge to I/O space, then reset. */
static void bridge_io_slots_then_reset(QTestState *qts)
{
    for (unsigned int slot = 0; slot < 8; slot++) {
        qtest_writel(qts, SMALL_WINDOW(BASEIO_WIDGET) +
                     BRIDGE_DEVICE_REG(slot) + 4, 0);
    }
    qtest_system_reset(qts);
}

static void test_reset_renders(const void *data)
{
    const ResetRenders *c = data;
    g_autoptr(GHashTable) renders = new_render_table();
    unsigned int most;

#ifndef CONFIG_TRACE_LOG
    g_test_skip("flat view renders are traced only by the log backend");
    return;
#endif
    most = count_renders(c->machine, c->prepare_and_reset, RESET_STARTS, 2,
                         renders);
    g_assert_cmpuint(g_hash_table_size(renders), >, 1);
    g_assert_cmpuint(most, <=, c->most);
}

/*
 * Each Hub's reset re-aims its seven big windows. A commit per window
 * renders every view seven times per Hub, 28 on four nodes; one
 * transaction per Hub makes that four. The Routers' reset-exit floods
 * then move each numbered Hub's claim aliases: one commit per Hub, or one
 * in all when the flood is one transaction.
 */
static const ResetRenders numbered_origin2000_reset = {
    .machine = "origin2000 -smp 8 -m 256M",
    .prepare_and_reset = number_hubs_then_reset,
    .most = 5,
};

/*
 * The Bridge's reset puts each slot's window back in memory space, gating
 * one region on and one off per slot: sixteen commits, one once batched.
 * The Hub's big windows add one more.
 */
static const ResetRenders bridge_origin200_reset = {
    .machine = "origin200",
    .prepare_and_reset = bridge_io_slots_then_reset,
    .most = 2,
};

#define NI_VECTOR_PARMS         0x600200
#define NI_VECTOR               0x600208
#define NI_VECTOR_DATA          0x600210
#define NI_VECTOR_STATUS        0x600300
#define NI_VECTOR_CLEAR         0x600380
#define NI_LOCAL_TABLE          0x638100
#define NI_META_TABLE           0x638000
#define ROUTER_PROTECTION       0x10
#define ROUTER_LOCAL_TABLE      0x70100
#define VECTOR_VALID            (UINT64_C(1) << 63)

static const unsigned int module_nasids[] = { 0x11, 0x12, 0x23, 0x24 };

static void hub_write(QTestState *qts, unsigned int node, uint64_t offset,
                      uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 2 * node, HUB_LOCAL + offset,
                                     bytes, sizeof(bytes)));
}

static uint64_t hub_read(QTestState *qts, unsigned int node, uint64_t offset)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, 2 * node, HUB_LOCAL + offset,
                                    bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void vector_write(QTestState *qts, uint64_t route, uint64_t address,
                         uint64_t data)
{
    uint64_t ids = ((UINT64_C(0x30) + route) << 40) | (UINT64_C(0x81) << 32);

    hub_read(qts, 0, NI_VECTOR_CLEAR);
    hub_write(qts, 0, NI_VECTOR_DATA, data);
    hub_write(qts, 0, NI_VECTOR, route);
    hub_write(qts, 0, NI_VECTOR_PARMS, ids | address | 1);
    g_assert_cmphex(hub_read(qts, 0, NI_VECTOR_STATUS) & (VECTOR_VALID | 7),
                    ==, VECTOR_VALID | 1);
}

/*
 * Number and route a four-node Origin 2000 as sn0-baseio-test does: the
 * Routers' tables through node 0's vector port, then each Hub's first
 * turns, so that every Hub reaches every other.
 */
static void program_module(QTestState *qts)
{
    static const uint8_t first_turns[][4] = {
        { 0, 5, 1, 1 }, { 1, 0, 2, 2 }, { 1, 1, 0, 5 }, { 2, 2, 1, 0 },
    };
    static const uint32_t router_entries[][4] = {
        { 0xffffff, 0xffffff, 0x5fffff, 0x4fffff },
        { 0x5fffff, 0x4fffff, 0xffffff, 0xffffff },
    };
    unsigned int from, to;

    for (from = 0; from < 4; from++) {
        hub_write(qts, from, NI_STATUS,
                  NI_REGION_SIZE | ((uint64_t)module_nasids[from] << 8));
    }
    for (from = 0; from < 2; from++) {
        uint64_t route = from ? 6 : 0;

        vector_write(qts, route, ROUTER_PROTECTION, UINT64_C(1) << 12);
        for (to = 0; to < 4; to++) {
            vector_write(qts, route,
                         ROUTER_LOCAL_TABLE + (module_nasids[to] & 15) * 8,
                         router_entries[from][to]);
        }
    }
    for (from = 0; from < 4; from++) {
        for (to = 0; to < 4; to++) {
            bool local = (module_nasids[from] >> 4) ==
                         (module_nasids[to] >> 4);
            uint64_t entry = local ?
                NI_LOCAL_TABLE + (module_nasids[to] & 15) * 8 :
                NI_META_TABLE + (module_nasids[to] >> 4) * 8;

            hub_write(qts, from, entry, first_turns[from][to]);
        }
    }
}

/*
 * A NUMAlink change floods every device the cable graph reaches, and each
 * Hub whose claim aliases move would commit on its own: one render per
 * Hub, four on a programmed four-node Origin 2000. One transaction at the
 * flood's origin makes that one. These cases program the module's routing
 * as sn0-baseio-test does, so that Hubs reach each other, then count the
 * renders after a marker store to a scratch register.
 */
#define NI_SCRATCH_0            0x600100
#define FLOOD_MARKER            "600100 value 0x1 "

static void program_module_then_mark(QTestState *qts)
{
    program_module(qts);
    hub_write(qts, 0, NI_SCRATCH_0, 1);
}

/*
 * A Hub's flood: renumbering node 0 moves every Hub's claim alias for
 * it. A route-table write starts the same flood, but only the writing
 * Hub's own claims follow its tables, so one would commit either way.
 */
static void renumber_hub_flood(QTestState *qts)
{
    program_module_then_mark(qts);
    hub_write(qts, 0, NI_STATUS, NI_REGION_SIZE | ((uint64_t)0x13 << 8));
}

/* A cable's flood: parting the Routers cuts each Hub off from two. */
static void cable_down_flood(QTestState *qts)
{
    program_module_then_mark(qts);
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': '/machine/r1-r2', "
                             "'property': 'down', 'value': true}}");
}

typedef struct FloodRenders {
    void (*flood)(QTestState *qts);
} FloodRenders;

static const FloodRenders hub_number_flood = { renumber_hub_flood };
static const FloodRenders cable_flood = { cable_down_flood };

static void test_flood_renders(const void *data)
{
    const FloodRenders *c = data;
    g_autoptr(GHashTable) renders = new_render_table();
    unsigned int most;

#ifndef CONFIG_TRACE_LOG
    g_test_skip("flat view renders are traced only by the log backend");
    return;
#endif
    most = count_renders("origin2000 -smp 8 -m 256M", c->flood, FLOOD_MARKER,
                         1, renders);
    g_assert_cmpuint(g_hash_table_size(renders), >, 1);
    g_assert_cmpuint(most, ==, 1);
}

/*
 * A device added with -device realizes after the builder has committed,
 * outside its transaction, so the device must open its own: otherwise
 * each region it maps commits alone and renders every view built so far
 * again. These cases count the renders before the first system reset, as
 * the one-memory-transaction cases do, with such a device on the command
 * line. The bounds are the counts each makes today.
 */
typedef struct DeviceRenders {
    const char *machine;
    const char *device;
    unsigned int most;
} DeviceRenders;

static void test_device_renders(const void *data)
{
    const DeviceRenders *c = data;
    g_autoptr(GHashTable) renders = new_render_table();
    unsigned int most;

#ifndef CONFIG_TRACE_LOG
    g_test_skip("flat view renders are traced only by the log backend");
    return;
#endif
    if (!qtest_has_device(c->device)) {
        g_test_skip("the device is not built in");
        return;
    }
    most = count_renders(c->machine, NULL, RESET_STARTS, 0, renders);
    g_assert_cmpuint(g_hash_table_size(renders), >, 1);
    g_assert_cmpuint(most, <=, c->most);
}

/* An XBridge: its crossbar's ports and routes and its two heads. */
#define CHIP_ARGS "none -cpu R10000 -device sgi-xio-test-target,id=fabric " \
    "-device sgi-xbridge,id=xbridge,host-port=0xb,xio-port=0xc," \
    "fabric=/machine/peripheral/fabric/fabric.ram[0]"

static const DeviceRenders xbridge_renders = { CHIP_ARGS, "sgi-xbridge", 3 };

/* An IOC3 on one of the XBridge's heads. */
static const DeviceRenders ioc3_renders = {
    CHIP_ARGS " -device sgi-ioc3,bus=head-e.0,addr=0", "sgi-ioc3", 5,
};

/* A MENET board in an Origin 2000's IO4 slot. */
static const DeviceRenders menet_renders = {
    "origin2000 -device sgi-xio-baseio,preset=menet,slot=io4",
    "sgi-xio-baseio", 11,
};

/* The test-only assembly of three XBridges. */
static const DeviceRenders assembly_renders = {
    "none -cpu R10000 -device sgi-xbridge-assembly-fixture,"
    "chip1-e-port=8,chip1-f-port=9,chip2-e-port=0xc,chip2-f-port=0xd",
    "sgi-xbridge-assembly-fixture", 2,
};

/* The test-only board fixture's two cascaded XBridges. */
static const DeviceRenders board_fixture_renders = {
    "none -cpu R10000 -device sgi-board-test-fixture,"
    "board-type=xbridge-cascade",
    "sgi-board-test-fixture", 2,
};

/*
 * The test-only PCI expansion fixture: a compute Bedrock and a whole
 * expansion module.
 */
static const DeviceRenders ni_fixture_renders = {
    "none -cpu R10000 -device sgi-pci-expansion-ni-fixture",
    "sgi-pci-expansion-ni-fixture", 8,
};

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn/builder/numa-refused", NULL,
                        test_numa_refused);
    qtest_add_data_func("sn/builder/one-memory-transaction/origin200",
                        "origin200",
                        test_one_memory_transaction);
    qtest_add_data_func("sn/builder/one-memory-transaction/origin2000",
                        "origin2000",
                        test_one_memory_transaction);
    qtest_add_data_func("sn/builder/reset-renders/numbered-origin2000",
                        &numbered_origin2000_reset, test_reset_renders);
    qtest_add_data_func("sn/builder/reset-renders/bridge-origin200",
                        &bridge_origin200_reset, test_reset_renders);
    qtest_add_data_func("sn/builder/flood-renders/hub-number",
                        &hub_number_flood, test_flood_renders);
    qtest_add_data_func("sn/builder/flood-renders/cable-down",
                        &cable_flood, test_flood_renders);
    qtest_add_data_func("sn/builder/device-renders/xbridge",
                        &xbridge_renders, test_device_renders);
    qtest_add_data_func("sn/builder/device-renders/ioc3", &ioc3_renders,
                        test_device_renders);
    qtest_add_data_func("sn/builder/device-renders/menet", &menet_renders,
                        test_device_renders);
    qtest_add_data_func("sn/builder/device-renders/xbridge-assembly",
                        &assembly_renders, test_device_renders);
    qtest_add_data_func("sn/builder/device-renders/board-fixture",
                        &board_fixture_renders, test_device_renders);
    qtest_add_data_func("sn/builder/device-renders/ni-fixture",
                        &ni_fixture_renders, test_device_renders);
    qtest_add_data_func("sn/builder/pci-expansion-requires-fabric", NULL,
                        test_pci_expansion_requires_fabric);
    qtest_add_data_func("sn/builder/expansion-boards-share-the-attach-"
                        "contract", NULL,
                        test_expansion_boards_share_the_attach_contract);
    qtest_add_data_func("sn/builder/named-slot-refusals", NULL,
                        test_named_slot_refusals);
    qtest_add_data_func("sn/builder/named-slot-takes-one-board", NULL,
                        test_named_slot_takes_one_board);
    qtest_add_data_func("sn/builder/explicit-fabric-must-match-its-slot",
                        NULL, test_explicit_fabric_must_match_its_slot);
    qtest_add_data_func("sn/builder/pci-expansion-attaches-at-a-named-slot",
                        NULL, test_pci_expansion_attaches_at_a_named_slot);
    qtest_add_data_func("sn/builder/xio-baseio-fits-only-io1", NULL,
                        test_xio_baseio_fits_only_io1);
    qtest_add_data_func("sn/builder/dropped-read-to-illegal-widget-times-out",
                        NULL, test_dropped_read_to_illegal_widget_times_out);
    qtest_add_data_func("sn/builder/dropped-read-to-empty-widget-times-out",
                        NULL, test_dropped_read_to_empty_widget_times_out);
    qtest_add_data_func("sn/builder/dropped-write-records-nothing", NULL,
                        test_dropped_write_records_nothing);
    qtest_add_data_func("sn/builder/pci-expansion-delivers-through-spare-"
                        "fabric", NULL,
                        test_pci_expansion_delivers_through_spare_fabric);
    qtest_add_data_func("sn/builder/pci-expansion-is-inbound-addressable",
                        NULL, test_pci_expansion_is_inbound_addressable);
    qtest_add_data_func("sn/builder/pci-expansion-delivers-card-interrupt-"
                        "to-hub", NULL,
                        test_pci_expansion_delivers_card_interrupt_to_hub);
    qtest_add_data_func("sn/builder/expansion-card-enumerates-inbound",
                        NULL, test_expansion_card_enumerates_inbound);
    qtest_add_data_func("sn/builder/spare-widget-present-only-when-claimed",
                        NULL, test_spare_widget_present_only_when_claimed);
    qtest_add_data_func("sn/builder/claimed-spare-widget-survives-link-reset",
                        NULL, test_claimed_spare_widget_survives_link_reset);
    qtest_add_data_func("sn/builder/board-selection-refusals", NULL,
                        test_board_selection_refusals);
    qtest_add_data_func("sn/builder/pic-io9-descriptor-refusals", NULL,
                        test_pic_io9_descriptor_refusals);
    qtest_add_data_func("sn/builder/unsupported-smp-population-rejected",
                        NULL, test_unsupported_smp_population_rejected);
    qtest_add_data_func("sn/builder/topology-from-another-product-refused",
                        NULL, test_topology_from_another_product_refused);
    qtest_add_data_func("sn/builder/console-node-gets-serial-line-zero", NULL,
                        test_console_field_puts_its_node_on_serial_line_zero);
    qtest_add_data_func("sn/builder/node-without-xio-attachment-rejected",
                        NULL, test_node_without_xio_attachment_rejected);
    qtest_add_data_func("sn/builder/unsupported-memory-population-rejected",
                        NULL, test_unsupported_memory_population_rejected);
    qtest_add_data_func("sn/builder/unbuildable-rows-rejected", NULL,
                        test_unbuildable_rows_rejected);
    qtest_add_data_func("sn/builder/bedrock-topology-accepts-two-nodes",
                        NULL, test_bedrock_topology_accepts_two_nodes);
    qtest_add_data_func("sn/builder/ip53-builds-linked-pic-and-io9-d1-h0",
                        "ip53-pic-io9-d1-h0",
                        test_ip53_builds_linked_pic_and_io9);
    qtest_add_data_func("sn/builder/ip53-builds-linked-pic-and-io9-d4-h0",
                        "ip53-pic-io9-d4-h0",
                        test_ip53_builds_linked_pic_and_io9);
    qtest_add_data_func("sn/builder/ip53-builds-linked-pic-and-io9-d1-h1",
                        "ip53-pic-io9-d1-h1",
                        test_ip53_builds_linked_pic_and_io9);
    qtest_add_data_func("sn/builder/ip53-builds-linked-pic-and-io9-d4-h1",
                        "ip53-pic-io9-d4-h1",
                        test_ip53_builds_linked_pic_and_io9);
    qtest_add_data_func("sn/builder/ip53-identical-fingerprint-migration",
                        "ip53-pic-io9", test_ip53_fingerprint_migration);
    qtest_add_data_func("sn/builder/ip53-fingerprint-ioc4-route-mismatch",
                        "ip53-pic-io9-ioc4-input0",
                        test_ip53_fingerprint_migration);
    qtest_add_data_func("sn/builder/ip53-fingerprint-clock-mismatch",
                        "ip53-alt-clock", test_ip53_fingerprint_migration);
    qtest_add_data_func("sn/builder/ip53-fingerprint-board-id-mismatch",
                        "ip53-alt-board-id", test_ip53_fingerprint_migration);
    qtest_add_data_func("sn/builder/ip53-fingerprint-l1-type-mismatch",
                        "ip53-alt-l1-type", test_ip53_fingerprint_migration);
    qtest_add_data_func("sn/builder/ip53-fingerprint-head-mismatch",
                        "ip53-pic-io9-d1-h1",
                        test_ip53_fingerprint_migration);
    qtest_add_data_func("sn/builder/ip53-fingerprint-devfn-mismatch",
                        "ip53-pic-io9-d4-h0",
                        test_ip53_fingerprint_migration);
    qtest_add_data_func("sn/builder/ip53-fingerprint-ni-revision-mismatch",
                        "ip53-ni3-widget2",
                        test_ip53_fingerprint_migration);
    qtest_add_data_func("sn/builder/ip53-fingerprint-widget-revision-mismatch",
                        "ip53-ni2-widget3",
                        test_ip53_fingerprint_migration);
    qtest_add_data_func("sn/builder/ip53-pxbow-identical-revision-migration",
                        "revision5", test_ip53_runtime_pxbow_migration);
    qtest_add_data_func("sn/builder/ip53-pxbow-revision-4-mismatch",
                        "revision4", test_ip53_runtime_pxbow_migration);
    qtest_add_func("sn/builder/ip53-legacy-composition-migration",
                   test_ip53_legacy_composition_migration);
    qtest_add_data_func("sn/builder/ip53-prior-reset-policy-to-current",
                        "prior-to-current",
                        test_ip53_reset_policy_migration_peer);
    qtest_add_data_func("sn/builder/ip53-current-reset-policy-to-prior",
                        "current-to-prior",
                        test_ip53_reset_policy_migration_peer);
    qtest_add_func("sn/builder/ip53-previous-cpu-pio-migration",
                   test_ip53_previous_cpu_pio_migration);
    qtest_add_func("sn/builder/ip53-previous-direct64-migration",
                   test_ip53_previous_direct64_migration);
    qtest_add_func("sn/builder/ip53-runtime-permission-refusals",
                   test_ip53_runtime_permission_refusals);
    qtest_add_func(
        "sn/builder/ip53-runtime-permission-defaults-false-and-immutable",
        test_ip53_runtime_permission_defaults_false_and_immutable);
    qtest_add_func("sn/builder/ip53-runtime-construction",
                   test_ip53_runtime_construction);
    qtest_add_func("sn/builder/ip53-integrated-bte-reset-overlap",
                   test_ip53_integrated_bte_reset_overlap);
    qtest_add_func("sn/builder/ip53-static-bcm5701-network-backend",
                   test_ip53_static_bcm5701_network_backend);
    qtest_add_func("sn/builder/ip53-static-bcm5701-nic-none",
                   test_ip53_static_bcm5701_nic_none);
    qtest_add_func("sn/builder/ip53-io9-endpoint-interrupt-routes",
                   test_ip53_io9_endpoint_interrupt_routes);
    qtest_add_func("sn/builder/ip53-l1-reported-type",
                   test_ip53_l1_reported_type);
    qtest_add_func("sn/builder/ip53-widget-zero-pxbow-identity",
                   test_ip53_widget_zero_pxbow_identity);
    qtest_add_func("sn/builder/ip53-widget-zero-status-source",
                   test_ip53_widget_zero_status_source);
    qtest_add_func("sn/builder/ip53-pxbow-route-fences-and-interrupt",
                   test_ip53_pxbow_route_fences_and_interrupt);
    qtest_add_func("sn/builder/ip53-pxbow-pic-memory-windows",
                   test_ip53_pxbow_pic_memory_windows);
    qtest_add_func("sn/builder/ip53-runtime-cpu-property-refusals",
                   test_ip53_runtime_cpu_property_refusals);
    qtest_add_func("sn/builder/ip53-test-fixture-refused-without-qtest",
                   test_ip53_fixture_refused_without_qtest);
    qtest_add_func("sn/builder/ip53-missing-profile-refused-in-qtest",
                   test_ip53_missing_profile_refused_in_qtest);
    qtest_add_data_func("sn/builder/ip53-slot-zero-refused-in-qtest",
                        "ip53-slot0",
                        test_ip53_invalid_position_refused_in_qtest);
    qtest_add_data_func("sn/builder/ip53-slot-five-refused-in-qtest",
                        "ip53-slot5",
                        test_ip53_invalid_position_refused_in_qtest);
    qtest_add_data_func("sn/builder/ip53-slot-thirty-one-refused-in-qtest",
                        "ip53-slot31",
                        test_ip53_invalid_position_refused_in_qtest);
    qtest_add_data_func("sn/builder/ip53-intb-refused-in-qtest",
                        "ip53-intxb",
                        test_ip53_invalid_position_refused_in_qtest);
    qtest_add_data_func(
        "sn/builder/bedrock-nodes-answer-at-their-own-nasid-window", NULL,
        test_bedrock_nodes_answer_at_their_own_nasid_window);
    qtest_add_data_func(
        "sn/builder/origin300-two-chassis-refuses-an-uneven-record-list",
        NULL, test_origin300_two_chassis_refuses_an_uneven_record_list);
    qtest_add_data_func("sn/builder/expansion/construction",
                        NULL, test_expansion_builder);
    qtest_add_data_func("sn/builder/expansion/experimental-offering",
                        "origin300,topology=origin300-pci-expansion-experiment",
                        test_expansion_builder);
    qtest_add_data_func("sn/builder/expansion/composed-lifecycle/reset",
                        NULL, test_expansion_composed_lifecycle);
    qtest_add_data_func("sn/builder/expansion/composed-lifecycle/migration",
                        GUINT_TO_POINTER(1), test_expansion_composed_lifecycle);
    qtest_add_func("sn/builder/expansion/controller-locations",
                   test_expansion_controller_locations);
    qtest_add_data_func("sn/builder/expansion/controller-startup",
                        NULL, test_expansion_controller_startup);
    qtest_add_data_func("sn/builder/expansion/physical-board-lookup",
                        NULL, test_expansion_physical_board_lookup);
    qtest_add_data_func("sn/builder/expansion/cpu-controller-latch",
                        GINT_TO_POINTER(1), test_expansion_cpu_board_load);
    qtest_add_data_func("sn/builder/expansion/cpu-board-load",
                        NULL, test_expansion_cpu_board_load);
    qtest_add_data_func("sn/builder/expansion/controller-serial/override",
                        NULL, test_expansion_controller_serial);
    qtest_add_data_func("sn/builder/expansion/controller-serial/refusals",
                        NULL, test_expansion_serial_refusals);
    qtest_add_func("sn/builder/expansion/storage/input-refusals",
                   test_expansion_storage_input_refusals);
    qtest_add_func("sn/builder/expansion/storage/backend-refusals",
                   test_expansion_storage_backend_refusals);
    qtest_add_func("sn/builder/expansion/storage/provider-refusals",
                   test_expansion_storage_provider_refusals);
    qtest_add_func("sn/builder/expansion/compute-board-identity",
                   test_expansion_compute_board_identity);
    qtest_add_func("sn/builder/expansion/compute-board-identity-migration",
                   test_expansion_compute_board_identity_migration);
    qtest_add_data_func("sn/builder/expansion/compute-remote-boot/payload",
                        NULL, test_expansion_compute_remote_boot);
    qtest_add_data_func("sn/builder/expansion/compute-remote-boot/pflash",
                        GINT_TO_POINTER(1), test_expansion_compute_remote_boot);
    qtest_add_func("sn/builder/expansion/processor-absence",
                   test_expansion_processor_absence);
    qtest_add_data_func(
        "sn/builder/expansion/compute-memory/ordinary-two-cpus",
                        GUINT_TO_POINTER(0), test_compute_memory_projection);
    qtest_add_data_func(
        "sn/builder/expansion/compute-memory/selected-two-cpus",
                        GUINT_TO_POINTER(1), test_compute_memory_projection);
    qtest_add_data_func(
        "sn/builder/expansion/compute-memory/ordinary-four-cpus",
                        GUINT_TO_POINTER(2), test_compute_memory_projection);
    qtest_add_data_func(
        "sn/builder/expansion/compute-memory/selected-four-cpus",
                        GUINT_TO_POINTER(3), test_compute_memory_projection);
    qtest_add_data_func("sn/builder/expansion/compute-uart/ordinary-two-cpus",
                        GUINT_TO_POINTER(0), test_compute_uart_with_expansion);
    qtest_add_data_func("sn/builder/expansion/compute-uart/selected-two-cpus",
                        GUINT_TO_POINTER(1), test_compute_uart_with_expansion);
    qtest_add_data_func("sn/builder/expansion/compute-uart/ordinary-four-cpus",
                        GUINT_TO_POINTER(2), test_compute_uart_with_expansion);
    qtest_add_data_func("sn/builder/expansion/compute-uart/selected-four-cpus",
                        GUINT_TO_POINTER(3), test_compute_uart_with_expansion);
    qtest_add_func("sn/builder/expansion/dma-memory-decode",
                   test_expansion_dma_memory_decode);
    qtest_add_func("sn/builder/expansion/composed-adapters",
                   test_expansion_composed_adapters);
    qtest_add_func("sn/builder/expansion/six-head-pio",
                   test_expansion_six_head_pio);
    qtest_add_data_func("sn/builder/expansion/pending-registers/two-cpus",
                        GUINT_TO_POINTER(2), test_expansion_pending_registers);
    qtest_add_data_func("sn/builder/expansion/pending-registers/four-cpus",
                        GUINT_TO_POINTER(4), test_expansion_pending_registers);
    qtest_add_data_func("sn/builder/expansion/unconnected-uart/two-cpus",
                        GUINT_TO_POINTER(2), test_expansion_unconnected_uart);
    qtest_add_data_func("sn/builder/expansion/unconnected-uart/four-cpus",
                        GUINT_TO_POINTER(4), test_expansion_unconnected_uart);
    qtest_add_func("sn/builder/expansion/storage/composed-read",
                   test_expansion_storage_composed_read);
    qtest_add_func("sn/builder/expansion/storage/binding-required",
                   test_expansion_storage_binding_required);
    qtest_add_data_func("sn/builder/expansion/controller-inputs",
                        NULL, test_expansion_controller_inputs);
    qtest_add_data_func("sn/builder/expansion/controller-refusals",
                        NULL, test_expansion_controller_refusals);
    qtest_add_data_func("sn/builder/expansion/management-migration",
                        NULL, test_expansion_management_migration);
    qtest_add_data_func("sn/builder/expansion/migration-identity-mismatch",
                        "identity-mismatch=true",
                        test_expansion_management_mismatch);
    qtest_add_data_func("sn/builder/expansion/migration-attachment-mismatch",
                        "attachment-mismatch=true",
                        test_expansion_management_mismatch);
    qtest_add_data_func("sn/builder/expansion/migration-ownership-mismatch",
                        "ownership-mismatch=true",
                        test_expansion_management_mismatch);
    qtest_add_data_func("sn/builder/origin300-two-chassis-l1s-are-peers",
                        NULL, test_origin300_two_chassis_l1s_are_peers);
    qtest_add_data_func("sn/builder/origin200-accepts-its-cpu-populations",
                        NULL, test_origin200_accepts_its_cpu_populations);
    qtest_add_data_func("sn/builder/origin200-accepts-its-memory-populations",
                        NULL, test_origin200_accepts_its_memory_populations);
    qtest_add_data_func("sn/builder/origin200-dual-accepts-its-populations",
                        NULL, test_origin200_dual_accepts_its_populations);
    qtest_add_data_func("sn/builder/origin300-direct-v12-builds-device",
                        NULL, test_origin300_direct_v12_builds_device);
    qtest_add_data_func("sn/builder/origin300-vbrick-projects-v12",
                        NULL, test_origin300_vbrick_projects_v12);
    qtest_add_data_func("sn/builder/origin300-two-chassis-population",
                        NULL, test_origin300_two_chassis_population);
    qtest_add_data_func(
        "sn/builder/origin2000-rack-accepts-its-populations", NULL,
        test_origin2000_rack_accepts_its_populations);
    qtest_add_data_func(
        "sn/builder/origin2000-deskside-accepts-its-populations", NULL,
        test_origin2000_one_node_accepts_its_populations);
    qtest_add_data_func(
        "sn/builder/origin2000-deskside-has-two-cpus-on-node-zero", NULL,
        test_origin2000_one_node_has_two_cpus_on_node_zero);
    qtest_add_data_func("sn/builder/origin200-onboard-board-is-ip29-preset",
                        NULL, test_origin200_onboard_board_is_ip29_preset);
    qtest_add_data_func("sn/builder/origin2000-onboard-board-is-xio-baseio",
                        NULL, test_origin2000_onboard_board_is_xio_baseio);
    qtest_add_data_func(
        "sn/builder/origin2000-deskside-baseio-follows-the-preset", NULL,
        test_origin2000_deskside_baseio_follows_the_preset);
    qtest_add_data_func("sn/builder/fitted-boards-have-distinct-serials",
                        NULL, test_fitted_boards_have_distinct_serials);
    qtest_add_data_func("sn/builder/origin2000-deskside-slots", NULL,
                        test_origin2000_deskside_slots);
    qtest_add_data_func("sn/builder/boards-on-two-crossbows", NULL,
                        test_boards_on_two_crossbows);
    qtest_add_data_func("sn/builder/widget-boards-on-two-crossbows", NULL,
                        test_widget_boards_on_two_crossbows);
    qtest_add_data_func("sn/builder/board-ids-are-unique", NULL,
                        test_board_ids_are_unique);
    qtest_add_data_func("sn/builder/mgras-fitted-at-a-named-slot", NULL,
                        test_mgras_fitted_at_a_named_slot);
    qtest_add_data_func("sn/builder/mgras-fitted-in-io3", NULL,
                        test_mgras_fitted_in_io3);
    qtest_add_data_func("sn/builder/no-mgras-without-a-catalogue-record",
                        NULL, test_no_mgras_without_a_catalogue_record);
    qtest_add_data_func("sn/builder/mgras-in-a-gigachannel-cabinet", NULL,
                        test_mgras_in_a_gigachannel_cabinet);
    qtest_add_data_func("sn/builder/odyssey-fitted-at-a-named-slot", NULL,
                        test_odyssey_fitted_at_a_named_slot);
    qtest_add_func(
        "sn/builder/odyssey-bad-nic-serial-does-not-leak-fabric",
        test_odyssey_bad_nic_serial_does_not_leak_fabric);

    return g_test_run();
}
