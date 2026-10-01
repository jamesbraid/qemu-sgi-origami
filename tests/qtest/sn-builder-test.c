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
#include <sys/wait.h>
#include "qemu/bswap.h"
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
        g_test_message("QEMU rejection: %s", error_output->str);
    }
    g_assert_nonnull(strstr(error_output->str, message));
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
    static const unsigned int mib[] = { 512, 256, 128, 64 };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(mib); i++) {
        g_autofree char *args = g_strdup_printf(
            "-M origin200 -m %uM -display none", mib[i]);
        QTestState *qts = qtest_init(args);

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
    static const unsigned int mib_per_node[] = { 512, 256, 128, 64 };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(mib_per_node); i++) {
        g_autofree char *args = g_strdup_printf(
            "-M origin200,topology=origin200-dual -smp 4 "
            "-m %uM -display none", mib_per_node[i] * 2);
        QTestState *qts = qtest_init(args);

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
    g_assert_cmphex(qtest_readl(qts, port + 4), ==, 0x1c013047);
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
#define NASID_TEST_SPACE_HUB UINT64_C(0x10000000000)
#define NASID_TEST_HUB_LOCAL_REGISTERS \
    (NASID_TEST_SPACE_HUB + UINT64_C(0x10000000))
#define NASID_TEST_BOARD_ID_LOCAL \
    (NASID_TEST_HUB_LOCAL_REGISTERS + UINT64_C(0x4000000))
#define NASID_TEST_BOARD_ID_VALUE UINT64_C(0x8001)
/* M mode's node field starts at physical address bit 33. */
#define NASID_TEST_NODE_ADDRESS_SHIFT 33

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
        const char *args[] = { qtest_qemu_binary(NULL), "-M",
            STORAGE_TOPOLOGY "," STORAGE_BINDING, "-cpu", "R14000",
            "-smp", "2", "-m", "512M", "-S", "-drive", drive,
            "-bios", prom, "-nodefaults", "-display", "none", NULL };

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
        "-bios %s -nodefaults -display none", STORAGE_TOPOLOGY,
        STORAGE_BINDING, image, compute, prom);
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
 * Origin 200 accepts 512, 256, 128, and 64 MiB per node. -m 768M (single
 * node, default topology) is not in the accepted list.
 */
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-m", "768M",
        "-display", "none", NULL
    };

    assert_qemu_rejects(args,
                        "topology 'origin200' does not accept 768 "
                        "MiB per node");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn/builder/numa-refused", NULL,
                        test_numa_refused);
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

    return g_test_run();
}
