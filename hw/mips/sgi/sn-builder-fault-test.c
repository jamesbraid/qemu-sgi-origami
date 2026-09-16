/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A minimal, self-contained two-node topology this fixture mutates per
 * the "fault" property, to exercise sgi_machine_validate_topology's
 * named faults (docs/plans/sn-machine-model.md's one-builder step)
 * without needing the real Origin 200 descriptor. mc->init runs the
 * validator and reports its verdict only once a fault has been named;
 * qom-test and test-hmp instantiate every registered machine type
 * with no properties at all to probe generic QOM/HMP behavior, so an
 * unset "fault" is a silent no-op there, the same as the "none"
 * machine. No device is ever built, since the faults this fixture
 * names are all caught before the builder creates anything.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/mips/sgi/sn-machine.h"

#define TYPE_SN_BUILDER_FAULT_TEST MACHINE_TYPE_NAME("sn-builder-fault-test")

static const SGIModelProcessor fault_test_processor = {
    .id = "test-cpu",
    .kind = "TEST",
};

static SGIModelCpuSlot fault_test_n0_cpu_slots[] = {
    { .interface = 0, .slot = 0, .assembly = &fault_test_processor },
    { .interface = 0, .slot = 1, .assembly = &fault_test_processor },
};

static SGIModelCpuSlot fault_test_n1_cpu_slots[] = {
    { .interface = 0, .slot = 0, .assembly = &fault_test_processor },
    { .interface = 0, .slot = 1, .assembly = &fault_test_processor },
};

static SGIModelNode fault_test_nodes[] = {
    {
        .id = "n0",
        .module_index = 0,
        .slot = 0,
        .asic_kind = SGI_MODEL_ASIC_HUB,
        .cpu_slots = fault_test_n0_cpu_slots,
        .cpu_slot_count = ARRAY_SIZE(fault_test_n0_cpu_slots),
        .bank_mib = { 64, 0, 0, 0, 0, 0, 0, 0 },
    },
    {
        .id = "n1",
        .module_index = 1,
        .slot = 0,
        .asic_kind = SGI_MODEL_ASIC_HUB,
        .cpu_slots = fault_test_n1_cpu_slots,
        .cpu_slot_count = ARRAY_SIZE(fault_test_n1_cpu_slots),
        .bank_mib = { 64, 0, 0, 0, 0, 0, 0, 0 },
    },
};

static unsigned int fault_test_m0_nodes[] = { 0 };
static unsigned int fault_test_m1_nodes[] = { 1 };

static SGIModelModule fault_test_modules[] = {
    {
        .id = "m0",
        .controller_index = -1,
        .nodes = fault_test_m0_nodes,
        .node_count = ARRAY_SIZE(fault_test_m0_nodes),
    },
    {
        .id = "m1",
        .controller_index = -1,
        .nodes = fault_test_m1_nodes,
        .node_count = ARRAY_SIZE(fault_test_m1_nodes),
    },
};

static SGIModelFabricLink fault_test_links[] = {
    {
        .name = "link0",
        .ends = {
            { .kind = SGI_MODEL_FABRIC_NODE, .index = 0, .port = 0 },
            { .kind = SGI_MODEL_FABRIC_NODE, .index = 1, .port = 0 },
        },
        .kind = SGI_MODEL_LINK_CABLE,
    },
};

static SGIModelTopology fault_test_topology = {
    .name = "sn-builder-fault-test",
    .generation = SGI_MODEL_GENERATION_SN0,
    .description = "the builder validator's own test fixture",
    .nodes = fault_test_nodes,
    .node_count = ARRAY_SIZE(fault_test_nodes),
    .modules = fault_test_modules,
    .module_count = ARRAY_SIZE(fault_test_modules),
    .links = fault_test_links,
    .link_count = ARRAY_SIZE(fault_test_links),
    .console = 0,
    .junk_uart = -1,
};

/*
 * Named faults for the checks sgi_machine_validate_topology runs before
 * the builder creates anything. Two of the plan's named checks have no
 * fixture case here: a bank beyond geometry needs a bank index past the
 * HUB's eight architectural banks, which is also this fixture's array
 * bound, so it cannot be expressed without a wider array than any real
 * node uses; a port beyond port_count needs a router, and this part's
 * Origin 200 topologies carry none. Both stay covered by the validator's
 * code, just not by a fixture case, until a router-carrying topology
 * (the Origin 2000 module, later) can drive one.
 */
static bool fault_test_fault_requested;

static void fault_test_set_fault(Object *obj, const char *value,
                                 Error **errp)
{
    fault_test_fault_requested = true;
    if (!strcmp(value, "link-end-range")) {
        fault_test_links[0].ends[0].index = 99;
    } else if (!strcmp(value, "node-in-two-modules")) {
        fault_test_m1_nodes[0] = 0;
    } else if (!strcmp(value, "cpu-slot-range")) {
        fault_test_n0_cpu_slots[0].interface = 5;
    } else {
        error_setg(errp, "unknown sn-builder-fault-test fault '%s'", value);
    }
}

static void fault_test_machine_init(MachineState *machine)
{
    Error *err = NULL;

    if (!fault_test_fault_requested) {
        return;
    }
    if (sgi_machine_validate_topology(&fault_test_topology, &err)) {
        error_setg(&error_fatal,
                  "sn-builder-fault-test: the injected fault was not "
                  "caught");
        return;
    }
    error_report_err(err);
    exit(EXIT_FAILURE);
}

static void fault_test_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "sgi-sn-machine builder validator test fixture";
    mc->init = fault_test_machine_init;
    object_class_property_add_str(oc, "fault", NULL, fault_test_set_fault);
}

static const TypeInfo fault_test_type = {
    .name = TYPE_SN_BUILDER_FAULT_TEST,
    .parent = TYPE_MACHINE,
    .class_init = fault_test_class_init,
};

static void fault_test_register_types(void)
{
    type_register_static(&fault_test_type);
}

type_init(fault_test_register_types)
