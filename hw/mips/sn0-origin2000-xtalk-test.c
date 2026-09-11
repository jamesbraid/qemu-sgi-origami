/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test machine for descriptor-owned Crosstalk preparation. */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "qemu/rcu.h"
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
        g_assert(!object_property_get_bool(crossbow, "stop-on-link-reset",
                                           &error_abort));
        g_assert(object_property_get_bool(
                     crossbow, "complete-posted-error-address-writes",
                     &error_abort));
        g_assert(object_property_get_uint(crossbow,
                                          "link-reset-restore-alive",
                                          &error_abort) ==
                 (i ? 0 : 0x0100));
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
    /* Make the board refuse to realize, to reach attach's unwind. */
    bool refuse_board;
    /* Make the second switch refuse to realize, for the unwind past it. */
    bool refuse_crossbow;
    /* Refuse the last PCI function, for a board that cannot be finished. */
    bool refuse_function;
    bool refused_again;
    /* Tear the assembly down after it attached, as a machine owner would. */
    bool teardown;
    bool released;
    /*
     * Where a processor finds widget zero of its own node's switch. A
     * processor addresses its own node unqualified; another node's takes
     * routing the firmware has not programmed at reset.
     */
    uint64_t window;
    /* Each node's own widget number, one nibble apiece, for the test. */
    uint64_t widgets;
    /* What the descriptor says about the one base I/O board. */
    uint64_t board_widget;
    uint64_t board_revision;
    uint64_t ioc3_subsystem;
};

static bool xtalk_attach_test_get_verified(Object *obj, Error **errp)
{
    return XTALK_ATTACH_TEST(obj)->verified;
}

static bool xtalk_attach_test_get_refuse(Object *obj, Error **errp)
{
    return XTALK_ATTACH_TEST(obj)->refuse_board;
}

static void xtalk_attach_test_set_refuse(Object *obj, bool value, Error **errp)
{
    XTALK_ATTACH_TEST(obj)->refuse_board = value;
}

static bool xtalk_attach_test_get_refuse_function(Object *obj, Error **errp)
{
    return XTALK_ATTACH_TEST(obj)->refuse_function;
}

static void xtalk_attach_test_set_refuse_function(Object *obj, bool value,
                                                  Error **errp)
{
    XTALK_ATTACH_TEST(obj)->refuse_function = value;
}

static bool xtalk_attach_test_get_refused_again(Object *obj, Error **errp)
{
    return XTALK_ATTACH_TEST(obj)->refused_again;
}

static bool xtalk_attach_test_get_refuse_crossbow(Object *obj, Error **errp)
{
    return XTALK_ATTACH_TEST(obj)->refuse_crossbow;
}

static void xtalk_attach_test_set_refuse_crossbow(Object *obj, bool value,
                                                  Error **errp)
{
    XTALK_ATTACH_TEST(obj)->refuse_crossbow = value;
}

static bool xtalk_attach_test_get_teardown(Object *obj, Error **errp)
{
    return XTALK_ATTACH_TEST(obj)->teardown;
}

static void xtalk_attach_test_set_teardown(Object *obj, bool value,
                                           Error **errp)
{
    XTALK_ATTACH_TEST(obj)->teardown = value;
}

static bool xtalk_attach_test_get_released(Object *obj, Error **errp)
{
    return XTALK_ATTACH_TEST(obj)->released;
}

/*
 * The base I/O board, checked where a guest cannot look: that each function
 * the descriptor names is real and sits at the address the record gives it,
 * that the Bridge sends its own transfers into this board's proxy rather
 * than a node's, and that the IOC3's interrupt goes to the Bridge pin the
 * record picks instead of taking the ordinary PCI route.
 */
static void xtalk_attach_test_verify_board(SN0XtalkAttachTest *test,
                                           Object *xtalk)
{
    const SN0ModelComposition *model = SN0_MACHINE(test)->composition;
    const SN0ModelIoBoard *record = &model->io_boards[0];
    Object *owner = object_resolve_path_component(xtalk, "baseio0");
    unsigned int scsi_index = 0;
    Object *bridge;
    Object *fabric;
    size_t i;

    g_assert(owner);
    bridge = object_resolve_path_component(owner, "bridge");
    g_assert(bridge);
    g_assert(DEVICE(bridge)->realized);

    fabric = object_property_get_link(bridge, "fabric", &error_abort);
    g_assert(fabric);
    {
        g_autofree char *owner_path = object_get_canonical_path(owner);
        g_autofree char *fabric_path = object_get_canonical_path(fabric);

        g_assert(g_str_has_prefix(fabric_path, owner_path));
    }
    g_assert(object_property_get_uint(bridge, "fabric-widget-shift",
                                      &error_abort) ==
             SGI_BRIDGE_PACKET_ADDRESS_BITS);
    g_assert(object_property_get_uint(bridge, "fabric-base",
                                      &error_abort) == 0);

    for (i = 0; i < record->pci_count; i++) {
        const SN0ModelComponent *function = &record->pci[i];
        g_autofree char *name =
            function->kind == SN0_COMPONENT_IOC3 ?
            g_strdup("ioc3") : g_strdup_printf("isp%u", scsi_index++);
        Object *child = object_resolve_path_component(owner, name);

        g_assert(child);
        g_assert(DEVICE(child)->realized);
        g_assert(PCI_DEVICE(child)->devfn ==
                 PCI_DEVFN(function->device, function->function));
        if (function->kind != SN0_COMPONENT_IOC3) {
            continue;
        }
        test->ioc3_subsystem = function->subsystem_id;
        g_assert(qdev_get_gpio_out_connector(DEVICE(child), NULL, 0) ==
                 qdev_get_gpio_in(DEVICE(bridge), function->irq_input));
    }

    test->board_widget = record->widget;
    test->board_revision = record->bridge->revision;
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
    xtalk_attach_test_verify_board(test, xtalk);
    test->verified = true;
}

/*
 * Terminal teardown, run where a machine owner would run it: after a
 * successful attach, with the Hubs realized. The drain is the point of
 * doing it here -- a bus child hands its reference back a grace period
 * later, on another thread, so storage freed too early takes the machine
 * down long after machine init returned and nowhere near the cause.
 */
static void xtalk_attach_test_release(SN0XtalkAttachTest *test)
{
    SN0MachineState *sn0 = SN0_MACHINE(test);
    MemoryRegion *outgoing = sn0->nodes[0]->hub.xtalk_io;
    g_autofree char *owner = NULL;

    g_assert(outgoing);
    sn0_origin2000_xtalk_destroy(sn0);
    drain_call_rcu();
    /*
     * Every Hub here is realized, so each still holds the big-window aliases
     * it built over the face it was linked to. Teardown cannot take those
     * back, so what they alias has to still belong to somebody: an owner with
     * no path to the machine is one that has been freed underneath them.
     */
    g_assert(sn0->nodes[0]->hub.xtalk_io == outgoing);
    owner = object_get_canonical_path(memory_region_owner(outgoing));
    g_assert(owner);
    g_assert(memory_region_size(outgoing) == SN0_XTALK_SIZE);
    test->released = true;
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

    /*
     * A failed attach has to come back as an error rather than take the
     * machine with it. The unwind runs with realized Hubs, which is the
     * case that cannot undo preparation, so it must undo only its own work
     * and leave a machine that can be attached again.
     *
     * Two failure points, because they leave different amounts behind. The
     * board is built after every Hub endpoint has been attached, so its
     * unwind has all of those to give back. The switch fails later still,
     * with one of the two already realized.
     */
    if (test->refuse_board) {
        sn0_origin2000_xtalk_set_test_board_part(sn0, "far too long a part");
        err = NULL;
        g_assert(!sn0_origin2000_xtalk_attach(sn0, &err));
        g_assert(err);
        error_free(err);
        sn0_origin2000_xtalk_set_test_board_part(sn0, NULL);
    }
    if (test->refuse_crossbow) {
        sn0_origin2000_xtalk_set_test_crossbow_failure(sn0, 1);
        err = NULL;
        g_assert(!sn0_origin2000_xtalk_attach(sn0, &err));
        g_assert(err);
        error_free(err);
        sn0_origin2000_xtalk_set_test_crossbow_failure(sn0, -1);
    }
    /*
     * A board that cannot be finished. The last function the descriptor
     * names is the one that takes the ethernet configuration; the hook
     * refuses it a slot, which is a refusal from the PCI core rather than
     * from inside the chip, and leaves the assembly holding exactly what a
     * refusal from inside would leave. It cannot tell the two apart, so it
     * treats the board as spent either way -- and the machine stays failed
     * rather than coming up with a second board and no ethernet address.
     */
    if (test->refuse_function) {
        int last = (int)SN0_MACHINE(machine)->composition
                       ->io_boards[0].pci_count - 1;

        sn0_origin2000_xtalk_set_test_function_failure(sn0, last);
        err = NULL;
        g_assert(!sn0_origin2000_xtalk_attach(sn0, &err));
        g_assert(err);
        error_free(err);
        sn0_origin2000_xtalk_set_test_function_failure(sn0, -1);

        err = NULL;
        g_assert(!sn0_origin2000_xtalk_attach(sn0, &err));
        g_assert(err);
        g_assert(strstr(error_get_pretty(err), "cannot be attached again"));
        error_free(err);
        test->refused_again = true;
        return;
    }

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
    object_property_add_uint64_ptr(OBJECT(test), "board-widget",
                                   &test->board_widget, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(test), "board-revision",
                                   &test->board_revision, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(test), "ioc3-subsystem",
                                   &test->ioc3_subsystem, OBJ_PROP_FLAG_READ);
    if (test->teardown) {
        xtalk_attach_test_release(test);
    }
}

static void xtalk_attach_test_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Origin 2000 Crosstalk attach test fixture";
    mc->init = xtalk_attach_test_init;
    object_class_property_add_bool(oc, "verified",
                                   xtalk_attach_test_get_verified, NULL);
    object_class_property_add_bool(oc, "refuse-board",
                                   xtalk_attach_test_get_refuse,
                                   xtalk_attach_test_set_refuse);
    object_class_property_add_bool(oc, "refuse-crossbow",
                                   xtalk_attach_test_get_refuse_crossbow,
                                   xtalk_attach_test_set_refuse_crossbow);
    object_class_property_add_bool(oc, "refuse-function",
                                   xtalk_attach_test_get_refuse_function,
                                   xtalk_attach_test_set_refuse_function);
    object_class_property_add_bool(oc, "refused-again",
                                   xtalk_attach_test_get_refused_again, NULL);
    object_class_property_add_bool(oc, "teardown",
                                   xtalk_attach_test_get_teardown,
                                   xtalk_attach_test_set_teardown);
    object_class_property_add_bool(oc, "released",
                                   xtalk_attach_test_get_released, NULL);
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
