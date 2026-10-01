/* SPDX-License-Identifier: BSD-3-Clause */
/* Test-only operation submission; these registers are not a controller bus. */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/sgi-bic-board-interface.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define TYPE_BIC_FIXTURE "sgi-bic-board-interface-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(BICFixtureState, BIC_FIXTURE)

struct BICFixtureState {
    SysBusDevice parent_obj;
    DeviceState *bic;
    MemoryRegion mmio;
    uint64_t identity;
    uint16_t initial_latch;
    bool initial_latch_set;
    uint64_t offset;
    uint64_t size;
    uint64_t value;
    MemTxResult result;
    MemTxResult controller_result;
};

static uint64_t fixture_read(void *opaque, hwaddr offset, unsigned size)
{
    BICFixtureState *s = opaque;

    switch (offset) {
    case 16:
        return s->value;
    case 32:
        return s->result;
    case 48:
        return s->controller_result;
    default:
        return 0;
    }
}

static void fixture_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    BICFixtureState *s = opaque;
    SGIBICBoardInterfaceState *bic = SGI_BIC_BOARD_INTERFACE(s->bic);

    switch (offset) {
    case 0:
        s->offset = value;
        break;
    case 8:
        s->size = value;
        break;
    case 16:
        s->value = value;
        break;
    case 24:
        if (value) {
            s->result = sgi_bic_board_write(bic, s->offset, s->size, s->value);
        } else {
            s->result = sgi_bic_board_read(bic, s->offset, s->size, &s->value);
        }
        break;
    case 56:
        s->controller_result = sgi_bic_board_controller_operation(
            OBJECT(bic), value >> 32, value >> 24, value >> 16,
            value >> 8, value) ? MEMTX_OK : MEMTX_ERROR;
        break;
    case 40:
        s->controller_result = sgi_bic_board_controller_byte(bic, value >> 16,
                                                   value & 0x100, value,
                                                   value & 0x200);
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
    BICFixtureState *s = BIC_FIXTURE(dev);

    s->bic = qdev_new(TYPE_SGI_BIC_BOARD_INTERFACE);
    object_property_add_child(OBJECT(s), "interface", OBJECT(s->bic));
    qdev_prop_set_uint64(s->bic, "identity", s->identity);
    qdev_prop_set_uint16(s->bic, "initial-latch", s->initial_latch);
    qdev_prop_set_bit(s->bic, "initial-latch-set", s->initial_latch_set);
    if (!qdev_realize(s->bic, NULL, errp)) {
        return;
    }
    object_unref(OBJECT(s->bic));
}

static void fixture_reset_hold(Object *obj, ResetType type)
{
    BICFixtureState *s = BIC_FIXTURE(obj);

    /* The internal interface has no bus to propagate its reset. */
    resettable_reset(OBJECT(s->bic), type);
    s->offset = 0;
    s->size = 0;
    s->value = 0;
    s->result = MEMTX_OK;
    s->controller_result = MEMTX_OK;
}

static void fixture_init(Object *obj)
{
    BICFixtureState *s = BIC_FIXTURE(obj);

    memory_region_init_io(&s->mmio, obj, &fixture_ops, s,
                          TYPE_BIC_FIXTURE, 64);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static const Property fixture_properties[] = {
    DEFINE_PROP_UINT64("identity", BICFixtureState, identity, 0),
    DEFINE_PROP_UINT16("initial-latch", BICFixtureState, initial_latch, 0),
    DEFINE_PROP_BOOL("initial-latch-set", BICFixtureState,
                     initial_latch_set, false),
};

static void fixture_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "Synthetic BIC board interface fixture";
    dc->realize = fixture_realize;
    dc->user_creatable = true;
    rc->phases.hold = fixture_reset_hold;
    device_class_set_props(dc, fixture_properties);
}

static const TypeInfo fixture_type = {
    .name = TYPE_BIC_FIXTURE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BICFixtureState),
    .instance_init = fixture_init,
    .class_init = fixture_class_init,
};

static void fixture_register_types(void)
{
    type_register_static(&fixture_type);
}

type_init(fixture_register_types)
