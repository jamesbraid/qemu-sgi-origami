/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Two machine types that run the real builder (sgi_machine_init) over a
 * topology no shipped product carries, each turned on only by its own
 * "activate" property the way any qtest-only fixture here is: qom-test
 * and test-hmp instantiate every registered machine type with no
 * properties at all to probe generic QOM/HMP behavior, so leaving
 * "activate" off makes mc->init a no-op there, the same as the "none"
 * machine. SGIMachineClass.test_topology hands each fixture's topology
 * straight to sgi_machine_init, bypassing the topology= name lookup,
 * since the compiled catalogue never carries a fixture's own descriptor.
 *
 * This file used to also carry sn-builder-fault-test, a fixture that
 * called sgi_machine_validate_topology() directly over a minimal
 * two-node topology to exercise its named faults. That validator (and
 * the four it dispatched to) is gone: hw/mips/sgi/sn-machine.c now
 * trusts the compiled descriptor, and tests/unit/test-sn-model.c checks
 * its structural invariants once instead of at every launch. See
 * docs/plans/sn-machine-model.md, "The builder trusts the descriptor".
 *
 * Every fixture topology below is built once, at type-registration time,
 * from plain arrays into the QAPI-generated list types qapi/sgi-sn.json
 * declares (hand-building a SGIModelTopology in C has no JSON to visit,
 * so nothing here goes through qobject_input_visitor -- the list nodes
 * just point at the arrays' own elements, the same sharing the old
 * array-and-count descriptor gave this fixture's node-1-attachment
 * mutation below).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/mips/sgi/sn-machine.h"

static SGIModelCpuSlotList *cpu_slot_list(SGIModelCpuSlot *items, size_t count)
{
    SGIModelCpuSlotList *head = NULL;
    SGIModelCpuSlotList **tail = &head;
    size_t i;

    for (i = 0; i < count; i++) {
        SGIModelCpuSlotList *entry = g_new0(SGIModelCpuSlotList, 1);

        entry->value = &items[i];
        *tail = entry;
        tail = &entry->next;
    }
    return head;
}

static SGIModelModuleList *module_list(SGIModelModule *items, size_t count)
{
    SGIModelModuleList *head = NULL;
    SGIModelModuleList **tail = &head;
    size_t i;

    for (i = 0; i < count; i++) {
        SGIModelModuleList *entry = g_new0(SGIModelModuleList, 1);

        entry->value = &items[i];
        *tail = entry;
        tail = &entry->next;
    }
    return head;
}

static SGIModelXtalkAttachmentList *xtalk_attachment_list(
    SGIModelXtalkAttachment *items, size_t count)
{
    SGIModelXtalkAttachmentList *head = NULL;
    SGIModelXtalkAttachmentList **tail = &head;
    size_t i;

    for (i = 0; i < count; i++) {
        SGIModelXtalkAttachmentList *entry =
            g_new0(SGIModelXtalkAttachmentList, 1);

        entry->value = &items[i];
        *tail = entry;
        tail = &entry->next;
    }
    return head;
}

static SGIModelIoBoardList *io_board_list(SGIModelIoBoard *items, size_t count)
{
    SGIModelIoBoardList *head = NULL;
    SGIModelIoBoardList **tail = &head;
    size_t i;

    for (i = 0; i < count; i++) {
        SGIModelIoBoardList *entry = g_new0(SGIModelIoBoardList, 1);

        entry->value = &items[i];
        *tail = entry;
        tail = &entry->next;
    }
    return head;
}

static SGIModelCpuPopulationList *cpu_population_list(
    SGIModelCpuPopulation *items, size_t count)
{
    SGIModelCpuPopulationList *head = NULL;
    SGIModelCpuPopulationList **tail = &head;
    size_t i;

    for (i = 0; i < count; i++) {
        SGIModelCpuPopulationList *entry =
            g_new0(SGIModelCpuPopulationList, 1);

        entry->value = &items[i];
        *tail = entry;
        tail = &entry->next;
    }
    return head;
}

static SGIModelNodeList *node_list(SGIModelNode *items, size_t count)
{
    SGIModelNodeList *head = NULL;
    SGIModelNodeList **tail = &head;
    size_t i;

    for (i = 0; i < count; i++) {
        SGIModelNodeList *entry = g_new0(SGIModelNodeList, 1);

        entry->value = &items[i];
        *tail = entry;
        tail = &entry->next;
    }
    return head;
}

static SGIModelCableList *cable_list(SGIModelCable *items, size_t count)
{
    SGIModelCableList *head = NULL;
    SGIModelCableList **tail = &head;
    size_t i;

    for (i = 0; i < count; i++) {
        SGIModelCableList *entry = g_new0(SGIModelCableList, 1);

        entry->value = &items[i];
        *tail = entry;
        tail = &entry->next;
    }
    return head;
}

static SGIModelLinkEndList *link_end_list(SGIModelLinkEnd *items, size_t count)
{
    SGIModelLinkEndList *head = NULL;
    SGIModelLinkEndList **tail = &head;
    size_t i;

    for (i = 0; i < count; i++) {
        SGIModelLinkEndList *entry = g_new0(SGIModelLinkEndList, 1);

        entry->value = &items[i];
        *tail = entry;
        tail = &entry->next;
    }
    return head;
}

static uint32List *uint32_list(const unsigned int *items, size_t count)
{
    uint32List *head = NULL;
    uint32List **tail = &head;
    size_t i;

    for (i = 0; i < count; i++) {
        uint32List *entry = g_new0(uint32List, 1);

        entry->value = items[i];
        *tail = entry;
        tail = &entry->next;
    }
    return head;
}

static uint64List *uint64_list(const uint64_t *items, size_t count)
{
    uint64List *head = NULL;
    uint64List **tail = &head;
    size_t i;

    for (i = 0; i < count; i++) {
        uint64List *entry = g_new0(uint64List, 1);

        entry->value = items[i];
        *tail = entry;
        tail = &entry->next;
    }
    return head;
}

/*
 * Console field test fixture: a two-node topology with console=1, each
 * node carrying its own base I/O controller the way Origin 200's two-tower
 * wiring does (node/module/xtalk/board values copied from the
 * origin200-dual configuration's own compiled catalogue entry, minus a
 * SCSI-carrying board, which the serial line contract never touches). All
 * shipping topologies have console=0, so the reordering
 * sgi_machine_compute_serial_line() does for a non-default console is
 * otherwise never built or realized, only validated.
 * SGIMachineClass.test_topology hands this straight to sgi_machine_init,
 * so the qtest in tests/qtest/sn-builder-test.c that boots
 * "sn-builder-console-test" exercises the real builder, not a stand-in.
 */
static SGIModelProcessor console_test_r10000_180mhz = {
    .id = (char *)"r10000-180mhz",
    .kind = (char *)"R10000",
    .prid = 0x926,
    .fpu_prid = 0x900,
    .clock_hz = 0xaba9500,
    .cache_clock_hz = 0x55d4a80,
    .cache_mib = 1,
    .mode_bits = 0x1211a785,
};

static SGIModelCpuSlot console_test_cpu_slots[] = {
    { .interface = 0, .slot = 0, .assembly = &console_test_r10000_180mhz },
    { .interface = 0, .slot = 1, .assembly = &console_test_r10000_180mhz },
};

static uint64_t console_test_accepted_mib[] = { 512, 256, 128, 64 };

static SGIModelAsicIdentity console_test_identity0 = {
    .ni_revision = 3,
    .widget_revision = 1,
    .serial = 0xc0b0a090807,
    .i2c_controller = true,
    .system_controller = true,
};

static SGIModelAsicIdentity console_test_identity1 = {
    .ni_revision = 3,
    .widget_revision = 1,
    .serial = 0xc0b0a090808,
    .i2c_controller = true,
    .system_controller = true,
};

static SGIModelFlash console_test_flash = {
    .kind = SGI_MODEL_FLASH_KIND_AM29F080,
    .size = 0x100000,
};

static uint64_t console_test_bank0_mib[] = { 64, 0, 0, 0, 0, 0, 0, 0 };

static SGIModelNode console_test_nodes[] = {
    {
        .id = (char *)"node0-hub",
        .module_index = 0,
        .slot = 0,
        .asic_kind = SGI_MODEL_ASIC_KIND_HUB,
        .identity = &console_test_identity0,
        .slave_role = false,
        .flash = &console_test_flash,
    },
    {
        .id = (char *)"node1-hub",
        .module_index = 1,
        .slot = 0,
        .asic_kind = SGI_MODEL_ASIC_KIND_HUB,
        .identity = &console_test_identity1,
        .slave_role = true,
        .flash = &console_test_flash,
    },
};

static unsigned int console_test_m0_nodes[] = { 0 };
static unsigned int console_test_m1_nodes[] = { 1 };

static SGIModelModule console_test_modules[] = {
    {
        .id = (char *)"node0-cpu-module",
        .controller_index = -1,
    },
    {
        .id = (char *)"node1-cpu-module",
        .controller_index = -1,
    },
};

static SGIModelLinkEnd console_test_cable_ends[] = {
    { .kind = SGI_MODEL_LINK_END_KIND_NODE, .index = 0, .port = 0 },
    { .kind = SGI_MODEL_LINK_END_KIND_NODE, .index = 1, .port = 0 },
};

static SGIModelCable console_test_cables[] = {
    {
        .name = (char *)"peer-craylink",
        .kind = SGI_MODEL_CABLE_KIND_CABLE,
    },
};

/*
 * decoded_widget_mask and discovery_widget went with the ownership-rule
 * trim: they were IP29's own fixed Bridge decode behavior, a device
 * fact hw/mips/sgi/sn0.c now hardcodes (SN0_IP29_IO_DECODED_WIDGET_MASK,
 * SN0_IP29_IO_DISCOVERY_WIDGET), not per-topology data.
 */
static SGIModelXtalkAttachment console_test_xtalk[] = {
    { .crossbar_index = -1, .widget = 8, .node_index = 0 },
    { .crossbar_index = -1, .widget = 8, .node_index = 1 },
};

/*
 * Node0's and node1's boards each their own fitted-part and IOC3
 * serial so the two devices are distinguishable; no ISP1040 function --
 * a PCI function's own position is a device fact sgi-baseio's preset
 * table now carries, not this fixture's concern, and the serial line
 * contract is all this fixture exists to exercise.
 */
static SGIModelIoBoard console_test_io_boards[] = {
    {
        .id = (char *)"baseio",
        .module_index = 0,
        .preset = SGI_MODEL_IO_BOARD_PRESET_IP29_IO,
        .carrier = SGI_MODEL_IO_BOARD_CARRIER_ONBOARD,
        .attachment = 0,
        .revision = 4,
        .serial = 0x60504030201,
        .part = (char *)"030-1244-001",
        .has_ioc3_serial = true,
        .ioc3_serial = 0x5e7000000001,
    },
    {
        .id = (char *)"baseio",
        .module_index = 1,
        .preset = SGI_MODEL_IO_BOARD_PRESET_IP29_IO,
        .carrier = SGI_MODEL_IO_BOARD_CARRIER_ONBOARD,
        .attachment = 0,
        .revision = 4,
        .serial = 0x60504030202,
        .part = (char *)"030-1244-001",
        .has_ioc3_serial = true,
        .ioc3_serial = 0x5e7000000002,
    },
};

static unsigned int console_test_population_0[] = { 2, 2 };

static SGIModelCpuPopulation console_test_populations[] = {
    { },
};

static SGIModelTopology console_test_topology = {
    .name = (char *)"sn-builder-console-test",
    .generation = SGI_MODEL_GENERATION_SN0,
    .description = (char *)"console field serial line reordering test fixture",
    .console = 1,
    .junk_uart = -1,
};

/*
 * Console field test machine: a real sgi-sn-machine product whose test_
 * topology is console_test_topology, so an activated instance goes
 * through the real node, bridge and IOC3 construction sgi_machine_
 * compute_serial_line() feeds -- not a validator-only stand-in.
 * test_topology bypasses the topology= name lookup, since the compiled
 * catalogue never carries a fixture's own descriptor.
 *
 * "activate" turns on the real builder the way every other qtest-only
 * fixture's own switch does: qom-test and test-hmp instantiate every
 * registered machine type with no properties at all, and this fixture's
 * topology has two nodes as two sockets, which the default one-socket
 * instantiation those two use does not satisfy. Off by default, mc->init
 * is a no-op, the same as the "none" machine there; the qtest that wants
 * the real two-node machine turns it on itself.
 */
#define TYPE_SN_BUILDER_CONSOLE_TEST \
    MACHINE_TYPE_NAME("sn-builder-console-test")

static bool console_test_activate_requested;

static void console_test_set_activate(Object *obj, bool value, Error **errp)
{
    console_test_activate_requested = value;
}

/*
 * A descriptor fault sgi_machine_index_xtalk_attachments catches (the
 * function that splits the topology's xtalk-attachments into each node's
 * own entry and each io-boards entry's placement, still run at every
 * realize -- unlike sgi_machine_validate_topology, it is not just a
 * validator: it computes the indices the builder needs to place each
 * board): a node with no matching crosstalk attachment reaches it only
 * once construction is already under way. Unlike sn-builder-test.c's
 * user-option rejection tests, this runs the real builder
 * (sgi_machine_init), the only place this gap is reachable, so it forces
 * activation the way console-test's own "activate" does.
 */
static void console_test_set_construction_fault(Object *obj,
                                                 const char *value,
                                                 Error **errp)
{
    if (!strcmp(value, "node-without-xtalk-attachment")) {
        /* Node 1 loses its own attachment; node 0's now answers for both. */
        console_test_xtalk[1].node_index = 0;
    } else {
        error_setg(errp, "unknown sn-builder-console-test construction "
                  "fault '%s'", value);
        return;
    }
    console_test_activate_requested = true;
}

static void console_test_machine_init(MachineState *machine)
{
    if (!console_test_activate_requested) {
        return;
    }
    sgi_machine_init(machine);
}

static void console_test_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);

    mc->desc = "console field serial line reordering test fixture";
    mc->init = console_test_machine_init;
    mc->max_cpus = 4;
    mc->default_ram_id = "sn-builder-console-test.node0.ram";
    smc->test_topology = &console_test_topology;
    object_class_property_add_bool(oc, "activate", NULL,
                                   console_test_set_activate);
    object_class_property_set_description(oc, "activate",
        "Build the fixture's two nodes through the real builder. Off by "
        "default so qom-test and test-hmp's blanket sweep leaves this "
        "inert; the qtest that wants a real machine turns it on.");
    object_class_property_add_str(oc, "construction-fault", NULL,
                                  console_test_set_construction_fault);
    object_class_property_set_description(oc, "construction-fault",
        "Break the fixture's descriptor in a way that only reaches "
        "sgi_machine_index_xtalk_attachments, not tests/unit/test-sn-"
        "model.c's static checks, then build through the real builder. "
        "Implies activate.");
}

static const TypeInfo console_test_type = {
    .name = TYPE_SN_BUILDER_CONSOLE_TEST,
    .parent = TYPE_SGI_SN_MACHINE,
    .class_init = console_test_class_init,
};

/*
 * Bank test fixture: one HUB node shipping 256 MiB in bank 0 and 512 MiB
 * in bank 1 (768 MiB total), a population no product's accepted memory
 * list carries -- origin200's largest accepted total is 512 MiB, so
 * a real product can never populate a bank past IP29's 256 MiB first-
 * bank cap. The IP29 backdoor window's quarter-size aliasing scales with
 * each bank's own size (tests/qtest/sn0-address-map-test.c's
 * check_backdoor_bank_span), so proving bank 1 aliases at ITS size's own
 * quarter (128 MiB), not bank 0's (64 MiB), needs a bank 1 this large.
 * That is generic sgi_sn0_hub_hardware decode behavior, identical for
 * every SN0 node regardless of product, so exercising it here over a
 * fixture-only population is as hardware-accurate as exercising it over
 * a real product would be -- it is the product catalogue, not the Hub,
 * that happens not to sell this combination today.
 *
 * accepted_memory_mib is left absent (empty): sgi_machine_validate_
 * memory_population() then requires the requested per-node size to
 * match this node's shipped bank-mib total exactly, so only 768 MiB
 * is ever accepted here, the same "shipped population only" contract
 * origin2000-module's own nodes are documented to want.
 */
static SGIModelCpuSlot bank_test_cpu_slots[] = {
    { .interface = 0, .slot = 0, .assembly = &console_test_r10000_180mhz },
};

static uint64_t bank_test_bank_mib[] = { 256, 512, 0, 0, 0, 0, 0, 0 };

/* Zero-valued: this fixture exercises memory-bank placement, not identity. */
static SGIModelAsicIdentity bank_test_identity;

static SGIModelNode bank_test_nodes[] = {
    {
        .id = (char *)"n0",
        .module_index = 0,
        .slot = 0,
        .asic_kind = SGI_MODEL_ASIC_KIND_HUB,
        .identity = &bank_test_identity,
        .flash = &console_test_flash,
    },
};

static unsigned int bank_test_m0_nodes[] = { 0 };

static SGIModelModule bank_test_modules[] = {
    {
        .id = (char *)"m0",
        .controller_index = -1,
    },
};

static unsigned int bank_test_population_0[] = { 1 };

static SGIModelCpuPopulation bank_test_populations[] = {
    { },
};

/*
 * Every node needs its own board and crosstalk attachment --
 * sgi_machine_populate_node() asserts both -- so this fixture reuses
 * the console fixture's node 0 board and attachment verbatim: a
 * plain onboard IP29 preset with one IOC3, nothing this test touches.
 */
static SGIModelTopology bank_test_topology = {
    .name = (char *)"sn-builder-bank-test",
    .generation = SGI_MODEL_GENERATION_SN0,
    .description = (char *)"one node shipping two banks (256+512 MiB) past any "
                   "product's accepted total, for the IP29 backdoor's "
                   "per-bank aliasing",
    .console = 0,
    .junk_uart = -1,
};

#define TYPE_SN_BUILDER_BANK_TEST MACHINE_TYPE_NAME("sn-builder-bank-test")

static bool bank_test_activate_requested;

static void bank_test_set_activate(Object *obj, bool value, Error **errp)
{
    bank_test_activate_requested = value;
}

static void bank_test_machine_init(MachineState *machine)
{
    if (!bank_test_activate_requested) {
        return;
    }
    sgi_machine_init(machine);
}

static void bank_test_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);

    mc->desc = "two-bank IP29 backdoor aliasing test fixture";
    mc->init = bank_test_machine_init;
    mc->max_cpus = 1;
    mc->default_ram_id = "sn-builder-bank-test.node0.ram";
    smc->test_topology = &bank_test_topology;
    object_class_property_add_bool(oc, "activate", NULL,
                                   bank_test_set_activate);
    object_class_property_set_description(oc, "activate",
        "Build the fixture's node through the real builder. Off by "
        "default so qom-test and test-hmp's blanket sweep leaves this "
        "inert; the qtest that wants a real machine turns it on.");
}

static const TypeInfo bank_test_type = {
    .name = TYPE_SN_BUILDER_BANK_TEST,
    .parent = TYPE_SGI_SN_MACHINE,
    .class_init = bank_test_class_init,
};

static void sn_builder_fault_test_register_types(void)
{
    /*
     * Wire the QAPI list fields both fixture topologies need before
     * either type registers: class_init (which reads test_topology)
     * only ever runs later, on first use, but building the lists here
     * keeps every array-to-list conversion in one place instead of
     * split across two type_init functions.
     */
    console_test_nodes[0].cpu_slots =
        cpu_slot_list(console_test_cpu_slots, ARRAY_SIZE(console_test_cpu_slots));
    console_test_nodes[1].cpu_slots = console_test_nodes[0].cpu_slots;
    console_test_nodes[0].accepted_memory_mib = console_test_nodes[1].accepted_memory_mib =
        uint64_list(console_test_accepted_mib, ARRAY_SIZE(console_test_accepted_mib));
    console_test_nodes[0].bank_mib = console_test_nodes[1].bank_mib =
        uint64_list(console_test_bank0_mib, ARRAY_SIZE(console_test_bank0_mib));

    console_test_modules[0].nodes =
        uint32_list(console_test_m0_nodes, ARRAY_SIZE(console_test_m0_nodes));
    console_test_modules[1].nodes =
        uint32_list(console_test_m1_nodes, ARRAY_SIZE(console_test_m1_nodes));

    console_test_cables[0].ends =
        link_end_list(console_test_cable_ends, ARRAY_SIZE(console_test_cable_ends));

    console_test_populations[0].cpus_per_node =
        uint32_list(console_test_population_0, ARRAY_SIZE(console_test_population_0));

    console_test_topology.nodes =
        node_list(console_test_nodes, ARRAY_SIZE(console_test_nodes));
    console_test_topology.modules =
        module_list(console_test_modules, ARRAY_SIZE(console_test_modules));
    console_test_topology.cables =
        cable_list(console_test_cables, ARRAY_SIZE(console_test_cables));
    console_test_topology.xtalk_attachments =
        xtalk_attachment_list(console_test_xtalk, ARRAY_SIZE(console_test_xtalk));
    console_test_topology.io_boards =
        io_board_list(console_test_io_boards, ARRAY_SIZE(console_test_io_boards));
    console_test_topology.supported_cpu_populations =
        cpu_population_list(console_test_populations,
                            ARRAY_SIZE(console_test_populations));

    bank_test_nodes[0].cpu_slots =
        cpu_slot_list(bank_test_cpu_slots, ARRAY_SIZE(bank_test_cpu_slots));
    bank_test_nodes[0].bank_mib =
        uint64_list(bank_test_bank_mib, ARRAY_SIZE(bank_test_bank_mib));

    bank_test_modules[0].nodes =
        uint32_list(bank_test_m0_nodes, ARRAY_SIZE(bank_test_m0_nodes));

    bank_test_populations[0].cpus_per_node =
        uint32_list(bank_test_population_0, ARRAY_SIZE(bank_test_population_0));

    bank_test_topology.nodes =
        node_list(bank_test_nodes, ARRAY_SIZE(bank_test_nodes));
    bank_test_topology.modules =
        module_list(bank_test_modules, ARRAY_SIZE(bank_test_modules));
    /* Reuses the console fixture's own node-0 attachment and board. */
    bank_test_topology.xtalk_attachments =
        xtalk_attachment_list(console_test_xtalk, 1);
    bank_test_topology.io_boards =
        io_board_list(console_test_io_boards, 1);
    bank_test_topology.supported_cpu_populations =
        cpu_population_list(bank_test_populations, ARRAY_SIZE(bank_test_populations));

    type_register_static(&console_test_type);
    type_register_static(&bank_test_type);
}

type_init(sn_builder_fault_test_register_types)
