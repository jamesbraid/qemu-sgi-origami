/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test machine for descriptor-owned Crosstalk preparation. */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "sn0.h"
#include "sn0-origin2000-xtalk.h"

#define TYPE_XTALK_OWNER_TEST MACHINE_TYPE_NAME("origin2000-xtalk-owner-test")
OBJECT_DECLARE_SIMPLE_TYPE(SN0XtalkOwnerTest, XTALK_OWNER_TEST)

struct SN0XtalkOwnerTest {
    SN0MachineState parent_obj;
    bool cycle;
    bool verified;
};

static bool xtalk_owner_test_get_cycle(Object *obj, Error **errp)
{
    return XTALK_OWNER_TEST(obj)->cycle;
}

static void xtalk_owner_test_set_cycle(Object *obj, bool value, Error **errp)
{
    XTALK_OWNER_TEST(obj)->cycle = value;
}

static bool xtalk_owner_test_get_verified(Object *obj, Error **errp)
{
    return XTALK_OWNER_TEST(obj)->verified;
}

static void xtalk_owner_test_verify(SN0XtalkOwnerTest *test)
{
    SN0MachineState *sn0 = SN0_MACHINE(test);
    Object *xtalk = object_resolve_path_component(OBJECT(test), "xtalk");
    MemoryRegion *outgoing[4];
    unsigned int i, j;

    g_assert(xtalk);
    for (i = 0; i < 2; i++) {
        g_autofree char *name = g_strdup_printf("crossbow%u", i);
        Object *crossbow = object_resolve_path_component(xtalk, name);

        g_assert(crossbow);
        g_assert(object_property_get_uint(crossbow, "span", &error_abort) ==
                 SN0_XTALK_WIDGET_SIZE);
        g_assert(object_property_get_uint(crossbow, "links-present",
                                          &error_abort) ==
                 (i ? 0x600 : 0x700));
        g_assert(object_property_get_uint(crossbow, "startup-links-alive",
                                          &error_abort) ==
                 (i ? 0x600 : 0x700));
        g_assert(object_property_get_bool(crossbow, "stop-on-link-reset",
                                          &error_abort));
    }
    g_assert(object_resolve_path_component(xtalk, "baseio0"));
    g_assert(sn0_origin2000_xtalk_test_proxies_disabled(sn0));
    for (i = 0; i < ARRAY_SIZE(outgoing); i++) {
        outgoing[i] = sn0->nodes[i]->hub.xtalk_io;
        g_assert(outgoing[i]);
        g_assert(memory_region_size(outgoing[i]) == SN0_XTALK_SIZE);
        for (j = 0; j < i; j++) {
            g_assert(outgoing[i] != outgoing[j]);
        }
    }
    test->verified = true;
}

static void xtalk_owner_test_init(MachineState *machine)
{
    SN0MachineState *sn0 = SN0_MACHINE(machine);
    SN0XtalkOwnerTest *test = XTALK_OWNER_TEST(machine);
    Error *err = NULL;

    sn0_origin2000_prepare_nodes(machine);
    if (!sn0_origin2000_xtalk_prepare(sn0, &error_fatal)) {
        return;
    }
    xtalk_owner_test_verify(test);
    g_assert(!sn0_origin2000_xtalk_prepare(sn0, &err));
    g_assert(err);
    g_assert(strstr(error_get_pretty(err), "already prepared"));
    error_free(err);
    if (test->cycle) {
        sn0_origin2000_xtalk_destroy(sn0);
        if (!sn0_origin2000_xtalk_prepare(sn0, &error_fatal)) {
            return;
        }
        xtalk_owner_test_verify(test);
    }
    sn0_origin2000_xtalk_destroy(sn0);
    sn0_origin2000_realize_nodes(machine);
}

static void xtalk_owner_test_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Origin 2000 Crosstalk owner test fixture";
    mc->init = xtalk_owner_test_init;
    object_class_property_add_bool(oc, "cycle",
                                   xtalk_owner_test_get_cycle,
                                   xtalk_owner_test_set_cycle);
    object_class_property_add_bool(oc, "verified",
                                   xtalk_owner_test_get_verified, NULL);
}

static const TypeInfo xtalk_owner_test_type = {
    .name = TYPE_XTALK_OWNER_TEST,
    .parent = MACHINE_TYPE_NAME("origin2000"),
    .instance_size = sizeof(SN0XtalkOwnerTest),
    .class_init = xtalk_owner_test_class_init,
};

/*
 * The same fixture carried one step further: nodes realized, endpoints
 * attached and Crossbows realized, which is the first point at which a
 * processor can reach a switch register through the machine's own windows.
 */
#define TYPE_XTALK_ATTACH_TEST \
    MACHINE_TYPE_NAME("origin2000-xtalk-attach-test")
OBJECT_DECLARE_SIMPLE_TYPE(SN0XtalkAttachTest, XTALK_ATTACH_TEST)

struct SN0XtalkAttachTest {
    SN0MachineState parent_obj;
    bool verified;
    /*
     * Where a processor finds widget zero of its own node's switch. A
     * processor addresses its own node unqualified; another node's takes
     * routing the firmware has not programmed at reset.
     */
    uint64_t window;
    /* Each node's own widget number, one nibble apiece, for the test. */
    uint64_t widgets;
};

static bool xtalk_attach_test_get_verified(Object *obj, Error **errp)
{
    return XTALK_ATTACH_TEST(obj)->verified;
}

static void xtalk_attach_test_verify(SN0XtalkAttachTest *test)
{
    SN0MachineState *sn0 = SN0_MACHINE(test);
    Object *xtalk = object_resolve_path_component(OBJECT(test), "xtalk");
    const SN0ModelComposition *model = sn0->composition;
    const SN0ModelAddressMap *map;
    unsigned int i;

    g_assert(xtalk);
    for (i = 0; i < 2; i++) {
        g_autofree char *name = g_strdup_printf("crossbow%u", i);
        Object *crossbow = object_resolve_path_component(xtalk, name);

        g_assert(crossbow);
        g_assert(DEVICE(crossbow)->realized);
    }
    /*
     * Every node sends into its own switch view, and the view covers the
     * whole crosstalk container rather than one widget's slice of it.
     */
    for (i = 0; i < 4; i++) {
        MemoryRegion *container = &sn0->nodes[i]->xtalk;
        MemoryRegion *ingress = NULL;
        MemoryRegion *child;

        QTAILQ_FOREACH(child, &container->subregions, subregions_link) {
            if (child->addr == 0 &&
                memory_region_size(child) == SN0_XTALK_SIZE) {
                ingress = child;
                break;
            }
        }
        g_assert(ingress);
        g_assert(sn0->nodes[i]->hub.xtalk_io);

        map = &sn0->nodes[i]->physical->map;
        test->window = sn0_model_node_address(map, &map->xtalk, 0, 0);
    }
    for (i = 0; i < model->xtalk_attachment_count; i++) {
        const SN0ModelXtalkAttachment *attachment =
            &model->xtalk_attachments[i];

        test->widgets |= (uint64_t)attachment->widget <<
                         (4 * attachment->node_index);

    }
    test->verified = true;
}

static void xtalk_attach_test_init(MachineState *machine)
{
    SN0MachineState *sn0 = SN0_MACHINE(machine);
    SN0XtalkAttachTest *test = XTALK_ATTACH_TEST(machine);
    Error *err = NULL;

    sn0_origin2000_prepare_nodes(machine);
    if (!sn0_origin2000_xtalk_prepare(sn0, &error_fatal)) {
        return;
    }
    /* Attaching needs realized Hubs, and says so rather than crashing. */
    g_assert(!sn0_origin2000_xtalk_attach(sn0, &err));
    g_assert(err);
    g_assert(strstr(error_get_pretty(err), "not realized"));
    error_free(err);

    sn0_origin2000_realize_nodes(machine);
    if (!sn0_origin2000_xtalk_attach(sn0, &error_fatal)) {
        return;
    }
    err = NULL;
    g_assert(!sn0_origin2000_xtalk_attach(sn0, &err));
    g_assert(err);
    g_assert(strstr(error_get_pretty(err), "already attached"));
    error_free(err);
    xtalk_attach_test_verify(test);
    object_property_add_uint64_ptr(OBJECT(test), "xtalk-window",
                                   &test->window, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(test), "node-widgets",
                                   &test->widgets, OBJ_PROP_FLAG_READ);
}

static void xtalk_attach_test_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Origin 2000 Crosstalk attach test fixture";
    mc->init = xtalk_attach_test_init;
    object_class_property_add_bool(oc, "verified",
                                   xtalk_attach_test_get_verified, NULL);
}

static const TypeInfo xtalk_attach_test_type = {
    .name = TYPE_XTALK_ATTACH_TEST,
    .parent = MACHINE_TYPE_NAME("origin2000"),
    .instance_size = sizeof(SN0XtalkAttachTest),
    .class_init = xtalk_attach_test_class_init,
};

static void xtalk_owner_test_register_types(void)
{
    type_register_static(&xtalk_owner_test_type);
    type_register_static(&xtalk_attach_test_type);
}
type_init(xtalk_owner_test_register_types)
