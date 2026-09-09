/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Descriptor-owned Origin 2000 Crosstalk assembly storage. */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/sgi-xbow.h"
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
    bool linked;
} SN0Origin2000XtalkSource;

struct SN0Origin2000XtalkState {
    Object parent_obj;

    SN0MachineState *machine;
    SGIXbowState *crossbow[ORIGIN2000_XTALK_CROSSBOWS];
    SN0Origin2000XtalkSource source[ORIGIN2000_XTALK_NODES];
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
