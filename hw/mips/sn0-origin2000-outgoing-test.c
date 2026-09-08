/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit test machine: per-physical-source outgoing transport observation. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/rcu.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/sgi-xbow.h"
#include "system/address-spaces.h"
#include "sn0-origin2000-io.h"

#define TYPE_OUTGOING_TEST MACHINE_TYPE_NAME("origin2000-hub-outgoing-test")
OBJECT_DECLARE_SIMPLE_TYPE(SN0OutgoingTest, OUTGOING_TEST)

#define TEST_CONTROL 0x40000000000ULL
#define TEST_VALUE 0x123456789abcdef0ULL

#define TYPE_OUTGOING_LIFETIME "sn0-outgoing-lifetime-test"
OBJECT_DECLARE_SIMPLE_TYPE(SN0OutgoingLifetime, OUTGOING_LIFETIME)

struct SN0OutgoingLifetime {
    Object parent_obj;
    SN0Origin2000HubOutgoingIO outgoing;
    SN0Origin2000HubIO receiver;
    MemoryRegion proxy;
    SN0OutgoingTest *test;
    bool receiving;
};

typedef struct SN0LifetimeMarker {
    struct rcu_head rcu;
    SN0OutgoingTest *test;
} SN0LifetimeMarker;


typedef struct SN0OutgoingSource {
    SN0OutgoingTest *test;
    unsigned int index;
    SN0Origin2000HubOutgoingIO outgoing;
    SN0Origin2000HubIO receiver;
    MemoryRegion cpu_face;
    MemoryRegion proxy;
    MemoryRegion alias;
    MemoryRegion recording;
    MemoryRegion memory;
    AddressSpace *accesses;
    bool bound;
} SN0OutgoingSource;

struct SN0OutgoingTest {
    SN0MachineState parent_obj;
    SN0OutgoingSource *source;
    SGIXbowState **xbow;
    MemoryRegion control;
    bool record_only;
    bool check_geometry;
    bool check_lifetime;
    bool receiver_lifetime;
    SN0OutgoingLifetime *lifetime;
    AddressSpace *retained;
    unsigned int quiesced;
    unsigned int finalized;
    bool cleaned;
    unsigned int prepared_sources;
    uint64_t address;
    uint64_t data;
    unsigned int width;
    unsigned int selected;
    MemTxResult result;
    uint64_t packets;
    uint64_t last_packet;
    uint64_t last_attrs;
    uint64_t last_width;
    uint64_t last_source;
    uint64_t memories;
    uint64_t last_memory;
    bool fail;
};

static MemTxResult record_read(void *opaque, hwaddr addr, uint64_t *data,
                               unsigned size, MemTxAttrs attrs)
{
    SN0OutgoingSource *source = opaque;
    SN0OutgoingTest *s = source->test;

    g_assert(source->bound);
    s->packets++;
    s->last_packet = addr;
    s->last_attrs = attrs.requester_id | ((uint64_t)attrs.secure << 16);
    s->last_width = size;
    s->last_source = source->index;
    *data = TEST_VALUE;
    return s->fail ? MEMTX_ERROR : MEMTX_OK;
}

static MemTxResult record_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size, MemTxAttrs attrs)
{
    return record_read(opaque, addr, &data, size, attrs);
}

static const MemoryRegionOps record_ops = {
    .read_with_attrs = record_read,
    .write_with_attrs = record_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static uint64_t memory_read(void *opaque, hwaddr addr, unsigned size)
{
    SN0OutgoingSource *source = opaque;
    source->test->memories++;
    source->test->last_memory = addr;
    return TEST_VALUE;
}

static void memory_write(void *opaque, hwaddr addr, uint64_t data,
                         unsigned size)
{
    memory_read(opaque, addr, size);
}

static const MemoryRegionOps memory_ops = {
    .read = memory_read,
    .write = memory_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void outgoing_test_cleanup(SN0OutgoingTest *s);

static void lifetime_marker(SN0LifetimeMarker *marker)
{
    qatomic_set(&marker->test->quiesced, 1);
    object_unref(OBJECT(marker->test));
    g_free(marker);
}

static void lifetime_detach(SN0OutgoingTest *s)
{
    SN0LifetimeMarker *marker;

    if (!s->lifetime) {
        return;
    }
    if (s->lifetime->receiving) {
        sn0_origin2000_hub_io_destroy(&s->lifetime->receiver);
    } else {
        sn0_origin2000_hub_outgoing_io_destroy(&s->lifetime->outgoing);
    }
    object_unparent(OBJECT(s->lifetime));
    s->lifetime = NULL;
    marker = g_new0(SN0LifetimeMarker, 1);
    marker->test = s;
    object_ref(OBJECT(s));
    call_rcu(marker, lifetime_marker, rcu);
}

static uint64_t control_read(void *opaque, hwaddr addr, unsigned size)
{
    SN0OutgoingTest *s = opaque;

    switch (addr) {
    case 0: return s->address;
    case 8: return s->data;
    case 16: return s->width;
    case 32: return s->result;
    case 40: return s->packets;
    case 48: return s->last_attrs;
    case 56: return s->last_packet;
    case 64: return s->last_width;
    case 72: return s->fail;
    case 80: return s->selected;
    case 88: return s->last_source;
    case 96: return s->memories;
    case 104: return s->last_memory;
    case 112: return s->cleaned;
    case 120: return qatomic_read(&s->quiesced);
    case 128: return qatomic_read(&s->finalized);
    default: return 0;
    }
}

static void control_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    SN0OutgoingTest *s = opaque;
    uint8_t bytes[8] = { 0 };
    MemTxAttrs attrs = { .requester_id = 0x1234, .secure = true };

    if (s->cleaned && addr != 112) {
        s->result = MEMTX_ERROR;
        return;
    }
    switch (addr) {
    case 120:
        lifetime_detach(s);
        break;
    case 128:
        if (s->retained) {
            address_space_destroy_free(g_steal_pointer(&s->retained));
        }
        break;
    case 136:
        s->result = s->retained ?
            address_space_read(s->retained, (8ULL << 34) | 0x01600000,
                               attrs, bytes, sizeof(bytes)) : MEMTX_ERROR;
        break;
    case 112:
        outgoing_test_cleanup(s);
        break;
    case 0:
        s->address = value;
        break;
    case 8:
        s->data = value;
        break;
    case 16:
        s->width = value;
        break;
    case 24:
        if (s->selected >= SN0_MACHINE(s)->num_nodes ||
            (s->width != 1 && s->width != 2 &&
             s->width != 4 && s->width != 8)) {
            s->result = MEMTX_ERROR;
            break;
        }
        stq_be_p(bytes, s->data);
        s->result = address_space_rw(s->source[s->selected].accesses,
                                     s->address, attrs, bytes + 8 - s->width,
                                     s->width, value != 0);
        if (!value) {
            s->data = ldq_be_p(bytes);
        }
        break;
    case 40:
        s->packets = 0;
        break;
    case 72:
        s->fail = value != 0;
        break;
    case 80:
        s->selected = value;
        break;
    }
}

static const MemoryRegionOps control_ops = {
    .read = control_read,
    .write = control_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static MemTxResult unsupported_cpu_read(void *opaque, hwaddr addr,
                                         uint64_t *data, unsigned size,
                                         MemTxAttrs attrs)
{
    *data = 0;
    qemu_log_mask(LOG_UNIMP,
                  "test: Hub CPU-facing widget decode is unsupported\n");
    return MEMTX_ERROR;
}

static MemTxResult unsupported_cpu_write(void *opaque, hwaddr addr,
                                          uint64_t data, unsigned size,
                                          MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP,
                  "test: Hub CPU-facing widget decode is unsupported\n");
    return MEMTX_ERROR;
}

static const MemoryRegionOps unsupported_cpu_ops = {
    .read_with_attrs = unsupported_cpu_read,
    .write_with_attrs = unsupported_cpu_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static const SN0ModelXtalkAttachment *source_attachment(SN0OutgoingTest *s,
                                                       unsigned node)
{
    const SN0ModelComposition *c = SN0_MACHINE(s)->composition;
    unsigned i;

    for (i = 0; i < c->xtalk_attachment_count; i++) {
        if (c->xtalk_attachments[i].node_index == node) {
            return &c->xtalk_attachments[i];
        }
    }
    g_assert_not_reached();
}

static void lifetime_create(SN0OutgoingTest *s, SN0NodeState *node,
                            bool receiving)
{
    SN0OutgoingLifetime *life = OUTGOING_LIFETIME(
        object_new(TYPE_OUTGOING_LIFETIME));
    MemoryRegion *face;

    object_property_add_child(OBJECT(s), "lifetime-probe", OBJECT(life));
    object_unref(OBJECT(life));
    s->lifetime = life;
    life->test = s;
    life->receiving = receiving;
    object_ref(OBJECT(s));
    if (receiving) {
        sn0_origin2000_hub_io_init(&life->receiver, OBJECT(life), node,
                                   &error_fatal);
        face = &life->receiver.face;
    } else {
        memory_region_init(&life->proxy, OBJECT(life), "lifetime-proxy",
                           1ULL << 52);
        sn0_origin2000_hub_outgoing_io_init(&life->outgoing, OBJECT(life),
                                           node, &life->proxy, &error_fatal);
        face = &life->outgoing.face;
    }
    s->retained = g_new0(AddressSpace, 1);
    address_space_init(s->retained, face, "test-retained-face");
}

static void outgoing_test_init(MachineState *machine)
{
    SN0OutgoingTest *s = OUTGOING_TEST(machine);
    SN0MachineState *sn0 = SN0_MACHINE(machine);
    const SN0ModelComposition *c;
    unsigned int i, j;

    sn0_origin2000_prepare_nodes(machine);
    c = sn0->composition;
    if (s->check_lifetime && !s->receiver_lifetime) {
        lifetime_create(s, sn0->nodes[0], false);
    }
    if (s->check_geometry) {
        unsigned int bits;
        for (bits = 51; bits <= 53; bits += 2) {
            MemoryRegion wrong;
            SN0Origin2000HubOutgoingIO probe = { 0 };
            Error *err = NULL;
            memory_region_init(&wrong, OBJECT(machine), "wrong-packet-span",
                               1ULL << bits);
            g_assert(!sn0_origin2000_hub_outgoing_io_init(
                &probe, OBJECT(s), sn0->nodes[0], &wrong, &err));
            g_assert(err);
            error_free(err);
            object_unparent(OBJECT(&wrong));
        }
        {
            MemoryRegion uninitialized = { 0 };
            SN0Origin2000HubOutgoingIO probe = { 0 };
            Error *err = NULL;
            g_assert(!sn0_origin2000_hub_outgoing_io_init(
                &probe, OBJECT(s), sn0->nodes[0], &uninitialized, &err));
            g_assert(err);
            error_free(err);
        }
        for (bits = 0; bits < 2; bits++) {
            DeviceState *hub = qdev_new(TYPE_SGI_HUB);
            MemoryRegion wrong;
            Error *err = NULL;
            memory_region_init(&wrong, OBJECT(machine), "wrong-io-span",
                               1ULL << (bits ? 38 : 37));
            object_property_set_link(OBJECT(hub), "xtalk", OBJECT(&wrong),
                                     &error_abort);
            object_property_set_link(OBJECT(hub), "xtalk-io", OBJECT(&wrong),
                                     &error_abort);
            qdev_prop_set_uint32(hub, "xtalk-widget-shift", bits ? 33 : 34);
            g_assert(!sysbus_realize(SYS_BUS_DEVICE(hub), &err));
            g_assert(strstr(error_get_pretty(err),
                                    "Hub I/O target requires"));
            error_free(err);
            /* Failed bus attachment is released later through RCU. */
            object_property_set_link(OBJECT(hub), "xtalk-io", NULL,
                                     &error_abort);
            object_property_set_link(OBJECT(hub), "xtalk", NULL, &error_abort);
            object_unref(OBJECT(hub));
            object_unparent(OBJECT(&wrong));
        }
    }
    s->source = g_new0(SN0OutgoingSource, sn0->num_nodes);
    for (i = 0; i < sn0->num_nodes; i++) {
        g_assert(!DEVICE(&sn0->nodes[i]->hub)->realized);
    }
    for (i = 0; i < sn0->num_nodes; i++) {
        SN0NodeState *node = sn0->nodes[i];
        SN0OutgoingSource *source = &s->source[i];

        source->test = s;
        source->index = i;
        memory_region_init(&source->proxy, OBJECT(s), "test-packet-proxy",
                           1ULL << 52);
        if (!sn0_origin2000_hub_outgoing_io_init(&source->outgoing, OBJECT(s),
                                                node, &source->proxy,
                                                &error_fatal)) {
            return;
        }
        s->prepared_sources++;
        if (s->check_geometry) {
            Error *err = NULL;
            g_assert(!sn0_origin2000_hub_outgoing_io_init(
                &source->outgoing, OBJECT(s), node, &source->proxy, &err));
            g_assert(err);
            error_free(err);
            {
                AddressSpace *unbound = g_new0(AddressSpace, 1);
                uint8_t bytes[8];
                address_space_init(unbound, &source->outgoing.face,
                                   "test-unbound-outgoing");
                g_assert(address_space_read(unbound,
                         (8ULL << SN0_XTALK_WIDGET_SHIFT) | 0x01600000,
                         MEMTXATTRS_UNSPECIFIED, bytes, sizeof(bytes)) !=
                         MEMTX_OK);
                address_space_destroy_free(unbound);
            }
        }
        object_property_set_link(OBJECT(&node->hub), "xtalk-io",
                                 OBJECT(&source->outgoing.face), &error_fatal);
        g_assert(node->hub.xtalk_io == &source->outgoing.face);
        for (j = 0; j < i; j++) {
            g_assert(&source->proxy != &s->source[j].proxy);
            g_assert(node->hub.xtalk_io != sn0->nodes[j]->hub.xtalk_io);
        }
    }
    sn0_origin2000_realize_nodes(machine);
    if (s->check_lifetime && s->receiver_lifetime) {
        lifetime_create(s, sn0->nodes[0], true);
    }
    if (!s->record_only) {
        s->xbow = g_new0(SGIXbowState *, c->crossbow_count);
        for (i = 0; i < c->crossbow_count; i++) {
            g_autofree char *name = g_strdup_printf("test-crossbow%u", i);
            const SN0ModelCrossbow *model = &c->crossbows[i];
            DeviceState *dev = qdev_new(TYPE_SGI_XBOW);
            unsigned int present = 0;

            object_property_add_child(OBJECT(machine), name, OBJECT(dev));
            s->xbow[i] = SGI_XBOW(dev);
            object_unref(OBJECT(dev));
            for (j = 0; j < c->xtalk_attachment_count; j++) {
                if (c->xtalk_attachments[j].crossbow_index == i) {
                    present |= 1U << c->xtalk_attachments[j].widget;
                }
            }
            qdev_prop_set_uint64(dev, "span", SN0_XTALK_WIDGET_SIZE);
            qdev_prop_set_uint32(dev, "revision", model->revision);
            qdev_prop_set_uint32(dev, "part", model->part);
            qdev_prop_set_uint32(dev, "manufacturer", model->manufacturer);
            qdev_prop_set_uint16(dev, "links-present", present);
            qdev_prop_set_uint16(dev, "startup-links-alive",
                                 present & model->initially_operational_links);
        }
        for (i = 0; i < sn0->num_nodes; i++) {
            SN0OutgoingSource *source = &s->source[i];
            SN0NodeState *node = sn0->nodes[i];
            const SN0ModelXtalkAttachment *a = source_attachment(s, i);

            g_assert(DEVICE(&node->hub)->realized);
            sn0_origin2000_hub_io_init(&source->receiver, OBJECT(s), node,
                                       &error_fatal);
            memory_region_init_io(&source->cpu_face, OBJECT(s),
                                  &unsupported_cpu_ops, NULL,
                                  "test-unsupported-hub-cpu-face",
                                  SN0_XTALK_WIDGET_SIZE);
            sgi_xbow_attach_endpoint(s->xbow[a->crossbow_index], a->widget,
                                     &source->cpu_face, &source->receiver.face,
                                     &error_fatal);
        }
        for (i = 0; i < c->crossbow_count; i++) {
            sysbus_realize(SYS_BUS_DEVICE(s->xbow[i]), &error_fatal);
        }
    }
    for (i = 0; i < sn0->num_nodes; i++) {
        SN0OutgoingSource *source = &s->source[i];
        SN0NodeState *node = sn0->nodes[i];
        MemoryRegion *target;

        g_assert(DEVICE(&node->hub)->realized);
        if (s->record_only) {
            g_autofree char *name =
                g_strdup_printf("test-packet-recorder%u", i);
            memory_region_init_io(&source->recording, OBJECT(machine),
                                  &record_ops, source, name, 1ULL << 52);
            target = &source->recording;
        } else {
            const SN0ModelXtalkAttachment *a = source_attachment(s, i);
            target = sgi_xbow_packet_ingress(s->xbow[a->crossbow_index],
                                            a->widget);
        }
        memory_region_init_alias(&source->alias, OBJECT(s),
                                 "test-packet-alias",
                                 target, 0, 1ULL << 52);
        memory_region_add_subregion(&source->proxy, 0, &source->alias);
        source->bound = true;
        source->accesses = g_new0(AddressSpace, 1);
        address_space_init(source->accesses, &source->outgoing.face,
                           "test-outgoing-access");
        if (s->record_only) {
            g_autofree char *name =
                g_strdup_printf("test-memory-recorder%u", i);
            memory_region_init_io(&source->memory, OBJECT(machine), &memory_ops,
                                  source, name, 1ULL << 38);
            memory_region_add_subregion(&node->xtalk, 0, &source->memory);
        }
    }
    memory_region_init_io(&s->control, OBJECT(machine), &control_ops, s,
                          "test-outgoing-control", 144);
    memory_region_add_subregion(get_system_memory(), TEST_CONTROL, &s->control);
}

/* Test teardown runs before QOM releases either endpoint owner. */
static void outgoing_test_cleanup(SN0OutgoingTest *s)
{
    SN0MachineState *sn0 = SN0_MACHINE(s);
    unsigned i, n;

    if (s->cleaned) {
        return;
    }
    s->cleaned = true;
    lifetime_detach(s);
    if (s->retained) {
        address_space_destroy_free(g_steal_pointer(&s->retained));
    }
    for (i = 0; i < s->prepared_sources; i++) {
        SN0OutgoingSource *source = &s->source[i];
        SGIHubState *hub = &sn0->nodes[i]->hub;
        MemoryRegion *io_target = hub->xtalk_io;

        if (DEVICE(hub)->realized) {
            for (n = 0; n < SGI_HUB_BWIN_ENTRIES; n++) {
                memory_region_del_subregion(&hub->bwin,
                                            &hub->bwin_io_window[n]);
                object_unparent(OBJECT(&hub->bwin_io_window[n]));
            }
        }
        /* Drop the strong QOM link after removing every alias that uses it. */
        hub->xtalk_io = NULL;
        object_unref(OBJECT(io_target));
        if (source->bound) {
            address_space_destroy_free(g_steal_pointer(&source->accesses));
            memory_region_del_subregion(&source->proxy, &source->alias);
            object_unparent(OBJECT(&source->alias));
            source->bound = false;
        }
        sn0_origin2000_hub_outgoing_io_destroy(&source->outgoing);
        object_unparent(OBJECT(&source->proxy));
    }
    if (s->xbow) {
        for (i = 0; i < sn0->composition->crossbow_count; i++) {
            if (s->xbow[i]) {
                if (DEVICE(s->xbow[i])->realized) {
                    qdev_unrealize(DEVICE(s->xbow[i]));
                }
                object_unparent(OBJECT(s->xbow[i]));
            }
        }
    }
    for (i = 0; i < s->prepared_sources; i++) {
        SN0OutgoingSource *source = &s->source[i];
        if (memory_region_size(&source->receiver.face)) {
            sn0_origin2000_hub_io_destroy(&source->receiver);
        }
        if (memory_region_size(&source->memory)) {
            memory_region_del_subregion(&sn0->nodes[i]->xtalk, &source->memory);
        }
    }
}

static ObjectUnparent *parent_unparent;

static void outgoing_test_unparent(Object *obj)
{
    SN0OutgoingTest *s = OUTGOING_TEST(obj);

    outgoing_test_cleanup(s);
    if (memory_region_size(&s->control)) {
        memory_region_del_subregion(get_system_memory(), &s->control);
    }
    if (parent_unparent) {
        parent_unparent(obj);
    }
}

static void outgoing_test_finalize(Object *obj)
{
    SN0OutgoingTest *s = OUTGOING_TEST(obj);

    g_free(s->source);
    g_free(s->xbow);
}

static bool get_record_only(Object *obj, Error **errp)
{
    return OUTGOING_TEST(obj)->record_only;
}

static void set_record_only(Object *obj, bool value, Error **errp)
{
    OUTGOING_TEST(obj)->record_only = value;
}

static bool get_check_geometry(Object *obj, Error **errp)
{
    return OUTGOING_TEST(obj)->check_geometry;
}

static void set_check_geometry(Object *obj, bool value, Error **errp)
{
    OUTGOING_TEST(obj)->check_geometry = value;
}

static bool get_check_lifetime(Object *obj, Error **errp)
{
    return OUTGOING_TEST(obj)->check_lifetime;
}

static void set_check_lifetime(Object *obj, bool value, Error **errp)
{
    OUTGOING_TEST(obj)->check_lifetime = value;
}

static bool get_receiver_lifetime(Object *obj, Error **errp)
{
    return OUTGOING_TEST(obj)->receiver_lifetime;
}

static void set_receiver_lifetime(Object *obj, bool value, Error **errp)
{
    OUTGOING_TEST(obj)->receiver_lifetime = value;
}

static void lifetime_finalize(Object *obj)
{
    SN0OutgoingLifetime *life = OUTGOING_LIFETIME(obj);

    qatomic_inc(&life->test->finalized);
    object_unref(OBJECT(life->test));
}

static const TypeInfo lifetime_type = {
    .name = TYPE_OUTGOING_LIFETIME,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(SN0OutgoingLifetime),
    .instance_finalize = lifetime_finalize,
};

static void outgoing_test_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    parent_unparent = oc->unparent;
    oc->unparent = outgoing_test_unparent;
    mc->desc = "Origin 2000 outgoing Hub test fixture";
    mc->init = outgoing_test_init;
    object_class_property_add_bool(oc, "receiver-lifetime",
                                    get_receiver_lifetime,
                                    set_receiver_lifetime);
    object_class_property_add_bool(oc, "check-lifetime", get_check_lifetime,
                                    set_check_lifetime);
    object_class_property_add_bool(oc, "check-geometry", get_check_geometry,
                                    set_check_geometry);
    object_class_property_add_bool(oc, "record-only", get_record_only,
                                    set_record_only);
}

static const TypeInfo outgoing_test_type = {
    .name = TYPE_OUTGOING_TEST,
    .parent = MACHINE_TYPE_NAME("origin2000"),
    .instance_size = sizeof(SN0OutgoingTest),
    .class_init = outgoing_test_class_init,
    .instance_finalize = outgoing_test_finalize,
};

static void outgoing_test_register_types(void)
{
    type_register_static(&lifetime_type);
    type_register_static(&outgoing_test_type);
}
type_init(outgoing_test_register_types)
