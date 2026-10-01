/* SPDX-License-Identifier: BSD-3-Clause */
/* Synthetic controller associations; these registers are not an L1 bus. */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/sgi-l1.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define TYPE_L1_QUERY_FIXTURE "sgi-l1-query-routing-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(L1QueryFixtureState, L1_QUERY_FIXTURE)

struct L1QueryFixtureState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    DeviceState *controllers[4];
    SGIL1QueryOwner owners[4];
    SGIL1QueryAssociation associations[12];
    SGIL1QueryRouter router;
    SGIL1QueryAttachment attachments[4];
    uint32_t attached_owner[4];
    uint32_t attachment_count;
    uint32_t topology;
    uint32_t initial_reachable;
    uint32_t association_count;
};

static uint32_t fixture_reachable(L1QueryFixtureState *s)
{
    uint32_t result = 0;

    for (unsigned i = 0; i < s->association_count; i++) {
        SGIL1QueryAssociation *a = &s->associations[i];
        if (a->reachable) {
            result |= 1u << (a->from * 4 + a->to);
        }
    }
    return result;
}

static void fixture_set_reachable(L1QueryFixtureState *s, uint32_t value)
{
    for (unsigned i = 0; i < s->association_count; i++) {
        SGIL1QueryAssociation *a = &s->associations[i];
        a->reachable = (value >> (a->from * 4 + a->to)) & 1;
    }
}

static uint64_t fixture_read(void *opaque, hwaddr offset, unsigned size)
{
    L1QueryFixtureState *s = opaque;

    switch (offset) {
    case 0:
        return fixture_reachable(s);
    case 8:
        return s->router.active;
    default:
        if (offset >= 32 && offset < 64) {
            return s->owners[(offset - 32) / 8].identity;
        }
        return 0;
    }
}

static void fixture_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    L1QueryFixtureState *s = opaque;

    switch (offset) {
    case 0:
        fixture_set_reachable(s, value);
        break;
    case 8:
        /* Construct an enclosing route to exercise the nested-call refusal. */
        s->router.active = value != 0;
        break;
    case 16:
        if (value < 4) {
            resettable_reset(OBJECT(s->controllers[value]), RESET_TYPE_COLD);
        }
        break;
    }
}

static const MemoryRegionOps fixture_ops = {
    .read = fixture_read,
    .write = fixture_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void fixture_realize(DeviceState *dev, Error **errp)
{
    L1QueryFixtureState *s = L1_QUERY_FIXTURE(dev);
    Error *local_error = NULL;

    if ((s->topology & ~UINT32_C(0xffff)) ||
        (s->topology & UINT32_C(0x8421)) ||
        (s->initial_reachable & ~s->topology)) {
        error_setg(errp, "Invalid synthetic L1 association topology");
        return;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (!s->controllers[i]) {
            error_setg(errp, "Synthetic L1 owner %u needs a controller", i);
            return;
        }
        s->owners[i].controller = SGI_L1(s->controllers[i]);
        for (unsigned j = 0; j < 4; j++) {
            if (s->topology & (1u << (i * 4 + j))) {
                SGIL1QueryAssociation *a =
                    &s->associations[s->association_count++];
                a->from = i;
                a->to = j;
                a->reachable = (s->initial_reachable >> (i * 4 + j)) & 1;
            }
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        if (s->attached_owner[i] != UINT32_MAX) {
            SGIL1QueryAttachment *a = &s->attachments[s->attachment_count++];
            a->issuer = i;
            a->owner = s->attached_owner[i];
        }
    }
    sgi_l1_query_router_init(&s->router, s->owners, 4, s->associations,
                            s->association_count, &local_error);
    if (local_error) {
        error_propagate(errp, local_error);
        return;
    }
    if (!sgi_l1_query_router_set_attached(&s->router, s->attachments,
                                         s->attachment_count, errp)) {
        sgi_l1_query_router_destroy(&s->router);
    }
}

static void fixture_unrealize(DeviceState *dev)
{
    L1QueryFixtureState *s = L1_QUERY_FIXTURE(dev);

    sgi_l1_query_router_destroy(&s->router);
}

static void fixture_reset_hold(Object *obj, ResetType type)
{
    L1QueryFixtureState *s = L1_QUERY_FIXTURE(obj);

    s->router.active = false;
    fixture_set_reachable(s, s->initial_reachable);
}

static int fixture_pre_save(void *opaque)
{
    L1QueryFixtureState *s = opaque;

    return s->router.active ? -EBUSY : 0;
}

static const VMStateDescription vmstate_query_owner = {
    .name = "sgi-l1-query-routing-fixture/owner",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_EQUAL(identity, SGIL1QueryOwner),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_query_association = {
    .name = "sgi-l1-query-routing-fixture/association",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(from, SGIL1QueryAssociation),
        VMSTATE_UINT32_EQUAL(to, SGIL1QueryAssociation),
        VMSTATE_BOOL(reachable, SGIL1QueryAssociation),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_query_attachment = {
    .name = "sgi-l1-query-routing-fixture/attachment",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(issuer, SGIL1QueryAttachment),
        VMSTATE_UINT32_EQUAL(owner, SGIL1QueryAttachment),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_query_fixture = {
    .name = TYPE_L1_QUERY_FIXTURE,
    .version_id = 2,
    .minimum_version_id = 2,
    .pre_save = fixture_pre_save,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(topology, L1QueryFixtureState),
        VMSTATE_UINT32_EQUAL(initial_reachable, L1QueryFixtureState),
        VMSTATE_UINT32_EQUAL(association_count, L1QueryFixtureState),
        VMSTATE_UINT32_EQUAL(attachment_count, L1QueryFixtureState),
        VMSTATE_STRUCT_ARRAY(attachments, L1QueryFixtureState, 4, 0,
                             vmstate_query_attachment, SGIL1QueryAttachment),
        VMSTATE_STRUCT_ARRAY(owners, L1QueryFixtureState, 4, 0,
                             vmstate_query_owner, SGIL1QueryOwner),
        VMSTATE_STRUCT_ARRAY(associations, L1QueryFixtureState, 12, 0,
                             vmstate_query_association, SGIL1QueryAssociation),
        VMSTATE_END_OF_LIST()
    },
};

static void fixture_init(Object *obj)
{
    L1QueryFixtureState *s = L1_QUERY_FIXTURE(obj);

    memory_region_init_io(&s->mmio, obj, &fixture_ops, s,
                          TYPE_L1_QUERY_FIXTURE, 64);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static const Property fixture_properties[] = {
    DEFINE_PROP_LINK("owner0", L1QueryFixtureState, controllers[0],
                     TYPE_SGI_L1, DeviceState *),
    DEFINE_PROP_UINT64("identity0", L1QueryFixtureState,
                       owners[0].identity, 0),
    DEFINE_PROP_LINK("owner1", L1QueryFixtureState, controllers[1],
                     TYPE_SGI_L1, DeviceState *),
    DEFINE_PROP_UINT64("identity1", L1QueryFixtureState,
                       owners[1].identity, 0),
    DEFINE_PROP_LINK("owner2", L1QueryFixtureState, controllers[2],
                     TYPE_SGI_L1, DeviceState *),
    DEFINE_PROP_UINT64("identity2", L1QueryFixtureState,
                       owners[2].identity, 0),
    DEFINE_PROP_LINK("owner3", L1QueryFixtureState, controllers[3],
                     TYPE_SGI_L1, DeviceState *),
    DEFINE_PROP_UINT64("identity3", L1QueryFixtureState,
                       owners[3].identity, 0),
    DEFINE_PROP_UINT32("attached0", L1QueryFixtureState, attached_owner[0],
                       UINT32_MAX),
    DEFINE_PROP_UINT32("attached1", L1QueryFixtureState, attached_owner[1],
                       UINT32_MAX),
    DEFINE_PROP_UINT32("attached2", L1QueryFixtureState, attached_owner[2],
                       UINT32_MAX),
    DEFINE_PROP_UINT32("attached3", L1QueryFixtureState, attached_owner[3],
                       UINT32_MAX),
    DEFINE_PROP_UINT32("topology", L1QueryFixtureState, topology, 0),
    DEFINE_PROP_UINT32("reachable", L1QueryFixtureState, initial_reachable, 0),
};

static void fixture_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "Synthetic L1 controller query associations";
    dc->user_creatable = true;
    dc->realize = fixture_realize;
    dc->unrealize = fixture_unrealize;
    dc->vmsd = &vmstate_query_fixture;
    rc->phases.hold = fixture_reset_hold;
    device_class_set_props(dc, fixture_properties);
}

static const TypeInfo fixture_type = {
    .name = TYPE_L1_QUERY_FIXTURE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(L1QueryFixtureState),
    .instance_init = fixture_init,
    .class_init = fixture_class_init,
};

static void fixture_register_types(void)
{
    type_register_static(&fixture_type);
}

type_init(fixture_register_types)
