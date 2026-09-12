/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the production constructor with reordered and malformed inputs. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/qtest.h"
#include "sn0.h"

#define TYPE_SN0_MODEL_TEST MACHINE_TYPE_NAME("origin2000-model-test")
OBJECT_DECLARE_SIMPLE_TYPE(SN0ModelTestState, SN0_MODEL_TEST)

struct SN0ModelTestState {
    SN0MachineState parent_obj;
    SN0ModelComposition composition;
    SN0ModelHardware hardware;
    SN0ModelPhysicalNode nodes[4];
    SN0ModelModuleIdentity identities[4];
    SN0ModelRouter routers[2];
    SN0ModelRoutedFabricLink links[5];
    SN0ModelModule module;
    SN0ModelModuleManagement management;
    unsigned int members[4];
    unsigned int adapters[4];
    SN0ModelXtalkAttachment attachments[4];
};

static void sn0_model_test_set_fault(Object *obj, const char *value,
                                     Error **errp)
{
    SN0ModelTestState *s = SN0_MODEL_TEST(obj);

    if (!strcmp(value, "composition")) {
        s->parent_obj.composition = NULL;
    } else if (!strcmp(value, "hardware")) {
        s->composition.hardware = NULL;
    } else if (!strcmp(value, "routers")) {
        s->composition.routers = NULL;
    } else if (!strcmp(value, "router-module")) {
        s->routers[0].module_index = 1;
    } else if (!strcmp(value, "router-slot")) {
        s->routers[0].slot = s->routers[1].slot;
    } else if (!strcmp(value, "router-identity")) {
        s->routers[0].part = 2;
    } else if (!strcmp(value, "links")) {
        s->composition.links = NULL;
    } else if (!strcmp(value, "link-count")) {
        s->composition.link_count = 4;
    } else if (!strcmp(value, "link-endpoint")) {
        s->links[0].ends[0].index = 99;
    } else if (!strcmp(value, "link-port")) {
        s->links[0].ends[0].port = 99;
    } else if (!strcmp(value, "link-kind")) {
        s->links[0].ends[0].kind = 99;
    } else if (!strcmp(value, "link-duplicate")) {
        s->links[1].ends[0] = s->links[0].ends[0];
    } else if (!strcmp(value, "link-name")) {
        s->links[1].name = s->links[0].name;
    } else if (!strcmp(value, "cpu-range")) {
        s->nodes[0].cpu_first_id = 8;
    } else if (!strcmp(value, "cpu-duplicate")) {
        s->nodes[0].cpu_first_id = s->nodes[1].cpu_first_id;
    } else if (!strcmp(value, "cpu-count")) {
        s->nodes[0].cpu_count = 1;
    } else if (!strcmp(value, "memory-total")) {
        s->nodes[0].memory_mib++;
    } else if (!strcmp(value, "memory-geometry")) {
        s->hardware.memory.architectural_banks = 0;
    } else if (!strcmp(value, "memory-bank")) {
        s->nodes[0].bank_mib[0] = UINT64_MAX;
    } else if (!strcmp(value, "address-map")) {
        s->nodes[0].map.node_stride = UINT64_MAX;
    } else if (!strcmp(value, "xtalk-reference")) {
        s->attachments[0].node_index = 4;
    } else if (!strcmp(value, "module-serial")) {
        s->module.serial[0] = '\0';
    } else if (!strcmp(value, "module-binding")) {
        s->nodes[0].module_identity = NULL;
    } else if (!strcmp(value, "module-cross-binding")) {
        s->nodes[0].module_index = 1;
    } else if (!strcmp(value, "module-rom-crc")) {
        s->identities[0].rom[7] ^= 1;
    } else if (!strcmp(value, "module-page-crc")) {
        s->identities[0].page[0][30] ^= 1;
    } else if (!strcmp(value, "module-rom-duplicate")) {
        memcpy(s->identities[0].rom, s->identities[1].rom,
               sizeof(s->identities[0].rom));
    } else {
        error_setg(errp, "unknown model-test fault '%s'", value);
    }
}

static void sn0_model_test_init(Object *obj)
{
    SN0ModelTestState *s = SN0_MODEL_TEST(obj);
    const SN0ModelComposition *c = s->parent_obj.composition;
    const unsigned int order[] = { 2, 0, 3, 1 };
    unsigned int inverse[4];

    g_assert(c->node_count == 4 && c->router_count == 2 && c->link_count == 5);
    g_assert(c->module_count == 1 && c->xtalk_attachment_count == 4);
    s->composition = *c;
    s->hardware = *c->hardware;
    s->composition.hardware = &s->hardware;
    s->composition.nodes = s->nodes;
    s->composition.routers = s->routers;
    s->composition.links = s->links;
    s->composition.modules = &s->module;
    s->composition.xtalk_attachments = s->attachments;
    for (unsigned int i = 0; i < 4; i++) {
        s->nodes[i] = c->nodes[order[i]];
        s->identities[i] = *s->nodes[i].module_identity;
        s->nodes[i].module_identity = &s->identities[i];
        inverse[order[i]] = i;
    }
    for (unsigned int i = 0; i < 2; i++) {
        s->routers[i] = c->routers[1 - i];
    }
    for (unsigned int i = 0; i < 5; i++) {
        s->links[i] = c->links[4 - i];
        for (unsigned int side = 0; side < 2; side++) {
            SN0ModelFabricVertex *v = &s->links[i].ends[side];

            v->index = v->kind == SN0_MODEL_FABRIC_HUB ?
                       inverse[v->index] : 1 - v->index;
        }
    }
    s->module = c->modules[0];
    s->management = *s->module.management;
    s->module.nodes = s->members;
    s->module.management = &s->management;
    s->management.adapter_nodes = s->adapters;
    for (unsigned int i = 0; i < 4; i++) {
        s->members[i] = inverse[c->modules[0].nodes[i]];
        s->adapters[i] = inverse[c->modules[0].management->adapter_nodes[i]];
        s->attachments[i] = c->xtalk_attachments[i];
        s->attachments[i].node_index = inverse[s->attachments[i].node_index];
    }
    s->parent_obj.composition = &s->composition;
}

static void sn0_model_test_machine_init(MachineState *machine)
{
    ObjectClass *parent = object_class_get_parent(
        object_get_class(OBJECT(machine)));

    if (!qtest_enabled()) {
        error_setg(&error_fatal, "model-test requires the qtest accelerator");
        return;
    }
    MACHINE_CLASS(parent)->init(machine);
}

static void sn0_model_test_class_init(ObjectClass *oc, const void *data)
{
    MACHINE_CLASS(oc)->desc = "Origin 2000 descriptor test fixture";
    MACHINE_CLASS(oc)->init = sn0_model_test_machine_init;
    object_class_property_add_str(oc, "fault", NULL, sn0_model_test_set_fault);
}

static const TypeInfo sn0_model_test_type = {
    .name = TYPE_SN0_MODEL_TEST,
    .parent = MACHINE_TYPE_NAME("origin2000"),
    .instance_size = sizeof(SN0ModelTestState),
    .instance_init = sn0_model_test_init,
    .class_init = sn0_model_test_class_init,
};

static void sn0_model_test_register_types(void)
{
    type_register_static(&sn0_model_test_type);
}

type_init(sn0_model_test_register_types)
