/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Descriptor-owned Origin 2000 Crosstalk assembly storage. */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-xbow.h"
#include "qemu/error-report.h"
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

struct SN0Origin2000XtalkState {
    Object parent_obj;

    SN0MachineState *machine;
    SGIXbowState *crossbow[ORIGIN2000_XTALK_CROSSBOWS];
    SN0Origin2000XtalkSource source[ORIGIN2000_XTALK_NODES];
    SN0Origin2000XtalkDestination destination[ORIGIN2000_XTALK_NODES];
    bool attached;
    Object *baseio[ORIGIN2000_XTALK_IO_BOARDS];
    MemoryRegion board_packet_proxy[ORIGIN2000_XTALK_IO_BOARDS];
    bool cleaned;
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
 * called alone. The request is asynchronous and qtest can keep issuing
 * accesses after it, so every access refuses on its own rather than trusting
 * the machine to have stopped.
 */
static MemTxResult sn0_origin2000_cpu_refusal(void *opaque, hwaddr addr,
                                              uint64_t *data, unsigned size,
                                              bool is_write, MemTxAttrs attrs)
{
    SN0Origin2000XtalkDestination *destination = opaque;

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

static void sn0_origin2000_xtalk_cleanup(SN0Origin2000XtalkState *s)
{
    size_t i;

    if (s->cleaned) {
        return;
    }
    s->cleaned = true;

    for (i = 0; i < ARRAY_SIZE(s->crossbow); i++) {
        if (s->crossbow[i]) {
            sgi_xbow_quiesce(s->crossbow[i]);
        }
    }
    /*
     * Aliases come out before the ingress regions they point into, and the
     * inbound decoders come out before the node regions they root on. Both
     * orders matter more than they look: an alias left behind outlives the
     * region under it.
     */
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
    for (i = 0; i < ARRAY_SIZE(s->destination); i++) {
        SN0Origin2000XtalkDestination *destination = &s->destination[i];

        if (destination->initialized) {
            sn0_origin2000_hub_io_destroy(&destination->inbound);
            destination->initialized = false;
        }
    }
    s->attached = false;
    for (i = 0; i < ARRAY_SIZE(s->source); i++) {
        SN0Origin2000XtalkSource *source = &s->source[i];

        if (source->linked) {
            object_property_set_link(OBJECT(&s->machine->nodes[i]->hub),
                                     "xtalk-io", NULL, &error_abort);
            source->linked = false;
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
    object_property_add_child(OBJECT(machine), "xtalk", OBJECT(s));
    machine->origin2000_xtalk = s;
    object_unref(OBJECT(s));

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

        s->baseio[i] = object_new(TYPE_CONTAINER);
        object_property_add_child(OBJECT(s), owner_name, s->baseio[i]);
        object_unref(s->baseio[i]);
        memory_region_init(&s->board_packet_proxy[i], s->baseio[i],
                           proxy_name, ORIGIN2000_PACKET_SIZE);
        memory_region_set_enabled(&s->board_packet_proxy[i], false);
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
        g_autofree char *name = NULL;

        destination->owner = s;
        destination->node_index = attachment->node_index;
        destination->widget = attachment->widget;
        if (!sn0_origin2000_hub_io_init(&destination->inbound, OBJECT(s),
                                        machine->nodes[attachment->node_index],
                                        errp)) {
            goto unwind;
        }
        destination->initialized = true;

        name = g_strdup_printf("node%u-cpu-face", attachment->node_index);
        memory_region_init_io(&destination->cpu_refusal, OBJECT(s),
                              &sn0_origin2000_cpu_refusal_ops, destination,
                              name, SN0_XTALK_WIDGET_SIZE);
        if (!sgi_xbow_attach_endpoint(crossbow, attachment->widget,
                                      &destination->cpu_refusal,
                                      &destination->inbound.face, errp)) {
            goto unwind;
        }
    }

    for (i = 0; i < model->crossbow_count; i++) {
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

    s->attached = true;
    return true;

unwind:
    sn0_origin2000_xtalk_destroy(machine);
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
    object_unparent(OBJECT(s));
}

bool sn0_origin2000_xtalk_test_proxies_disabled(SN0MachineState *machine)
{
    SN0Origin2000XtalkState *s = machine->origin2000_xtalk;
    size_t i, j;

    if (!s) {
        return false;
    }
    for (i = 0; i < ARRAY_SIZE(s->source); i++) {
        if (s->source[i].packet_proxy.enabled ||
            memory_region_size(&s->source[i].packet_proxy) !=
            ORIGIN2000_PACKET_SIZE) {
            return false;
        }
        for (j = 0; j < i; j++) {
            if (&s->source[i].packet_proxy ==
                &s->source[j].packet_proxy) {
                return false;
            }
        }
    }
    return !s->board_packet_proxy[0].enabled &&
        memory_region_size(&s->board_packet_proxy[0]) ==
        ORIGIN2000_PACKET_SIZE;
}
