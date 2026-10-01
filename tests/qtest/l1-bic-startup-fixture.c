/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic operation receiver; these registers are not a controller bus. */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/sgi-l1.h"
#include "hw/misc/sgi-bic-board-interface.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/reset.h"

#define TYPE_L1_BIC_FIXTURE "sgi-l1-bic-startup-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(L1BICFixtureState, L1_BIC_FIXTURE)

struct L1BICFixtureState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    DeviceState *l1;
    DeviceState *bic[2];
    uint32_t population;
    uint64_t identity_base;
    uint32_t fail_mask;
    uint32_t count;
    uint64_t operations[4];
    bool selected;
    bool initial_set;
    bool registered;
};

static bool fixture_operation(Object *receiver, unsigned position,
                              uint8_t bus, uint8_t address,
                              uint8_t command, uint8_t value)
{
    L1BICFixtureState *s = L1_BIC_FIXTURE(receiver->parent);
    bool completed = !(s->fail_mask & (1u << (2 * position +
                                             (command == 0x12))));

    g_assert_cmpuint(s->count, <, G_N_ELEMENTS(s->operations));
    s->operations[s->count++] = ((uint64_t)position << 32) |
        ((uint64_t)bus << 24) | ((uint64_t)address << 16) |
        ((uint64_t)command << 8) | value;
    if (position >= s->population || receiver != OBJECT(s->bic[position]) ||
        bus != 0 || address != 0x8c + 2 * position ||
        (command != 0x13 && command != 0x12)) {
        return false;
    }
    /* Each fake endpoint owns a separate position-zero BIC. */
    return sgi_bic_board_controller_byte(SGI_BIC_BOARD_INTERFACE(receiver),
        0, command == 0x13, value, completed) == MEMTX_OK;
}

static uint64_t fixture_read(void *opaque, hwaddr offset, unsigned size)
{
    L1BICFixtureState *s = opaque;
    uint64_t value = 0;

    if (offset == 0) {
        return s->count;
    }
    if (offset == 8) {
        return s->fail_mask;
    }
    if (offset >= 16 && offset < 48) {
        return s->operations[(offset - 16) / 8];
    }
    if (offset == 88 || offset == 96) {
        unsigned position = (offset - 88) / 8;
        if (position >= s->population ||
            sgi_bic_board_read(SGI_BIC_BOARD_INTERFACE(s->bic[position]),
                0x35000000, 2, &value) != MEMTX_OK) {
            return UINT64_C(1) << 63;
        }
    }
    return value;
}

static void fixture_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    L1BICFixtureState *s = opaque;
    SGIL1State *l1 = SGI_L1(s->l1);

    switch (offset) {
    case 0:
        s->count = 0;
        break;
    case 8:
        s->fail_mask = value;
        break;
    case 64:
        l1->pending_rack = value;
        break;
    case 72:
        l1->pending_bay = value;
        break;
    case 80:
        s->count = 0;
        if (value == 0) {
            device_cold_reset(s->l1);
        } else if (value <= s->population) {
            device_cold_reset(s->bic[value - 1]);
        }
        break;
    case 104: {
        SGIL1BICEndpoint endpoints[2] = {
            { OBJECT(s->bic[0]), 1 },
        };
        Error *error = NULL;

        g_assert_false(sgi_l1_bind_bic_startup(l1,
            SGI_L1_BIC_STARTUP_EXPERIMENT, endpoints,
            fixture_operation, &error));
        g_assert_nonnull(error);
        error_free(error);
        for (unsigned invalid = 0; invalid < 4; invalid++) {
            DeviceState *probe = qdev_new(TYPE_SGI_L1);
            SGIL1BICEndpoint bad[2] = {
                { OBJECT(s->bic[0]), 1 },
            };

            error = NULL;
            qdev_prop_set_string(probe, "brick-type",
                                 invalid == 0 ? "ip45" : "pci-expansion");
            if (invalid == 1) {
                bad[0].receiver = NULL;
            } else if (invalid == 2) {
                bad[1] = bad[0];
            } else if (invalid == 3) {
                g_assert_true(sgi_l1_bind_bic_startup(SGI_L1(probe),
                    SGI_L1_BIC_STARTUP_EXPERIMENT, bad,
                    fixture_operation, &error));
                g_assert_null(error);
            }
            g_assert_false(sgi_l1_bind_bic_startup(SGI_L1(probe),
                SGI_L1_BIC_STARTUP_EXPERIMENT, bad, fixture_operation,
                &error));
            g_assert_nonnull(error);
            error_free(error);
            object_unref(OBJECT(probe));
        }
        break;
    }
    case 112:
        l1->bic_operation_active = value != 0;
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
    L1BICFixtureState *s = L1_BIC_FIXTURE(dev);
    SGIL1BICEndpoint endpoints[2] = {};

    if (s->population < 1 || s->population > 2) {
        error_setg(errp, "synthetic startup population must be one or two");
        return;
    }
    for (unsigned i = 0; i < s->population; i++) {
        g_autofree char *name = g_strdup_printf("bic%u", i);

        s->bic[i] = qdev_new(TYPE_SGI_BIC_BOARD_INTERFACE);
        object_property_add_child(OBJECT(s), name, OBJECT(s->bic[i]));
        qdev_prop_set_uint64(s->bic[i], "identity", 0xc000 + i);
        qdev_prop_set_uint16(s->bic[i], "initial-latch", 0xabcd);
        qdev_prop_set_bit(s->bic[i], "initial-latch-set", s->initial_set);
        if (!qdev_realize(s->bic[i], NULL, errp)) {
            return;
        }
        device_cold_reset(s->bic[i]);
        endpoints[i].receiver = OBJECT(s->bic[i]);
        endpoints[i].identity = s->identity_base + i;
    }
    s->l1 = qdev_new(TYPE_SGI_L1);
    object_property_add_child(OBJECT(s), "l1", OBJECT(s->l1));
    qdev_prop_set_string(s->l1, "brick-type", "pci-expansion");
    if (s->selected &&
        !sgi_l1_bind_bic_startup(SGI_L1(s->l1),
            SGI_L1_BIC_STARTUP_EXPERIMENT, endpoints, fixture_operation,
            errp)) {
        return;
    }
    if (!qdev_realize(s->l1, NULL, errp)) {
        return;
    }
    for (unsigned i = 0; i < s->population; i++) {
        qemu_register_resettable(OBJECT(s->bic[i]));
    }
    qemu_register_resettable(OBJECT(s->l1));
    s->registered = true;
}

static void fixture_finalize(Object *obj)
{
    L1BICFixtureState *s = L1_BIC_FIXTURE(obj);

    if (s->registered) {
        qemu_unregister_resettable(OBJECT(s->l1));
        for (unsigned i = 0; i < s->population; i++) {
            qemu_unregister_resettable(OBJECT(s->bic[i]));
        }
    }
    if (s->l1) {
        object_unref(OBJECT(s->l1));
    }
    for (unsigned i = 0; i < 2; i++) {
        if (s->bic[i]) {
            object_unref(OBJECT(s->bic[i]));
        }
    }
}

static void fixture_reset_enter(Object *obj, ResetType type)
{
    L1_BIC_FIXTURE(obj)->count = 0;
}

static const VMStateDescription vmstate_fixture = {
    .name = TYPE_L1_BIC_FIXTURE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(population, L1BICFixtureState),
        VMSTATE_UINT32(fail_mask, L1BICFixtureState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property fixture_properties[] = {
    DEFINE_PROP_UINT64("identity-base", L1BICFixtureState, identity_base, 1),
    DEFINE_PROP_UINT32("population", L1BICFixtureState, population, 1),
    DEFINE_PROP_UINT32("fail-mask", L1BICFixtureState, fail_mask, 0),
    DEFINE_PROP_BOOL("selected", L1BICFixtureState, selected, true),
    DEFINE_PROP_BOOL("initial-set", L1BICFixtureState, initial_set, true),
};

static void fixture_init(Object *obj)
{
    L1BICFixtureState *s = L1_BIC_FIXTURE(obj);

    memory_region_init_io(&s->mmio, obj, &fixture_ops, s,
                          TYPE_L1_BIC_FIXTURE, 128);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static void fixture_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = fixture_realize;
    dc->user_creatable = true;
    dc->vmsd = &vmstate_fixture;
    rc->phases.enter = fixture_reset_enter;
    device_class_set_props(dc, fixture_properties);
}

static const TypeInfo fixture_type = {
    .name = TYPE_L1_BIC_FIXTURE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(L1BICFixtureState),
    .instance_init = fixture_init,
    .instance_finalize = fixture_finalize,
    .class_init = fixture_class_init,
};

static void fixture_register_types(void)
{
    type_register_static(&fixture_type);
}

type_init(fixture_register_types)
