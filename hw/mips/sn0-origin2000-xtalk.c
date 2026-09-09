/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Descriptor-owned Origin 2000 Crosstalk assembly storage. */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/scsi/qlogic-isp.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "system/runstate.h"
#include "sn0.h"
#include "sn0-origin2000-io.h"
#include "sn0-origin2000-xtalk.h"

#define TYPE_SN0_ORIGIN2000_XTALK "sn0-origin2000-xtalk"
OBJECT_DECLARE_SIMPLE_TYPE(SN0Origin2000XtalkState,
                           SN0_ORIGIN2000_XTALK)

#define ORIGIN2000_XTALK_NODES 4
#define ORIGIN2000_XTALK_CROSSBOWS 2
#define ORIGIN2000_XTALK_IO_BOARDS 1
#define ORIGIN2000_PACKET_SIZE (1ULL << 52)

typedef struct SN0Origin2000XtalkSource {
    SN0Origin2000HubOutgoingIO outgoing;
    MemoryRegion packet_proxy;
    MemoryRegion packet_alias;
    MemoryRegion cpu_alias;
    bool linked;
    bool bound;
} SN0Origin2000XtalkSource;

/*
 * What a Crossbow reaches when it addresses a Hub. The packet face is the
 * reviewed bounded decoder. The processor-side face is not: no cleared
 * contract says what a Hub answers to a general crosstalk read from another
 * node's processor, and neither RAM, the raw registers nor the packet face
 * is that answer. Rather than invent one, the face refuses every access and
 * stops the machine where a guest would otherwise consume a fabricated
 * value. Replacing it is the point at which this becomes a machine that can
 * run, not an implementation detail to tidy later.
 */
typedef struct SN0Origin2000XtalkDestination {
    SN0Origin2000XtalkState *owner;
    SN0Origin2000HubIO inbound;
    MemoryRegion cpu_refusal;
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
} SN0Origin2000XtalkBoard;

struct SN0Origin2000XtalkState {
    Object parent_obj;

    SN0MachineState *machine;
    SGIXbowState *crossbow[ORIGIN2000_XTALK_CROSSBOWS];
    SN0Origin2000XtalkSource source[ORIGIN2000_XTALK_NODES];
    SN0Origin2000XtalkDestination destination[ORIGIN2000_XTALK_NODES];
    bool attached;
    const char *test_board_part;
    int test_crossbow_failure;
    SN0Origin2000XtalkBoard board[ORIGIN2000_XTALK_IO_BOARDS];
    bool cleaned;
    /* A realized Hub still aliases an outgoing face of this object's. */
    bool faces_kept;
};

static ObjectUnparent *parent_unparent;

static uint16_t crossbow_present_mask(const SN0ModelComposition *model,
                                      unsigned int crossbow_index)
{
    uint16_t mask = 0;
    size_t i;

    for (i = 0; i < model->xtalk_attachment_count; i++) {
        const SN0ModelXtalkAttachment *attachment =
            &model->xtalk_attachments[i];

        if (attachment->crossbow_index == crossbow_index) {
            mask |= 1U << attachment->widget;
        }
    }
    for (i = 0; i < model->io_board_count; i++) {
        const SN0ModelIoBoard *board = &model->io_boards[i];

        if (board->crossbow_index == crossbow_index) {
            mask |= 1U << board->widget;
        }
    }
    return mask;
}

/*
 * A processor reaching a Hub across the switch. The stop is deliberate and
 * the diagnostic is unconditional, because the alternative is answering with
 * a number no hardware would have produced. The pair of stop calls is
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
    error_report("Origin 2000 node %u widget %u: unsupported %u-byte %s at "
                 "0x%" HWADDR_PRIx " on the processor-side Hub face, which "
                 "has no cleared decode; stopping instead of answering",
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
    const SN0ModelIoc3Population *population)
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
static void sn0_origin2000_xtalk_build_crossbows(SN0Origin2000XtalkState *s)
{
    const SN0ModelComposition *model = s->machine->composition;
    size_t i;

    for (i = 0; i < model->crossbow_count; i++) {
        g_autofree char *name = g_strdup_printf("crossbow%zu", i);
        const SN0ModelCrossbow *descriptor = &model->crossbows[i];
        DeviceState *crossbow = qdev_new(TYPE_SGI_XBOW);

        object_property_add_child(OBJECT(s), name, OBJECT(crossbow));
        s->crossbow[i] = SGI_XBOW(crossbow);
        object_unref(OBJECT(crossbow));
        qdev_prop_set_uint64(crossbow, "span", SN0_XTALK_WIDGET_SIZE);
        qdev_prop_set_bit(crossbow, "stop-on-link-reset", true);
        qdev_prop_set_uint32(crossbow, "revision", descriptor->revision);
        qdev_prop_set_uint32(crossbow, "part", descriptor->part);
        qdev_prop_set_uint32(crossbow, "manufacturer",
                             descriptor->manufacturer);
        qdev_prop_set_uint16(crossbow, "links-present",
                             crossbow_present_mask(model, i));
        qdev_prop_set_uint16(crossbow, "startup-links-alive",
                             descriptor->initially_operational_links);
    }
}

/*
 * A board's PCI function, built from the board record. The Origin 200 helpers
 * read node-local topology and instance fields a board on a switch has no
 * equivalent of, so using them here would mean handing them a node record
 * that describes nothing in this chassis.
 */
static PCIDevice *sn0_origin2000_xtalk_pci_function(
    SN0Origin2000XtalkBoard *board, const SN0ModelIoBoard *record,
    const SN0ModelComponent *function, unsigned int scsi_index, Error **errp)
{
    PCIBus *bus = PCI_HOST_BRIDGE(board->bridge)->bus;
    g_autofree char *name = NULL;
    PCIDevice *dev;

    switch (function->kind) {
    case SN0_COMPONENT_ISP1040:
        name = g_strdup_printf("isp%u", scsi_index);
        dev = pci_new(PCI_DEVFN(function->device, function->function),
                      TYPE_QLOGIC_ISP1040);
        object_property_add_child(board->owner, name, OBJECT(dev));
        if (!pci_realize_and_unref(dev, bus, errp)) {
            return NULL;
        }
        pci_set_byte(dev->config + PCI_REVISION_ID, function->revision);
        qlogic_isp_handle_legacy_cmdline(DEVICE(dev));
        return dev;

    case SN0_COMPONENT_IOC3:
        dev = pci_new(PCI_DEVFN(function->device, function->function),
                      TYPE_SGI_IOC3);
        object_property_add_child(board->owner, "ioc3", OBJECT(dev));
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
        if (!pci_realize_and_unref(dev, bus, errp)) {
            return NULL;
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
}

/*
 * The chip the board hangs off. Separate from the functions behind it
 * because the two survive a failed attach differently: no function exists
 * until this has realized, since they are built on the bus it creates.
 */
static bool sn0_origin2000_xtalk_build_bridge(SN0Origin2000XtalkState *s,
                                              size_t index, Error **errp)
{
    const SN0ModelIoBoard *record =
        &s->machine->composition->io_boards[index];
    SN0Origin2000XtalkBoard *board = &s->board[index];
    g_autofree char *bus_name = g_strdup_printf("pci.baseio%zu", index);
    DeviceState *bridge = qdev_new(TYPE_SGI_BRIDGE);

    object_property_add_child(board->owner, "bridge", OBJECT(bridge));
    board->bridge = SGI_BRIDGE(bridge);
    object_unref(OBJECT(bridge));

    qdev_prop_set_uint32(bridge, "revision", record->bridge->revision);
    qdev_prop_set_uint64(bridge, "onewire-serial", record->bridge->serial);
    qdev_prop_set_string(bridge, "board-part",
                         s->test_board_part ? s->test_board_part :
                         record->bridge->part);
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
                                           size_t index, Error **errp)
{
    const SN0ModelIoBoard *record =
        &s->machine->composition->io_boards[index];
    SN0Origin2000XtalkBoard *board = &s->board[index];
    unsigned int scsi_index = 0;
    MemoryRegion *face;
    size_t i;

    if (!board->bridge &&
        !sn0_origin2000_xtalk_build_bridge(s, index, errp)) {
        return false;
    }

    face = sysbus_mmio_get_region(SYS_BUS_DEVICE(board->bridge), 0);
    if (!sgi_xbow_attach_endpoint(s->crossbow[record->crossbow_index],
                                  record->widget, face, face, errp)) {
        return false;
    }

    if (!board->pci) {
        board->pci = g_new0(PCIDevice *, record->pci_count);
        board->pci_count = record->pci_count;
    }
    for (i = 0; i < record->pci_count; i++) {
        const SN0ModelComponent *function = &record->pci[i];
        unsigned int instance =
            function->kind == SN0_COMPONENT_ISP1040 ? scsi_index++ : 0;

        if (board->pci[i]) {
            continue;
        }
        board->pci[i] = sn0_origin2000_xtalk_pci_function(board, record,
                                                          function, instance,
                                                          errp);
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
static void sn0_origin2000_xtalk_release_crossbows(SN0Origin2000XtalkState *s)
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
 * back through RCU, a grace period later and on another thread. So nothing
 * unparented above is actually gone until this returns, and until it is gone
 * its own regions still alias the faces and spaces it was wired to. Draining
 * here is what lets the rest of teardown release those.
 *
 * One pass is enough because unparenting a device queues its children's
 * releases before its own, so a subtree comes apart from the inside out.
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
    for (i = 0; i < ARRAY_SIZE(s->board); i++) {
        SN0Origin2000XtalkBoard *board = &s->board[i];

        if (board->bound) {
            memory_region_del_subregion(&board->packet_proxy,
                                        &board->packet_alias);
            memory_region_set_enabled(&board->packet_proxy, false);
            board->bound = false;
        }
    }
    for (i = 0; i < ARRAY_SIZE(s->source); i++) {
        SN0Origin2000XtalkSource *source = &s->source[i];

        if (source->bound) {
            memory_region_del_subregion(&source->packet_proxy,
                                        &source->packet_alias);
            memory_region_del_subregion(&s->machine->nodes[i]->xtalk,
                                        &source->cpu_alias);
            source->bound = false;
        }
    }
    /*
     * Devices stop before the faces they hold spaces over: the board before
     * its proxy, both switches before the Hub decoders they route packets
     * into. The switches go first of all, because a switch's routes alias the
     * board's own face as well as the Hub decoders, so a board released ahead
     * of them would be freed with a route still naming it. An unrealized
     * object here has nothing to undo and says so.
     */
    sn0_origin2000_xtalk_release_crossbows(s);
    for (i = 0; i < ARRAY_SIZE(s->board); i++) {
        SN0Origin2000XtalkBoard *board = &s->board[i];

        /*
         * A board whose chip realized survives a failed attach. Its
         * functions hold host configuration the machine claimed once and
         * cannot claim again: legacy SCSI buses take their numbers from a
         * counter that only counts up, and an ethernet configuration is
         * marked used the first time a device takes it. A second board
         * would come up without the disks and without the station address
         * the command line asked for, and nothing would say so. Nothing
         * about the board depends on the switches, so the replacement
         * switch is handed this same board's face.
         *
         * A chip that never realized has claimed nothing -- its functions
         * are built on the bus it creates -- so that board is released and
         * built again.
         */
        if (terminal || !board->bridge ||
            !DEVICE(board->bridge)->realized) {
            sn0_origin2000_xtalk_release_board(board);
        }
    }
    sn0_origin2000_xtalk_settle_releases();
    if (terminal) {
        for (i = 0; i < ARRAY_SIZE(s->destination); i++) {
            SN0Origin2000XtalkDestination *destination = &s->destination[i];

            if (destination->initialized) {
                sn0_origin2000_hub_io_destroy(&destination->inbound);
                destination->initialized = false;
            }
        }
    } else {
        /*
         * What a retry needs back. The switches are built again rather than
         * reused: a switch keeps the endpoints it was given and refuses a
         * second set, and once unrealized it refuses realization too, so
         * there is nothing to reuse. The Hub decoders and refusing faces are
         * left alone in the other direction -- they are storage of this
         * object's, they can be initialized once only, and nothing is
         * pointing at them now that the switches are gone.
         */
        sn0_origin2000_xtalk_build_crossbows(s);
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

    for (i = 0; i < ARRAY_SIZE(s->source); i++) {
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
            if (DEVICE(&s->machine->nodes[i]->hub)->realized) {
                s->faces_kept = true;
            } else {
                object_property_set_link(OBJECT(&s->machine->nodes[i]->hub),
                                         "xtalk-io", NULL, &error_abort);
                source->linked = false;
            }
        }
        sn0_origin2000_hub_outgoing_io_destroy(&source->outgoing);
    }
    if (s->machine && s->machine->origin2000_xtalk == s) {
        s->machine->origin2000_xtalk = NULL;
    }
}

static void sn0_origin2000_xtalk_unparent(Object *obj)
{
    sn0_origin2000_xtalk_cleanup(SN0_ORIGIN2000_XTALK(obj));
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

bool sn0_origin2000_xtalk_prepare(SN0MachineState *machine, Error **errp)
{
    const SN0ModelComposition *model;
    SN0Origin2000XtalkState *s;
    size_t i;

    if (!machine || !machine->composition) {
        error_setg(errp, "Origin 2000 Crosstalk needs a composition");
        return false;
    }
    if (machine->origin2000_xtalk) {
        error_setg(errp, "Origin 2000 Crosstalk is already prepared");
        return false;
    }
    model = machine->composition;
    if (!sn0_origin2000_xtalk_validate(model, errp)) {
        return false;
    }
    if (!machine->nodes || machine->num_nodes != model->node_count) {
        error_setg(errp, "Origin 2000 Crosstalk needs prepared nodes");
        return false;
    }
    for (i = 0; i < model->node_count; i++) {
        if (!machine->nodes[i] ||
            machine->nodes[i]->physical != &model->nodes[i] ||
            DEVICE(&machine->nodes[i]->hub)->realized) {
            error_setg(errp, "Origin 2000 Crosstalk node %zu is not prepared",
                       i);
            return false;
        }
    }

    s = SN0_ORIGIN2000_XTALK(object_new(TYPE_SN0_ORIGIN2000_XTALK));
    s->machine = machine;
    s->test_crossbow_failure = -1;
    object_property_add_child(OBJECT(machine), "xtalk", OBJECT(s));
    machine->origin2000_xtalk = s;
    object_unref(OBJECT(s));

    sn0_origin2000_xtalk_build_crossbows(s);

    for (i = 0; i < model->node_count; i++) {
        g_autofree char *name =
            g_strdup_printf("node%zu-packet-proxy", i);
        SN0Origin2000XtalkSource *source = &s->source[i];

        memory_region_init(&source->packet_proxy, OBJECT(s), name,
                           ORIGIN2000_PACKET_SIZE);
        memory_region_set_enabled(&source->packet_proxy, false);
        if (!sn0_origin2000_hub_outgoing_io_init(
                &source->outgoing, OBJECT(s), machine->nodes[i],
                &source->packet_proxy, errp) ||
            !object_property_set_link(OBJECT(&machine->nodes[i]->hub),
                                      "xtalk-io",
                                      OBJECT(&source->outgoing.face), errp)) {
            sn0_origin2000_xtalk_destroy(machine);
            return false;
        }
        source->linked = true;
    }

    for (i = 0; i < model->io_board_count; i++) {
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

    return true;
}

/*
 * Everything a Crossbow can reach has to be attached before it is realized,
 * because realize is what builds the ingress regions out of the endpoints.
 * That ordering is why this is a second entry point rather than more of
 * prepare: the Hub faces it attaches do not exist until the nodes are real.
 */
bool sn0_origin2000_xtalk_attach(SN0MachineState *machine, Error **errp)
{
    const SN0ModelComposition *model;
    SN0Origin2000XtalkState *s;
    size_t i;

    if (!machine || !machine->origin2000_xtalk) {
        error_setg(errp, "Origin 2000 Crosstalk is not prepared");
        return false;
    }
    s = machine->origin2000_xtalk;
    if (s->attached) {
        error_setg(errp, "Origin 2000 Crosstalk is already attached");
        return false;
    }
    model = machine->composition;

    /*
     * Checked before anything is touched, so a caller that attaches too
     * early keeps the prepared machine it had rather than losing it to an
     * unwind that has nothing to undo.
     */
    for (i = 0; i < model->node_count; i++) {
        if (!DEVICE(&machine->nodes[i]->hub)->realized) {
            error_setg(errp, "Origin 2000 Crosstalk node %zu is not realized",
                       i);
            return false;
        }
    }

    for (i = 0; i < model->xtalk_attachment_count; i++) {
        const SN0ModelXtalkAttachment *attachment =
            &model->xtalk_attachments[i];
        SN0Origin2000XtalkDestination *destination =
            &s->destination[attachment->node_index];
        SGIXbowState *crossbow = s->crossbow[attachment->crossbow_index];

        destination->owner = s;
        destination->node_index = attachment->node_index;
        destination->widget = attachment->widget;
        /*
         * Built once and kept. Both of these are storage of this object's
         * that can be initialized once only, so a retry reuses what a failed
         * attempt left rather than initializing over it.
         */
        if (!destination->initialized) {
            g_autofree char *name =
                g_strdup_printf("node%u-cpu-face", attachment->node_index);

            if (!sn0_origin2000_hub_io_init(
                    &destination->inbound, OBJECT(s),
                    machine->nodes[attachment->node_index], errp)) {
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
    for (i = 0; i < model->io_board_count; i++) {
        if (!sn0_origin2000_xtalk_fit_board(s, i, errp)) {
            goto unwind;
        }
    }

    for (i = 0; i < model->crossbow_count; i++) {
        if ((int)i == s->test_crossbow_failure) {
            /* A span the provider refuses, set where a retry can clear it. */
            qdev_prop_set_uint64(DEVICE(s->crossbow[i]), "span", 0);
        }
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
    for (i = 0; i < model->xtalk_attachment_count; i++) {
        const SN0ModelXtalkAttachment *attachment =
            &model->xtalk_attachments[i];
        SN0Origin2000XtalkSource *source = &s->source[attachment->node_index];
        SGIXbowState *crossbow = s->crossbow[attachment->crossbow_index];
        MemoryRegion *packet = sgi_xbow_packet_ingress(crossbow,
                                                       attachment->widget);
        MemoryRegion *cpu = sgi_xbow_ingress(crossbow, attachment->widget);
        g_autofree char *packet_name =
            g_strdup_printf("node%u-packet-ingress", attachment->node_index);
        g_autofree char *cpu_name =
            g_strdup_printf("node%u-cpu-ingress", attachment->node_index);

        memory_region_init_alias(&source->packet_alias, OBJECT(s), packet_name,
                                 packet, 0, memory_region_size(packet));
        memory_region_add_subregion(&source->packet_proxy, 0,
                                    &source->packet_alias);
        memory_region_init_alias(&source->cpu_alias, OBJECT(s), cpu_name, cpu,
                                 0, memory_region_size(cpu));
        memory_region_add_subregion(&machine->nodes[attachment->node_index]
                                    ->xtalk, 0, &source->cpu_alias);
        source->bound = true;
        memory_region_set_enabled(&source->packet_proxy, true);
    }

    for (i = 0; i < model->io_board_count; i++) {
        const SN0ModelIoBoard *record = &model->io_boards[i];
        SN0Origin2000XtalkBoard *board = &s->board[i];
        SGIXbowState *crossbow = s->crossbow[record->crossbow_index];
        MemoryRegion *packet = sgi_xbow_packet_ingress(crossbow,
                                                       record->widget);
        g_autofree char *name =
            g_strdup_printf("baseio%zu-packet-ingress", i);

        memory_region_init_alias(&board->packet_alias, board->owner, name,
                                 packet, 0, memory_region_size(packet));
        memory_region_add_subregion(&board->packet_proxy, 0,
                                    &board->packet_alias);
        board->bound = true;
        memory_region_set_enabled(&board->packet_proxy, true);
    }

    s->attached = true;
    return true;

unwind:
    sn0_origin2000_xtalk_cleanup_attach(s, false);
    return false;
}

void sn0_origin2000_xtalk_destroy(SN0MachineState *machine)
{
    SN0Origin2000XtalkState *s;

    if (!machine || !machine->origin2000_xtalk) {
        return;
    }
    s = machine->origin2000_xtalk;
    sn0_origin2000_xtalk_cleanup(s);
    if (!s->faces_kept) {
        object_unparent(OBJECT(s));
        return;
    }
    /*
     * Retired rather than released, because a realized Hub is still pointing
     * at faces that live in here. The name goes back so a later assembly can
     * have it; the storage stays with the machine, disabled and reaching
     * nothing, for as long as the Hubs that alias it.
     */
    object_ref(OBJECT(s));
    object_unparent(OBJECT(s));
    object_property_add_child(OBJECT(machine), "xtalk-retired[*]", OBJECT(s));
    object_unref(OBJECT(s));
}

void sn0_origin2000_xtalk_set_test_board_part(SN0MachineState *machine,
                                              const char *part)
{
    machine->origin2000_xtalk->test_board_part = part;
}

void sn0_origin2000_xtalk_set_test_crossbow_failure(SN0MachineState *machine,
                                                    int index)
{
    machine->origin2000_xtalk->test_crossbow_failure = index;
}

bool sn0_origin2000_xtalk_test_proxies_disabled(SN0MachineState *machine)
{
    SN0Origin2000XtalkState *s = machine->origin2000_xtalk;
    size_t i;

    if (!s) {
        return false;
    }
    for (i = 0; i < ARRAY_SIZE(s->source); i++) {
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
