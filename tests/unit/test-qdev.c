#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "qapi/visitor.h"


#define TYPE_MY_DEV "my-dev"
typedef struct MyDev MyDev;
DECLARE_INSTANCE_CHECKER(MyDev, STATIC_TYPE,
                         TYPE_MY_DEV)

struct MyDev {
    DeviceState parent_obj;

    uint32_t prop_u32;
    char *prop_string;
    uint32_t *prop_array_u32;
    uint32_t prop_array_u32_nb;
};

static const Property my_dev_props[] = {
    DEFINE_PROP_UINT32("u32", MyDev, prop_u32, 100),
    DEFINE_PROP_STRING("string", MyDev, prop_string),
    DEFINE_PROP_ARRAY("array-u32", MyDev, prop_array_u32_nb, prop_array_u32,
                     qdev_prop_uint32, uint32_t),
};

static void my_dev_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = NULL;
    device_class_set_props(dc, my_dev_props);
}

static const TypeInfo my_dev_type_info = {
    .name = TYPE_MY_DEV,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(MyDev),
    .class_init = my_dev_class_init,
};

/*
 * Initialize a fake machine, being prepared for future tests.
 *
 * Realization of anonymous qdev (with no parent object) requires both
 * the machine object and its "unattached" container to be at least present.
 */
static void test_init_machine(void)
{
    /* This is a fake machine - it doesn't need to be a machine object */
    Object *machine = object_property_add_new_container(
        object_get_root(), "machine");

    /* This container must exist for anonymous qdevs to realize() */
    object_property_add_new_container(machine, "unattached");
}

static void test_qdev_free_properties(void)
{
    MyDev *mt;

    mt = STATIC_TYPE(object_new(TYPE_MY_DEV));
    object_set_props(OBJECT(mt), &error_fatal,
                     "string", "something",
                     "array-u32", "12,13",
                     NULL);
    qdev_realize(DEVICE(mt), NULL, &error_fatal);

    g_assert_cmpuint(mt->prop_u32, ==, 100);
    g_assert_cmpstr(mt->prop_string, ==, "something");
    g_assert_cmpuint(mt->prop_array_u32_nb, ==, 2);
    g_assert_cmpuint(mt->prop_array_u32[0], ==, 12);
    g_assert_cmpuint(mt->prop_array_u32[1], ==, 13);

    object_unparent(OBJECT(mt));
    object_unref(mt);
}

static void test_qdev_double_realization(void)
{
    MyDev *mt = STATIC_TYPE(object_new(TYPE_MY_DEV));

    qdev_realize(DEVICE(mt), NULL, &error_fatal);
    qdev_realize(DEVICE(mt), NULL, &error_fatal);
    object_unparent(OBJECT(mt));
    object_unref(OBJECT(mt));
}

#define TYPE_PRE_UNREALIZE_DEV "pre-unrealize-test-device"
#define TYPE_PRE_UNREALIZE_CHILD "pre-unrealize-test-child"
#define TYPE_PRE_UNREALIZE_BUS "pre-unrealize-test-bus"

typedef struct PreUnrealizeDev {
    DeviceState parent_obj;
    BusState *bus;
    DeviceState *child;
    unsigned preparations;
    unsigned child_teardowns;
    unsigned teardowns;
    unsigned phase;
    bool reject;
    bool reenter;
} PreUnrealizeDev;

static void pre_unrealize_realize(DeviceState *dev, Error **errp)
{
    PreUnrealizeDev *s = (PreUnrealizeDev *)dev;

    s->bus = qbus_new(TYPE_PRE_UNREALIZE_BUS, dev, "children");
    s->child = qdev_new(TYPE_PRE_UNREALIZE_CHILD);
    qdev_realize(s->child, s->bus, &error_abort);
}

static void assert_realize_transition_refused(DeviceState *dev, bool value)
{
    Error *err = NULL;

    g_assert_false(object_property_set_bool(OBJECT(dev), "realized", value,
                                            &err));
    g_assert_nonnull(err);
    error_free(err);
}

static void prepare_unrealize(DeviceState *dev, Error **errp)
{
    PreUnrealizeDev *s = (PreUnrealizeDev *)dev;

    g_assert_true(dev->realized);
    g_assert_true(s->bus->realized);
    g_assert_true(s->child->realized);
    s->preparations++;
    if (s->reject) {
        error_setg(errp, "teardown refused");
        return;
    }
    if (s->reenter) {
        assert_realize_transition_refused(dev, false);
        assert_realize_transition_refused(dev, true);
    }
    s->phase = 1;
}

static void pre_unrealize_child_unrealize(DeviceState *dev)
{
    PreUnrealizeDev *s = (PreUnrealizeDev *)dev->parent_bus->parent;

    g_assert_cmpuint(s->phase, ==, 1);
    g_assert_false(DEVICE(s)->realized);
    assert_realize_transition_refused(DEVICE(s), false);
    assert_realize_transition_refused(DEVICE(s), true);
    s->child_teardowns++;
    s->phase = 2;
}

static void pre_unrealize_unrealize(DeviceState *dev)
{
    PreUnrealizeDev *s = (PreUnrealizeDev *)dev;

    g_assert_cmpuint(s->phase, ==, 2);
    g_assert_false(s->bus->realized);
    g_assert_false(s->child->realized);
    s->teardowns++;
    s->phase = 3;
}

static void pre_unrealize_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = pre_unrealize_realize;
    dc->pre_unrealize = prepare_unrealize;
    dc->unrealize = pre_unrealize_unrealize;
}

static void pre_unrealize_child_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->bus_type = TYPE_PRE_UNREALIZE_BUS;
    dc->unrealize = pre_unrealize_child_unrealize;
}

static void test_qdev_pre_unrealize(void)
{
    PreUnrealizeDev *s = (PreUnrealizeDev *)qdev_new(TYPE_PRE_UNREALIZE_DEV);
    Error *err = NULL;

    qdev_realize(DEVICE(s), NULL, &error_abort);
    s->reject = true;
    g_assert_false(object_property_set_bool(OBJECT(s), "realized", false,
                                            &err));
    g_assert_nonnull(err);
    error_free(err);
    g_assert_true(DEVICE(s)->realized);
    g_assert_true(s->bus->realized);
    g_assert_true(s->child->realized);
    g_assert_cmpuint(s->preparations, ==, 1);
    g_assert_cmpuint(s->child_teardowns, ==, 0);
    g_assert_cmpuint(s->teardowns, ==, 0);
    s->reject = false;
    s->reenter = true;
    object_property_set_bool(OBJECT(s), "realized", false, &error_abort);
    g_assert_cmpuint(s->phase, ==, 3);
    g_assert_cmpuint(s->preparations, ==, 2);
    g_assert_cmpuint(s->child_teardowns, ==, 1);
    g_assert_cmpuint(s->teardowns, ==, 1);
    object_property_set_bool(OBJECT(s), "realized", false, &error_abort);
    g_assert_cmpuint(s->preparations, ==, 2);
    g_assert_cmpuint(s->teardowns, ==, 1);
    object_unparent(OBJECT(s->child));
    object_unref(OBJECT(s->child));
    object_unparent(OBJECT(s));
    object_unref(OBJECT(s));
}


int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    module_call_init(MODULE_INIT_QOM);
    type_register_static(&my_dev_type_info);
    const TypeInfo pre_unrealize_types[] = {
        {
            .name = TYPE_PRE_UNREALIZE_BUS,
            .parent = TYPE_BUS,
        }, {
            .name = TYPE_PRE_UNREALIZE_DEV,
            .parent = TYPE_DEVICE,
            .instance_size = sizeof(PreUnrealizeDev),
            .class_init = pre_unrealize_class_init,
        }, {
            .name = TYPE_PRE_UNREALIZE_CHILD,
            .parent = TYPE_DEVICE,
            .class_init = pre_unrealize_child_class_init,
        },
    };
    type_register_static_array(pre_unrealize_types,
                               G_N_ELEMENTS(pre_unrealize_types));
    test_init_machine();

    g_test_add_func("/qdev/free-properties",
                    test_qdev_free_properties);

    g_test_add_func("/qdev/double-realization",
                    test_qdev_double_realization);

    g_test_add_func("/qdev/pre-unrealize", test_qdev_pre_unrealize);
    g_test_run();

    return 0;
}
