/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 2000 Crosstalk assembly over a format-2 topology's crossbar
 * records.
 */

#include "qemu/osdep.h"
#include CONFIG_DEVICES /* CONFIG_SN0_XTALK_ASSEMBLY_TEST */
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/scsi/qlogic-isp.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/runstate.h"
#include "system/system.h"
#include "sn0.h"
#include "sn0-origin2000-io.h"
#include "sn0-origin2000-xtalk.h"

/*
 * Bounds on what one module's crosstalk assembly can carry: the shipped
 * Origin 2000 compute module (four nodes, two Crossbows, one BaseIO board)
 * is the only populated instance today. sgi_machine_init asserts the
 * topology fits before calling prepare(); raising these is a capacity
 * change, not a format-2 field.
 */
#define ORIGIN2000_XTALK_MAX_NODES 4
#define ORIGIN2000_XTALK_MAX_CROSSBOWS 2
#define ORIGIN2000_XTALK_MAX_IO_BOARDS 1
#define ORIGIN2000_PACKET_SIZE (1ULL << 52)
#define HUB_WIDGET_ID 0x400000
#define WIDGET_REGISTER_DATA 4

typedef struct SN0Origin2000XtalkSource {
    SN0Origin2000HubOutgoingIO outgoing;
    MemoryRegion packet_proxy;
    MemoryRegion packet_alias;
    MemoryRegion cpu_alias;
    bool linked;
    bool bound;
    /* The two aliases above are storage that initializes once only. */
    bool initialized;
} SN0Origin2000XtalkSource;

/*
 * What a Crossbow reaches when it addresses a Hub. The packet face is the
 * reviewed bounded decoder. The processor-side face is deliberately outside
 * that scope rather than an unanswered question: the address-transport
 * design's independent clearance (Origami project tree,
 * docs/datasheets/hub-io-interface.md, "Origin 2000 address transport", and
 * the record it names) excludes the remote Hub processor-facing decoder
 * along with complete machine assembly, forbids substituting RAM, the packet
 * face or a guessed register bank for it, and permits a deliberately
 * refusing face only in a test configuration -- expressly not as a
 * production machine's answer.
 *
 * The one cleared processor-side access is the common widget-identification
 * word at short offset four. Everything else still refuses and stops the
 * machine rather than returning a fabricated value.
 */
typedef struct SN0Origin2000XtalkDestination {
    SN0Origin2000HubIO inbound;
    MemoryRegion cpu_refusal;
    SGIHubState *hub;
    unsigned int node_index;
    unsigned int widget;
    bool initialized;
} SN0Origin2000XtalkDestination;

/*
 * The base I/O board: one Bridge and the PCI functions the descriptor fits
 * behind it, under an owner of their own so the board can be released as a
 * unit. The proxy is what the chip's own transfers leave through, standing
 * in for the switch port until the switch is realized and has one.
 */
typedef struct SN0Origin2000XtalkBoard {
    Object *owner;
    SGIBridgeState *bridge;
    PCIDevice **pci;
    size_t pci_count;
    MemoryRegion packet_proxy;
    MemoryRegion packet_alias;
    bool bound;
    /* As on a source: the alias above initializes once only. */
    bool initialized;
} SN0Origin2000XtalkBoard;

struct SN0Origin2000XtalkState {
    Object parent_obj;

    /*
     * Borrowed, not owned: the machine's node array and the topology it
     * was prepared over. Kept so unparent (called generically by QOM
     * teardown, with no extra argument to pass) can still find them.
     */
    SN0NodeState **nodes;
    const SGIModelTopology *t;

    unsigned int node_count;
    unsigned int crossbar_count;
    unsigned int io_board_count;

    /*
     * Which xtalk_attachments[] entry is each node's own host attachment
     * (source and destination), and which is each io_boards[] entry's own
     * attachment. See sn0_origin2000_xtalk_index_attachments: the format-2
     * array holds both kinds together, one host entry per node plus one
     * entry per board, distinguished only by this bookkeeping, since
     * SGIModelIoBoard itself carries no crossbar_index/widget.
     */
    unsigned int node_attachment[ORIGIN2000_XTALK_MAX_NODES];
    unsigned int board_attachment[ORIGIN2000_XTALK_MAX_IO_BOARDS];

    SGIXbowState *crossbow[ORIGIN2000_XTALK_MAX_CROSSBOWS];
    SN0Origin2000XtalkSource source[ORIGIN2000_XTALK_MAX_NODES];
    SN0Origin2000XtalkDestination destination[ORIGIN2000_XTALK_MAX_NODES];
    bool attached;
    /*
     * A board was built far enough for a function to take host
     * configuration, and could not be finished. Nothing gives that back,
     * so there is no honest machine left to attach into.
     */
    bool spent;
#ifdef CONFIG_SN0_XTALK_ASSEMBLY_TEST
    const char *test_board_part;
    int test_crossbow_failure;
    int test_function_failure;
#endif
    SN0Origin2000XtalkBoard board[ORIGIN2000_XTALK_MAX_IO_BOARDS];
    bool cleaned;
    /* A realized Hub still aliases an outgoing face of this object's. */
    bool faces_kept;
};

static ObjectUnparent *parent_unparent;

/*
 * Split t->xtalk_attachments[] into each node's own host entry (used to
 * build that node's outgoing proxy and processor-side destination decoder)
 * and each io_boards[] entry's own placement (used only to attach the
 * board's Bridge face to its crossbar). The format-2 array carries both
 * kinds together: one entry per node (node_index unique among these) plus
 * one entry per board, both keyed by node_index (a board's entry names the
 * node that controls it). A node's first attachment in array order is
 * always its own host entry -- the shipped module lists a board's entry
 * after its controlling node's host entry -- so the remaining entries, in
 * array order, are the boards', matched to io_boards[] in array order.
 * This is bookkeeping over an already-validated array, not a new hardware
 * fact: sgi_machine_validate_topology has already bounded every index, and
 * the assertion below catches a topology this rule cannot resolve.
 */
static void sn0_origin2000_xtalk_index_attachments(
    SN0Origin2000XtalkState *s, const SGIModelTopology *t)
{
    bool claimed[ORIGIN2000_XTALK_MAX_NODES] = { false };
    unsigned int board = 0;
    size_t i;

    g_assert(t->node_count <= ORIGIN2000_XTALK_MAX_NODES);
    g_assert(t->crossbar_count <= ORIGIN2000_XTALK_MAX_CROSSBOWS);
    g_assert(t->io_board_count <= ORIGIN2000_XTALK_MAX_IO_BOARDS);
    g_assert(t->xtalk_attachment_count == t->node_count + t->io_board_count);

    for (i = 0; i < t->node_count; i++) {
        s->node_attachment[i] = t->xtalk_attachment_count;
    }
    for (i = 0; i < t->xtalk_attachment_count; i++) {
        unsigned int node_index = t->xtalk_attachments[i].node_index;

        g_assert(node_index < t->node_count);
        if (!claimed[node_index]) {
            claimed[node_index] = true;
            s->node_attachment[node_index] = i;
        } else {
            g_assert(board < t->io_board_count);
            s->board_attachment[board++] = i;
        }
    }
    g_assert(board == t->io_board_count);
    for (i = 0; i < t->node_count; i++) {
        g_assert(s->node_attachment[i] < t->xtalk_attachment_count);
    }
}

/*
 * Fault injection, built only where the fixture machines that drive it are.
 * The two setters below are the only way in, so an assembly built without
 * those machines carries neither the state nor a path that reaches it.
 */
#ifdef CONFIG_SN0_XTALK_ASSEMBLY_TEST
static void sn0_origin2000_xtalk_arm_hooks(SN0Origin2000XtalkState *s)
{
    s->test_crossbow_failure = -1;
    s->test_function_failure = -1;
}

/*
 * Which slot a function asks the chip for. The hook sends one function to a
 * slot this chip reserves, so the PCI core refuses it through its own path
 * and leaves behind exactly what a function that refuses leaves behind.
 */
static int sn0_origin2000_xtalk_function_devfn(
    SN0Origin2000XtalkState *s, const SGIModelPciFunction *fn,
    size_t index)
{
    if ((int)index == s->test_function_failure) {
        return PCI_DEVFN(SGI_BRIDGE_PINS, 0);
    }
    return PCI_DEVFN(fn->device, fn->function);
}

static const char *sn0_origin2000_xtalk_board_part(
    SN0Origin2000XtalkState *s, const SGIModelIoBoard *record)
{
    return s->test_board_part ? s->test_board_part : record->part;
}

static void sn0_origin2000_xtalk_break_crossbow(
    SN0Origin2000XtalkState *s, size_t index)
{
    if ((int)index == s->test_crossbow_failure) {
        /* A span the provider refuses, set where a retry can clear it. */
        qdev_prop_set_uint64(DEVICE(s->crossbow[index]), "span", 0);
    }
}

void sn0_origin2000_xtalk_set_test_board_part(SN0Origin2000XtalkState *state,
                                              const char *part)
{
    SN0Origin2000XtalkState *s = SN0_ORIGIN2000_XTALK(state);

    s->test_board_part = part;
}

void sn0_origin2000_xtalk_set_test_crossbow_failure(
    SN0Origin2000XtalkState *state, int index)
{
    SN0Origin2000XtalkState *s = SN0_ORIGIN2000_XTALK(state);

    s->test_crossbow_failure = index;
}

void sn0_origin2000_xtalk_set_test_function_failure(
    SN0Origin2000XtalkState *state, int index)
{
    SN0Origin2000XtalkState *s = SN0_ORIGIN2000_XTALK(state);

    s->test_function_failure = index;
}
#else
static void sn0_origin2000_xtalk_arm_hooks(SN0Origin2000XtalkState *s)
{
}

static int sn0_origin2000_xtalk_function_devfn(
    SN0Origin2000XtalkState *s, const SGIModelPciFunction *fn,
    size_t index)
{
    return PCI_DEVFN(fn->device, fn->function);
}

static const char *sn0_origin2000_xtalk_board_part(
    SN0Origin2000XtalkState *s, const SGIModelIoBoard *record)
{
    return record->part;
}

static void sn0_origin2000_xtalk_break_crossbow(
    SN0Origin2000XtalkState *s, size_t index)
{
}
#endif

static uint16_t crossbow_present_mask(const SGIModelTopology *t,
                                      unsigned int crossbar_index)
{
    uint16_t mask = 0;
    size_t i;

    for (i = 0; i < t->xtalk_attachment_count; i++) {
        const SGIModelXtalkAttachment *attachment = &t->xtalk_attachments[i];

        if ((unsigned int)attachment->crossbar_index == crossbar_index) {
            mask |= 1U << attachment->widget;
        }
    }
    return mask;
}

/*
 * A processor reaching a Hub across the switch. The selected physical Hub's
 * widget-identification word answers its one cleared short read. Every other
 * access stops deliberately rather than returning a fabricated value. The
 * pair of stop calls is
 * ordered: prepare takes the lock that request releases, so neither may be
 * called alone, and neither is safe from an unserialized callback. The
 * request is asynchronous and qtest can keep issuing accesses after it, so
 * every access refuses on its own rather than trusting the machine to have
 * stopped.
 */
static MemTxResult sn0_origin2000_cpu_refusal(void *opaque, hwaddr addr,
                                              uint64_t *data, unsigned size,
                                              bool is_write, MemTxAttrs attrs)
{
    SN0Origin2000XtalkDestination *destination = opaque;

    assert(bql_locked());
    if (!is_write && addr == WIDGET_REGISTER_DATA && size == 4) {
        return memory_region_dispatch_read(&destination->hub->regs,
                                           HUB_WIDGET_ID + addr, data,
                                           MO_32 | MO_BE, attrs);
    }
    error_report("Origin 2000 node %u widget %u: unsupported %u-byte %s at "
                 "0x%" HWADDR_PRIx " on the processor-side Hub face, which "
                 "is outside its cleared decode; stopping instead of "
                 "answering",
                 destination->node_index, destination->widget, size,
                 is_write ? "write" : "read", addr);
    qemu_system_vmstop_request_prepare();
    qemu_system_vmstop_request(RUN_STATE_INTERNAL_ERROR);
    if (data) {
        *data = 0;
    }
    return MEMTX_ERROR;
}

static MemTxResult sn0_origin2000_cpu_refusal_read(void *opaque, hwaddr addr,
                                                   uint64_t *data,
                                                   unsigned size,
                                                   MemTxAttrs attrs)
{
    return sn0_origin2000_cpu_refusal(opaque, addr, data, size, false, attrs);
}

static MemTxResult sn0_origin2000_cpu_refusal_write(void *opaque, hwaddr addr,
                                                    uint64_t data,
                                                    unsigned size,
                                                    MemTxAttrs attrs)
{
    return sn0_origin2000_cpu_refusal(opaque, addr, NULL, size, true, attrs);
}

static const MemoryRegionOps sn0_origin2000_cpu_refusal_ops = {
    .read_with_attrs = sn0_origin2000_cpu_refusal_read,
    .write_with_attrs = sn0_origin2000_cpu_refusal_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
};

static uint32_t sn0_origin2000_ioc3_population(
    const SGIModelIoc3Population *population)
{
    return (population->ethernet ? SGI_IOC3_POPULATION_ETHERNET : 0) |
           (population->serial_a ? SGI_IOC3_POPULATION_SERIAL_A : 0) |
           (population->serial_b ? SGI_IOC3_POPULATION_SERIAL_B : 0) |
           (population->keyboard_mouse ?
            SGI_IOC3_POPULATION_KEYBOARD_MOUSE : 0) |
           (population->parallel ? SGI_IOC3_POPULATION_PARALLEL : 0) |
           (population->onewire_identity ?
            SGI_IOC3_POPULATION_ONEWIRE_IDENTITY : 0);
}

/*
 * The switches, from the descriptor. Attaching builds these as well as
 * preparation does, because a switch keeps every endpoint it is given and a
 * failed attach has no way to hand those back.
 */
static void sn0_origin2000_xtalk_build_crossbows(
    SN0Origin2000XtalkState *s, const SGIModelTopology *t)
{
    size_t i;

    for (i = 0; i < t->crossbar_count; i++) {
        g_autofree char *name = g_strdup_printf("crossbow%zu", i);
        const SGIModelCrossbar *descriptor = &t->crossbars[i];
        DeviceState *crossbow = qdev_new(TYPE_SGI_XBOW);

        object_property_add_child(OBJECT(s), name, OBJECT(crossbow));
        s->crossbow[i] = SGI_XBOW(crossbow);
        object_unref(OBJECT(crossbow));
        qdev_prop_set_uint64(crossbow, "span", SN0_XTALK_WIDGET_SIZE);
        qdev_prop_set_bit(crossbow,
                          "complete-posted-error-address-writes", true);
        qdev_prop_set_uint32(crossbow, "revision",
                             descriptor->widget_revision);
        qdev_prop_set_uint32(crossbow, "part",
                             descriptor->widget_part);
        qdev_prop_set_uint32(crossbow, "manufacturer",
                             descriptor->widget_manufacturer);
        qdev_prop_set_uint16(crossbow, "links-present",
                             crossbow_present_mask(t, i));
        qdev_prop_set_uint16(crossbow, "startup-links-alive",
                             descriptor->initially_operational_links);
        /*
         * Stock PROM resets populated Hub links while checking each node and
         * expects an attached link that was alive before the reset to return
         * to service after the pulse. The generic Crossbow keeps its
         * conservative training contract; this assembly supplies the
         * machine-specific compatibility policy for every populated endpoint.
         */
        qdev_prop_set_uint16(crossbow, "link-reset-restore-alive",
                             crossbow_present_mask(t, i));
    }
}

/*
 * A board's PCI function, built from the board record. The Origin 200 helpers
 * read node-local topology and instance fields a board on a switch has no
 * equivalent of, so using them here would mean handing them a node record
 * that describes nothing in this chassis.
 */
static PCIDevice *sn0_origin2000_xtalk_pci_function(
    SN0Origin2000XtalkBoard *board, const SGIModelIoBoard *record,
    const SGIModelPciFunction *function, unsigned int scsi_index,
    unsigned int board_index, int devfn, Error **errp)
{
    PCIBus *bus = PCI_HOST_BRIDGE(board->bridge)->bus;
    g_autofree char *name = NULL;
    DriveInfo *dinfo;
    BlockBackend *blk;
    PCIDevice *dev;
    Chardev *console;
    Chardev *console_record;

    switch (function->kind) {
    case SGI_MODEL_PCI_ISP1040:
        name = g_strdup_printf("isp%u", scsi_index);
        dev = pci_new(devfn, TYPE_QLOGIC_ISP1040);
        object_property_add_child(board->owner, name, OBJECT(dev));
        if (!pci_realize_and_unref(dev, bus, errp)) {
            goto refused;
        }
        pci_set_byte(dev->config + PCI_REVISION_ID, function->revision);
        qlogic_isp_handle_legacy_cmdline(DEVICE(dev));
        return dev;

    case SGI_MODEL_PCI_IOC3:
        name = g_strdup_printf("sgi-nvram%u", board_index);
        dinfo = drive_get(IF_MTD, 0, board_index);
        blk = dinfo ? blk_by_legacy_dinfo(dinfo) : blk_by_name(name);
        dev = pci_new(devfn, TYPE_SGI_IOC3);
        object_property_add_child(board->owner, "ioc3", OBJECT(dev));
        console = serial_hd(SN0_IOC3_SERIAL_A_CHARDEV);
        if (console) {
            qdev_prop_set_chr(DEVICE(dev), "chardev-a", console);
        }
        console_record = serial_hd(SN0_IOC3_RECORD_A_CHARDEV);
        if (console_record) {
            qdev_prop_set_chr(DEVICE(dev), "record-a", console_record);
        }
        qdev_prop_set_uint16(DEVICE(dev), "subsystem-id",
                             function->subsystem_id);
        qdev_prop_set_uint32(DEVICE(dev), "population",
                             sn0_origin2000_ioc3_population(
                                 &record->ioc3_population));
        qdev_prop_set_uint8(DEVICE(dev), "timekeeper-kind",
                            record->ioc3_population.timekeeper ?
                            SGI_IOC3_TIMEKEEPER_M48T35 :
                            SGI_IOC3_TIMEKEEPER_NONE);
        qdev_prop_set_uint64(DEVICE(dev), "onewire-serial", function->serial);
        if (blk) {
            qdev_prop_set_drive(DEVICE(dev), "nvram-drive", blk);
        }
        if (!pci_realize_and_unref(dev, bus, errp)) {
            goto refused;
        }
        pci_set_byte(dev->config + PCI_REVISION_ID, function->revision);
        /* Board wiring, not PCI INTx: the chip drives a Bridge pin direct. */
        qdev_connect_gpio_out(DEVICE(dev), 0,
                              qdev_get_gpio_in(DEVICE(board->bridge),
                                               function->irq_input));
        return dev;

    default:
        g_assert_not_reached();
    }

refused:
    /*
     * Nothing to undo here. pci_realize_and_unref() hands the caller's
     * reference back and leaves the refused device parented, but a board
     * short of a function is always released, and releasing the chip
     * unparents everything on the bus it carries -- this device included.
     */
    return NULL;
}

/*
 * The chip the board hangs off. Separate from the functions behind it
 * because the two survive a failed attach differently: no function exists
 * until this has realized, since they are built on the bus it creates.
 */
static bool sn0_origin2000_xtalk_build_bridge(
    SN0Origin2000XtalkState *s, const SGIModelIoBoard *record,
    size_t index, Error **errp)
{
    SN0Origin2000XtalkBoard *board = &s->board[index];
    g_autofree char *bus_name = g_strdup_printf("pci.baseio%zu", index);
    DeviceState *bridge = qdev_new(TYPE_SGI_BRIDGE);

    object_property_add_child(board->owner, "bridge", OBJECT(bridge));
    board->bridge = SGI_BRIDGE(bridge);
    object_unref(OBJECT(bridge));

    qdev_prop_set_uint32(bridge, "revision", record->revision);
    qdev_prop_set_uint64(bridge, "onewire-serial", record->serial);
    qdev_prop_set_string(bridge, "board-part",
                         sn0_origin2000_xtalk_board_part(s, record));
    qdev_prop_set_string(bridge, "pci-bus-name", bus_name);
    /*
     * A switched fabric carries the destination widget, so the chip keeps it
     * in bits 51:48 of the address it composes rather than spending address
     * bits on a node number the switch would not read.
     */
    if (!object_property_set_link(OBJECT(bridge), "fabric",
                                  OBJECT(&board->packet_proxy), errp)) {
        return false;
    }
    qdev_prop_set_uint64(bridge, "fabric-base", 0);
    qdev_prop_set_uint32(bridge, "fabric-widget-shift",
                         SGI_BRIDGE_PACKET_ADDRESS_BITS);

    return sysbus_realize(SYS_BUS_DEVICE(bridge), errp);
}

/*
 * The board, built where the descriptor puts it and handed to the switch it
 * hangs off. The Bridge answers a processor and a packet at one face, unlike
 * a Hub, so the switch takes the same region for both of that widget's
 * views.
 *
 * Each step is skipped where a previous attempt already took it. A board
 * outlives a failed attach -- see the unwind -- and a switch built to
 * replace one that refused has to be handed the same board again.
 */
static bool sn0_origin2000_xtalk_fit_board(SN0Origin2000XtalkState *s,
                                           const SGIModelTopology *t,
                                           size_t index, Error **errp)
{
    const SGIModelIoBoard *record = &t->io_boards[index];
    const SGIModelXtalkAttachment *attachment =
        &t->xtalk_attachments[s->board_attachment[index]];
    SN0Origin2000XtalkBoard *board = &s->board[index];
    unsigned int scsi_index = 0;
    MemoryRegion *face;
    size_t i;

    if (!board->bridge &&
        !sn0_origin2000_xtalk_build_bridge(s, record, index, errp)) {
        return false;
    }

    face = sysbus_mmio_get_region(SYS_BUS_DEVICE(board->bridge), 0);
    if (!sgi_xbow_attach_endpoint(s->crossbow[attachment->crossbar_index],
                                  attachment->widget, face,
                                  sgi_bridge_packet_face(board->bridge),
                                  errp)) {
        return false;
    }

    if (!board->pci) {
        board->pci = g_new0(PCIDevice *, record->pci_count);
        board->pci_count = record->pci_count;
    }
    for (i = 0; i < record->pci_count; i++) {
        const SGIModelPciFunction *function = &record->pci[i];
        unsigned int instance =
            function->kind == SGI_MODEL_PCI_ISP1040 ? scsi_index++ : 0;

        if (board->pci[i]) {
            continue;
        }
        board->pci[i] = sn0_origin2000_xtalk_pci_function(
            board, record, function, instance, index,
            sn0_origin2000_xtalk_function_devfn(s, function, i), errp);
        if (!board->pci[i]) {
            return false;
        }
    }

    return true;
}

/*
 * Whether this board is whole: the chip realized, and every function the
 * descriptor names built behind it.
 *
 * Releasing an incomplete board is also what disposes of a function the PCI
 * core refused: unparenting the chip unparents its bus, which takes every
 * device still on that bus with it. So no unparent is needed where a function
 * is refused -- but that holds only while this rule releases every board short
 * of complete. Narrow the rule and the unparent has to come back.
 */
static bool sn0_origin2000_xtalk_board_complete(SN0Origin2000XtalkBoard *board)
{
    size_t i;

    if (!board->bridge || !DEVICE(board->bridge)->realized || !board->pci) {
        return false;
    }
    for (i = 0; i < board->pci_count; i++) {
        if (!board->pci[i]) {
            return false;
        }
    }
    return true;
}

/*
 * A board's devices stop before the Bridge whose spaces their transfers use.
 * Each is unparented as well as unrealized: the owner keeps its children by
 * name, so one left behind makes a second attempt collide with the corpse of
 * the first rather than build a board. Unparenting queues the release rather
 * than performing it -- see the note on settling below.
 */
static void sn0_origin2000_xtalk_release_board(SN0Origin2000XtalkBoard *board)
{
    size_t i;

    for (i = board->pci_count; i > 0; i--) {
        if (board->pci[i - 1]) {
            qdev_unrealize(DEVICE(board->pci[i - 1]));
            object_unparent(OBJECT(board->pci[i - 1]));
            board->pci[i - 1] = NULL;
        }
    }
    g_clear_pointer(&board->pci, g_free);
    board->pci_count = 0;
    if (board->bridge) {
        qdev_unrealize(DEVICE(board->bridge));
        object_unparent(OBJECT(board->bridge));
        board->bridge = NULL;
    }
}

/*
 * The switches, given up. Their routes are aliases of the faces the machine
 * attached, so a switch has to be gone before any of those faces is, and
 * unrealizing one is not that: its routes still name the storage until it is
 * finalized.
 */
static void sn0_origin2000_xtalk_release_crossbows(
    SN0Origin2000XtalkState *s)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(s->crossbow); i++) {
        if (s->crossbow[i]) {
            qdev_unrealize(DEVICE(s->crossbow[i]));
            object_unparent(OBJECT(s->crossbow[i]));
            s->crossbow[i] = NULL;
        }
    }
}

/*
 * Unparenting a device does not free it: leaving a bus hands its reference
 * back through RCU, a grace period later and on another thread. Waiting for
 * that here keeps teardown inside this call, so a release lands where the
 * cause is rather than at an unrelated point later in the run.
 *
 * Nothing after the drain needs it. Every face a switch still names is held
 * by something else -- a realized Hub makes this object retire rather than
 * be freed, and the memory core holds a Bridge for as long as a route
 * renders it -- and the address spaces destroyed after this are themselves
 * released through RCU, so the drain does not bound those either. It buys
 * a predictable point of collapse, not correctness.
 *
 * One pass is enough because unparenting a device queues its children's
 * releases before its own, so a subtree comes apart from the inside out.
 *
 * For a caller that is not machine init: drain_call_rcu() drops the BQL and
 * takes it again while it waits, so whatever the lock was holding still can
 * run underneath this.
 */
static void sn0_origin2000_xtalk_settle_releases(void)
{
    drain_call_rcu();
}

/*
 * Undo what attaching did and nothing else. A failed attach leaves the
 * prepared machine it was given, because tearing that down would mean
 * unlinking a property from a Hub that is realized by then, which the
 * property refuses. Keeping the two halves separate also means a caller
 * that recovers from a failed attach has something to recover to.
 */
static void sn0_origin2000_xtalk_cleanup_attach(SN0Origin2000XtalkState *s,
                                                bool terminal)
{
    size_t i;

    /*
     * Aliases come out before the ingress regions they point into, and the
     * inbound decoders come out before the node regions they root on. Both
     * orders matter more than they look: an alias left behind outlives the
     * region under it.
     */
    for (i = 0; i < s->io_board_count; i++) {
        SN0Origin2000XtalkBoard *board = &s->board[i];

        if (board->bound) {
            memory_region_del_subregion(&board->packet_proxy,
                                        &board->packet_alias);
            memory_region_set_enabled(&board->packet_proxy, false);
            board->bound = false;
        }
    }
    for (i = 0; i < s->node_count; i++) {
        SN0Origin2000XtalkSource *source = &s->source[i];

        if (source->bound) {
            memory_region_del_subregion(&source->packet_proxy,
                                        &source->packet_alias);
            memory_region_set_enabled(&source->packet_proxy, false);
            memory_region_del_subregion(&s->nodes[i]->xtalk,
                                        &source->cpu_alias);
            source->bound = false;
        }
    }
    /*
     * Devices stop before the faces they hold spaces over: the board before
     * its proxy, both switches before the Hub decoders they route packets
     * into. The switches go first because this is the order the machine was
     * built in, read backwards -- not because the other order is unsafe. The
     * memory core takes a reference on a region's owner for every flat-view
     * section that renders it and gives it back only once the topology has
     * been re-rendered and the stale views reclaimed, which is part of the
     * switches' own teardown. So a Bridge a Crossbow route still aliases
     * survives its bus-reference drop whichever order the two are released
     * in. An unrealized object here has nothing to undo and says so.
     */
    sn0_origin2000_xtalk_release_crossbows(s);
    for (i = 0; i < s->io_board_count; i++) {
        SN0Origin2000XtalkBoard *board = &s->board[i];

        /*
         * A whole board survives a failed attach. Its functions hold host
         * configuration the machine took once and cannot take again: legacy
         * SCSI buses draw their numbers from a counter that only counts up,
         * and an ethernet configuration is marked used the first time a
         * device takes it. Nothing about the board depends on the switches,
         * so the replacement switch is handed this same board's face.
         *
         * A board with no function array never reached the loop that builds
         * them, so it has taken nothing and is released and built again.
         *
         * In between is a board that started building functions and could
         * not finish, and there is no honest way out of it. A function takes
         * its host configuration partway through its own realize, and from
         * out here the two halves of that realize look the same, so a board
         * short of complete has to be assumed to have taken something.
         * Release it and refuse to attach again, rather than build a second
         * board that comes up quietly missing what the first one took.
         */
        if (!terminal && sn0_origin2000_xtalk_board_complete(board)) {
            continue;
        }
        if (!terminal && board->pci) {
            s->spent = true;
        }
        sn0_origin2000_xtalk_release_board(board);
    }
    sn0_origin2000_xtalk_settle_releases();
    if (terminal) {
        for (i = 0; i < s->node_count; i++) {
            SN0Origin2000XtalkDestination *destination = &s->destination[i];

            if (destination->initialized) {
                sn0_origin2000_hub_io_destroy(&destination->inbound);
                destination->initialized = false;
            }
        }
    } else if (!s->spent) {
        /*
         * What a retry needs back. The switches are built again rather than
         * reused: a switch keeps the endpoints it was given and refuses a
         * second set, and once unrealized it refuses realization too, so
         * there is nothing to reuse. The Hub decoders and refusing faces are
         * left alone in the other direction -- they are storage of this
         * object's, they can be initialized once only, and nothing is
         * pointing at them now that the switches are gone.
         *
         * A spent assembly gets none, because nothing will attach again and
         * a switch nobody realizes is a device left unrealized under the
         * machine, which qdev refuses at the end of machine init.
         */
        sn0_origin2000_xtalk_build_crossbows(s, s->t);
    }
    s->attached = false;
}

static void sn0_origin2000_xtalk_cleanup(SN0Origin2000XtalkState *s)
{
    size_t i;

    if (s->cleaned) {
        return;
    }
    s->cleaned = true;
    /*
     * Quiesce before anything is detached, including a switch that never
     * finished realizing: unrealizing a realized switch quiesces it, but
     * one that failed earlier never gets there and still has to have its
     * link timers cancelled. It is terminal either way -- a quiesced switch
     * refuses everything afterwards, realization included -- which is why a
     * failed attach does not come here and recovers with new switches.
     */
    for (i = 0; i < ARRAY_SIZE(s->crossbow); i++) {
        if (s->crossbow[i]) {
            sgi_xbow_quiesce(s->crossbow[i]);
        }
    }
    sn0_origin2000_xtalk_cleanup_attach(s, true);

    for (i = 0; i < s->node_count; i++) {
        SN0Origin2000XtalkSource *source = &s->source[i];

        /*
         * A realized Hub will not give this link up, and taking it back would
         * not help if it did: the big windows it built over the face at
         * realize are aliases of that region, and they stay in the Hub's own
         * space. So the face has to outlive the Hub rather than the other way
         * round, and the face is storage of this object's -- note that this
         * object can no longer be released. An unrealized Hub is the teardown
         * of a machine that never finished being built, and unlinks cleanly.
         */
        if (source->linked) {
            SN0NodeState *node = s->nodes[i];

            if (DEVICE(&node->hub)->realized) {
                s->faces_kept = true;
            } else {
                object_property_set_link(OBJECT(&node->hub),
                                         "xtalk-io", NULL, &error_abort);
                source->linked = false;
            }
        }
        sn0_origin2000_hub_outgoing_io_destroy(&source->outgoing);
    }
}

static void sn0_origin2000_xtalk_unparent(Object *obj)
{
    SN0Origin2000XtalkState *s = SN0_ORIGIN2000_XTALK(obj);

    sn0_origin2000_xtalk_cleanup(s);
    if (parent_unparent) {
        parent_unparent(obj);
    }
}

static void sn0_origin2000_xtalk_class_init(ObjectClass *oc, const void *data)
{
    parent_unparent = oc->unparent;
    oc->unparent = sn0_origin2000_xtalk_unparent;
}

static const TypeInfo sn0_origin2000_xtalk_type = {
    .name = TYPE_SN0_ORIGIN2000_XTALK,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(SN0Origin2000XtalkState),
    .class_init = sn0_origin2000_xtalk_class_init,
};

static void sn0_origin2000_xtalk_register_types(void)
{
    type_register_static(&sn0_origin2000_xtalk_type);
}
type_init(sn0_origin2000_xtalk_register_types)

SN0Origin2000XtalkState *sn0_origin2000_xtalk_prepare(
    Object *owner, SN0NodeState **nodes, unsigned int node_count,
    const SGIModelTopology *t, Error **errp)
{
    SN0Origin2000XtalkState *s;
    size_t i;

    if (node_count != t->node_count) {
        error_setg(errp, "Origin 2000 Crosstalk needs prepared nodes");
        return NULL;
    }
    for (i = 0; i < t->node_count; i++) {
        if (!nodes[i] || DEVICE(&nodes[i]->hub)->realized) {
            error_setg(errp, "Origin 2000 Crosstalk node %zu is not prepared",
                       i);
            return NULL;
        }
    }

    s = SN0_ORIGIN2000_XTALK(object_new(TYPE_SN0_ORIGIN2000_XTALK));
    s->nodes = nodes;
    s->t = t;
    s->node_count = t->node_count;
    s->crossbar_count = t->crossbar_count;
    s->io_board_count = t->io_board_count;
    sn0_origin2000_xtalk_index_attachments(s, t);
    sn0_origin2000_xtalk_arm_hooks(s);
    object_property_add_child(owner, "xtalk", OBJECT(s));
    object_unref(OBJECT(s));

    sn0_origin2000_xtalk_build_crossbows(s, t);

    for (i = 0; i < t->node_count; i++) {
        g_autofree char *name =
            g_strdup_printf("node%zu-packet-proxy", i);
        SN0Origin2000XtalkSource *source = &s->source[i];
        SN0NodeState *node = nodes[i];

        memory_region_init(&source->packet_proxy, OBJECT(s), name,
                           ORIGIN2000_PACKET_SIZE);
        memory_region_set_enabled(&source->packet_proxy, false);
        if (!sn0_origin2000_hub_outgoing_io_init(
                &source->outgoing, OBJECT(s), node,
                &source->packet_proxy, errp) ||
            !object_property_set_link(OBJECT(&node->hub),
                                      "xtalk-io",
                                      OBJECT(&source->outgoing.face), errp)) {
            sn0_origin2000_xtalk_destroy(s);
            return NULL;
        }
        source->linked = true;
    }

    for (i = 0; i < t->io_board_count; i++) {
        g_autofree char *proxy_name =
            g_strdup_printf("baseio%zu-packet-proxy", i);
        g_autofree char *owner_name = g_strdup_printf("baseio%zu", i);
        SN0Origin2000XtalkBoard *board = &s->board[i];

        board->owner = object_new(TYPE_CONTAINER);
        object_property_add_child(OBJECT(s), owner_name, board->owner);
        object_unref(board->owner);
        memory_region_init(&board->packet_proxy, board->owner, proxy_name,
                           ORIGIN2000_PACKET_SIZE);
        memory_region_set_enabled(&board->packet_proxy, false);
    }

    return s;
}

/*
 * Everything a Crossbow can reach has to be attached before it is realized,
 * because realize is what builds the ingress regions out of the endpoints.
 * That ordering is why this is a second entry point rather than more of
 * prepare: the Hub faces it attaches do not exist until the nodes are real.
 */
bool sn0_origin2000_xtalk_attach(SN0Origin2000XtalkState *s, Error **errp)
{
    const SGIModelTopology *t = s->t;
    SN0NodeState **nodes = s->nodes;
    size_t i;

    if (s->attached) {
        error_setg(errp, "Origin 2000 Crosstalk is already attached");
        return false;
    }
    if (s->spent) {
        error_setg(errp, "Origin 2000 Crosstalk cannot be attached again: a "
                         "base I/O board failed part-built, and the host "
                         "configuration its functions took cannot be given "
                         "back");
        return false;
    }

    /*
     * Checked before anything is touched, so a caller that attaches too
     * early keeps the prepared machine it had rather than losing it to an
     * unwind that has nothing to undo.
     */
    for (i = 0; i < t->node_count; i++) {
        if (!DEVICE(&nodes[i]->hub)->realized) {
            error_setg(errp, "Origin 2000 Crosstalk node %zu is not realized",
                       i);
            return false;
        }
    }

    for (i = 0; i < t->node_count; i++) {
        const SGIModelXtalkAttachment *attachment =
            &t->xtalk_attachments[s->node_attachment[i]];
        SN0NodeState *node = nodes[i];
        SN0Origin2000XtalkDestination *destination = &s->destination[i];
        SGIXbowState *crossbow = s->crossbow[attachment->crossbar_index];

        destination->node_index = i;
        destination->widget = attachment->widget;
        destination->hub = &node->hub;
        /*
         * Built once and kept. Both of these are storage of this object's
         * that can be initialized once only, so a retry reuses what a failed
         * attempt left rather than initializing over it.
         */
        if (!destination->initialized) {
            g_autofree char *name = g_strdup_printf("node%zu-cpu-face", i);

            if (!sn0_origin2000_hub_io_init(
                    &destination->inbound, OBJECT(s),
                    node, errp)) {
                goto unwind;
            }
            memory_region_init_io(&destination->cpu_refusal, OBJECT(s),
                                  &sn0_origin2000_cpu_refusal_ops, destination,
                                  name, SN0_XTALK_WIDGET_SIZE);
            destination->initialized = true;
        }
        if (!sgi_xbow_attach_endpoint(crossbow, attachment->widget,
                                      &destination->cpu_refusal,
                                      &destination->inbound.face, errp)) {
            goto unwind;
        }
    }

    /*
     * The board after the Hubs, though either order builds the same machine:
     * both have to precede the switch realize below, and this way a board
     * that refuses leaves an unwind with something to undo.
     */
    for (i = 0; i < t->io_board_count; i++) {
        if (!sn0_origin2000_xtalk_fit_board(s, t, i, errp)) {
            goto unwind;
        }
    }

    for (i = 0; i < t->crossbar_count; i++) {
        sn0_origin2000_xtalk_break_crossbow(s, i);
        if (!sysbus_realize(SYS_BUS_DEVICE(s->crossbow[i]), errp)) {
            goto unwind;
        }
    }

    /*
     * Now that the ingress regions exist, give each source the two views it
     * sends into: its packet proxy, which the Hub's outgoing adapter already
     * points at, and its own crosstalk container, where the existing small
     * and big windows already alias from.
     */
    for (i = 0; i < t->node_count; i++) {
        const SGIModelXtalkAttachment *attachment =
            &t->xtalk_attachments[s->node_attachment[i]];
        SN0NodeState *node = nodes[i];
        SN0Origin2000XtalkSource *source = &s->source[i];
        SGIXbowState *crossbow = s->crossbow[attachment->crossbar_index];
        MemoryRegion *packet = sgi_xbow_packet_ingress(crossbow,
                                                       attachment->widget);
        MemoryRegion *cpu = sgi_xbow_ingress(crossbow, attachment->widget);
        g_autofree char *packet_name =
            g_strdup_printf("node%zu-packet-ingress", i);
        g_autofree char *cpu_name = g_strdup_printf("node%zu-cpu-ingress", i);

        /*
         * Initialized once only, for the reason the destination decoders
         * are: memory_region_init() zeroes the object, drops the property
         * table it had and adds a second child property on the owner
         * naming the same storage, so the owner's finalize would unref
         * something already finalized. Nothing between here and the end of
         * attach can fail, so this runs once -- and the assertion is what
         * says so if a failure point ever moves above it.
         */
        assert(!source->initialized);
        memory_region_init_alias(&source->packet_alias, OBJECT(s), packet_name,
                                 packet, 0, memory_region_size(packet));
        memory_region_add_subregion(&source->packet_proxy, 0,
                                    &source->packet_alias);
        memory_region_init_alias(&source->cpu_alias, OBJECT(s), cpu_name, cpu,
                                 0, memory_region_size(cpu));
        memory_region_add_subregion(&node->xtalk, 0, &source->cpu_alias);
        source->initialized = true;
        source->bound = true;
        memory_region_set_enabled(&source->packet_proxy, true);
    }

    for (i = 0; i < t->io_board_count; i++) {
        const SGIModelXtalkAttachment *attachment =
            &t->xtalk_attachments[s->board_attachment[i]];
        SN0Origin2000XtalkBoard *board = &s->board[i];
        SGIXbowState *crossbow = s->crossbow[attachment->crossbar_index];
        MemoryRegion *packet = sgi_xbow_packet_ingress(crossbow,
                                                       attachment->widget);
        g_autofree char *name = g_strdup_printf("baseio%zu-packet-ingress", i);

        assert(!board->initialized);
        memory_region_init_alias(&board->packet_alias, board->owner, name,
                                 packet, 0, memory_region_size(packet));
        memory_region_add_subregion(&board->packet_proxy, 0,
                                    &board->packet_alias);
        board->initialized = true;
        board->bound = true;
        memory_region_set_enabled(&board->packet_proxy, true);
    }

    s->attached = true;
    return true;

unwind:
    sn0_origin2000_xtalk_cleanup_attach(s, false);
    return false;
}

void sn0_origin2000_xtalk_destroy(SN0Origin2000XtalkState *s)
{
    if (!s) {
        return;
    }
    sn0_origin2000_xtalk_cleanup(s);
    if (!s->faces_kept) {
        object_unparent(OBJECT(s));
        return;
    }
    /*
     * Retired rather than released, because a realized Hub is still pointing
     * at faces that live in here: the storage stays with the machine,
     * disabled and reaching nothing, for as long as the Hubs that alias it.
     * Giving the name up is not the point. Retirement happens only where a
     * Hub is realized, and preparation refuses a machine in that state, so
     * there is no later assembly to hand the name to.
     */
    object_ref(OBJECT(s));
    object_unparent(OBJECT(s));
    object_property_add_child(object_get_root(), "xtalk-retired[*]",
                              OBJECT(s));
    object_unref(OBJECT(s));
}

bool sn0_origin2000_xtalk_test_proxies_disabled(SN0Origin2000XtalkState *state)
{
    SN0Origin2000XtalkState *s = SN0_ORIGIN2000_XTALK(state);
    size_t i;

    if (!s) {
        return false;
    }
    for (i = 0; i < s->node_count; i++) {
        SN0Origin2000XtalkSource *source = &s->source[i];

        if (source->packet_proxy.enabled ||
            memory_region_size(&source->packet_proxy) !=
            ORIGIN2000_PACKET_SIZE) {
            return false;
        }
        /*
         * Which proxy this Hub's own adapter sends into, read back from the
         * adapter. Handing every Hub one shared ingress is the mistake this
         * catches, and it is the sender that has to be asked: the proxies
         * are separate members of an array whatever they are wired to.
         */
        if (!source->outgoing.packets ||
            source->outgoing.packets->root != &source->packet_proxy) {
            return false;
        }
    }
    return !s->board[0].packet_proxy.enabled &&
        memory_region_size(&s->board[0].packet_proxy) ==
        ORIGIN2000_PACKET_SIZE;
}
