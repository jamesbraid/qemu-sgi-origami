/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Test-only latch seeding; this fixture does not detect or signal errors. */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/mips/sgi/crime.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "system/qtest.h"

#define TYPE_SGI_CRIME_TEST "sgi-crime-test-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(CrimeTestState, SGI_CRIME_TEST)

struct CrimeTestState {
    DeviceState parent_obj;
    SGICrimeState *crime;
    uint64_t line_sample;
};

static SGICrimeState *crime_test_target(Object *obj, Error **errp)
{
    CrimeTestState *s = SGI_CRIME_TEST(obj);

    if (!qtest_driver() || !qdev_is_realized(DEVICE(obj)) || !s->crime ||
        !qdev_is_realized(DEVICE(s->crime))) {
        error_setg(errp, "error latch access requires realized qtest devices");
        return NULL;
    }
    return s->crime;
}

static void crime_test_get(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    SGICrimeState *crime = crime_test_target(obj, errp);
    uint64_t value;

    if (!crime) {
        return;
    }
    if (!strcmp(name, "cpu-error-address")) {
        value = crime->cpu_error_address;
    } else if (!strcmp(name, "cpu-error-status")) {
        value = crime->cpu_error_status;
    } else if (!strcmp(name, "memory-error-address")) {
        value = crime->memory_error_address;
    } else {
        value = crime->memory_error_status;
    }
    visit_type_uint64(v, name, &value, errp);
}

static void crime_test_set(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    SGICrimeState *crime = crime_test_target(obj, errp);
    uint64_t value;

    if (!crime || !visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (!strcmp(name, "cpu-error-address")) {
        crime->cpu_error_address = value;
    } else if (!strcmp(name, "cpu-error-status")) {
        if (value & ~7ULL) {
            goto invalid;
        }
        crime->cpu_error_status = value;
    } else if (!strcmp(name, "memory-error-address")) {
        if (value > 0x3fffffff) {
            goto invalid;
        }
        crime->memory_error_address = value;
    } else {
        if (value & ~0x0e000000ULL) {
            goto invalid;
        }
        crime->memory_error_status = value;
    }
    return;
invalid:
    error_setg(errp, "%s exceeds the admitted latch fields", name);
}

static void crime_test_line_get(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    CrimeTestState *s = SGI_CRIME_TEST(obj);
    SGICrimeState *crime = crime_test_target(obj, errp);
    uint64_t value = s->line_sample;

    if (!crime) {
        return;
    }
    if (!strcmp(name, "line-area") &&
        !sgi_crime_line_coverage(crime, (int32_t)(s->line_sample >> 32),
                                (int32_t)s->line_sample, &value)) {
        error_setg(errp, "invalid native AA line numerical sample");
        return;
    }
    visit_type_uint64(v, name, &value, errp);
}

static void crime_test_line_set(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    CrimeTestState *s = SGI_CRIME_TEST(obj);
    uint64_t value;
    int32_t x, y;

    if (!crime_test_target(obj, errp) ||
        !visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    x = value >> 32;
    y = value;
    if (x <= -(1 << 18) || x >= (1 << 18) ||
        y <= -(1 << 18) || y >= (1 << 18)) {
        error_setg(errp, "native AA line sample exceeds the numerical domain");
        return;
    }
    s->line_sample = value;
}

static void crime_test_realize(DeviceState *dev, Error **errp)
{
    CrimeTestState *s = SGI_CRIME_TEST(dev);

    if (!qtest_driver()) {
        error_setg(errp, "CRIME test fixture requires a qtest server");
        return;
    }
    if (!s->crime || !qdev_is_realized(DEVICE(s->crime))) {
        error_setg(errp, "CRIME test fixture requires a realized interface");
    }
}

static const VMStateDescription crime_test_vmstate = {
    .name = TYPE_SGI_CRIME_TEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static const Property crime_test_properties[] = {
    DEFINE_PROP_LINK("crime", CrimeTestState, crime, TYPE_SGI_CRIME,
                     SGICrimeState *),
};

static void crime_test_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    const char *properties[] = {
        "cpu-error-address", "cpu-error-status", "memory-error-address",
        "memory-error-status",
    };

    dc->realize = crime_test_realize;
    dc->vmsd = &crime_test_vmstate;
    dc->desc = "Synthetic CRIME error latch fixture";
    device_class_set_props(dc, crime_test_properties);
    object_class_property_add(oc, "line-sample", "uint64", crime_test_line_get,
                               crime_test_line_set, NULL, NULL);
    object_class_property_add(oc, "line-area", "uint64", crime_test_line_get,
                               NULL, NULL, NULL);
    for (unsigned i = 0; i < ARRAY_SIZE(properties); i++) {
        object_class_property_add(oc, properties[i], "uint64", crime_test_get,
                                   crime_test_set, NULL, NULL);
    }
}

static const TypeInfo crime_test_type = {
    .name = TYPE_SGI_CRIME_TEST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(CrimeTestState),
    .class_init = crime_test_class_init,
};

static void crime_test_register_types(void)
{
    type_register_static(&crime_test_type);
}

type_init(crime_test_register_types)
