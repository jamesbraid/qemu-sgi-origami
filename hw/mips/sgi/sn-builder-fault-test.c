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
 * Console field test fixture: a two-node topology with console=1, each
 * node carrying its own base I/O controller the way Origin 200's two-tower
 * wiring does (node/module/xtalk/PCI values copied from the origin200-dual
 * descriptor in sn-model.c, minus its two SCSI functions, which the serial
 * line contract never touches). All shipping topologies have console=0,
 * so the reordering sgi_machine_compute_serial_line() does for a non-
 * default console is otherwise never built or realized, only validated.
 * SGIMachineClass.test_topology hands this straight to sgi_machine_init,
 * so the qtest in tests/qtest/sn-builder-test.c that boots
 * "sn-builder-console-test" exercises the real builder, not a stand-in.
 */
static const SGIModelProcessor console_test_r10000_180mhz = {
    .id = "r10000-180mhz",
    .kind = "R10000",
    .prid = 0x926,
    .fpu_prid = 0x900,
    .clock_hz = 0xaba9500,
    .cache_clock_hz = 0x55d4a80,
    .cache_mib = 1,
    .mode_bits = 0x1211a785,
};

static const SGIModelCpuSlot console_test_cpu_slots[] = {
    { .interface = 0, .slot = 0, .assembly = &console_test_r10000_180mhz },
    { .interface = 0, .slot = 1, .assembly = &console_test_r10000_180mhz },
};

static const uint64_t console_test_accepted_mib[] = { 512, 256, 128, 64 };

static const SGIModelFlashView console_test_flash_views[] = {
    { .name = "reset", .size = 0x100000 },
    { .name = "lboot", .size = 0x10000000 },
    { .name = "rboot", .size = 0x10000000 },
};

static const SGIModelNode console_test_nodes[] = {
    {
        .id = "node0-hub",
        .module_index = 0,
        .slot = 0,
        .asic_kind = SGI_MODEL_ASIC_HUB,
        .identity = {
            .ni_revision = 3,
            .widget_revision = 1,
            .serial = 0xc0b0a090807,
            .i2c_controller = true,
            .system_controller = true,
            .i2c_controller_offset = 0x220000,
            .i2c_elsc_addr = 8,
            .i2c_nvram_first_addr = 0x50,
            .i2c_nvram_pages = 8,
        },
        .cpu_slots = console_test_cpu_slots,
        .cpu_slot_count = ARRAY_SIZE(console_test_cpu_slots),
        .flash = {
            .kind = SGI_MODEL_FLASH_AM29F080,
            .size = 0x100000,
            .views = console_test_flash_views,
            .view_count = ARRAY_SIZE(console_test_flash_views),
        },
        .management_adapter_offset = 0x220000,
        .slave_role = false,
        .accepted_memory_mib = console_test_accepted_mib,
        .accepted_memory_mib_count = ARRAY_SIZE(console_test_accepted_mib),
        .bank_mib = { 64, 0, 0, 0, 0, 0, 0, 0 },
    },
    {
        .id = "node1-hub",
        .module_index = 1,
        .slot = 0,
        .asic_kind = SGI_MODEL_ASIC_HUB,
        .identity = {
            .ni_revision = 3,
            .widget_revision = 1,
            .serial = 0xc0b0a090808,
            .i2c_controller = true,
            .system_controller = true,
            .i2c_controller_offset = 0x220000,
            .i2c_elsc_addr = 8,
            .i2c_nvram_first_addr = 0x50,
            .i2c_nvram_pages = 8,
        },
        .cpu_slots = console_test_cpu_slots,
        .cpu_slot_count = ARRAY_SIZE(console_test_cpu_slots),
        .flash = {
            .kind = SGI_MODEL_FLASH_AM29F080,
            .size = 0x100000,
            .views = console_test_flash_views,
            .view_count = ARRAY_SIZE(console_test_flash_views),
        },
        .management_adapter_offset = 0x220000,
        .slave_role = true,
        .accepted_memory_mib = console_test_accepted_mib,
        .accepted_memory_mib_count = ARRAY_SIZE(console_test_accepted_mib),
        .bank_mib = { 64, 0, 0, 0, 0, 0, 0, 0 },
    },
};

static const unsigned int console_test_m0_nodes[] = { 0 };
static const unsigned int console_test_m1_nodes[] = { 1 };

static const SGIModelModule console_test_modules[] = {
    {
        .id = "node0-cpu-module",
        .controller_index = -1,
        .nodes = console_test_m0_nodes,
        .node_count = ARRAY_SIZE(console_test_m0_nodes),
    },
    {
        .id = "node1-cpu-module",
        .controller_index = -1,
        .nodes = console_test_m1_nodes,
        .node_count = ARRAY_SIZE(console_test_m1_nodes),
    },
};

static const SGIModelFabricLink console_test_links[] = {
    {
        .name = "peer-craylink",
        .ends = {
            { .kind = SGI_MODEL_FABRIC_NODE, .index = 0, .port = 0 },
            { .kind = SGI_MODEL_FABRIC_NODE, .index = 1, .port = 0 },
        },
        .kind = SGI_MODEL_LINK_CABLE,
    },
};

static const SGIModelXtalkAttachment console_test_xtalk[] = {
    {
        .crossbar_index = -1,
        .widget = 8,
        .node_index = 0,
        .decoded_widget_mask = 0xfffd,
        .discovery_widget = 0,
    },
    {
        .crossbar_index = -1,
        .widget = 8,
        .node_index = 1,
        .decoded_widget_mask = 0xfffd,
        .discovery_widget = 0,
    },
};

/*
 * One IOC3 per node's board, node0's and node1's boards each their own
 * onewire serial so the two devices are distinguishable; no ISP1040, since
 * SCSI takes no serial line and the serial line contract is all this
 * fixture exists to exercise.
 */
static const SGIModelPciFunction console_test_m0_baseio_pci[] = {
    {
        .kind = SGI_MODEL_PCI_IOC3,
        .device = 2,
        .function = 0,
        .revision = 1,
        .irq_input = 3,
        .subsystem_id = 0xc303,
        .serial = 0x5e7000000001,
        .ioc3_population = {
            .ethernet = true,
            .serial_a = true,
            .serial_b = true,
            .keyboard_mouse = true,
            .parallel = true,
            .timekeeper = true,
            .onewire_identity = true,
        },
    },
};

static const SGIModelPciFunction console_test_m1_baseio_pci[] = {
    {
        .kind = SGI_MODEL_PCI_IOC3,
        .device = 2,
        .function = 0,
        .revision = 1,
        .irq_input = 3,
        .subsystem_id = 0xc303,
        .serial = 0x5e7000000002,
        .ioc3_population = {
            .ethernet = true,
            .serial_a = true,
            .serial_b = true,
            .keyboard_mouse = true,
            .parallel = true,
            .timekeeper = true,
            .onewire_identity = true,
        },
    },
};

static const SGIModelIoBoard console_test_io_boards[] = {
    {
        .id = "baseio",
        .module_index = 0,
        .board_kind = SGI_MODEL_IO_BASEIO,
        .onboard = true,
        .slot_index = 0,
        .removable = false,
        .revision = 4,
        .serial = 0x60504030201,
        .part = "030-1244-001",
        .pci = console_test_m0_baseio_pci,
        .pci_count = ARRAY_SIZE(console_test_m0_baseio_pci),
        .ioc3_population = {
            .ethernet = true,
            .serial_a = true,
            .serial_b = true,
            .keyboard_mouse = true,
            .parallel = true,
            .timekeeper = true,
            .onewire_identity = true,
        },
    },
    {
        .id = "baseio",
        .module_index = 1,
        .board_kind = SGI_MODEL_IO_BASEIO,
        .onboard = true,
        .slot_index = 0,
        .removable = false,
        .revision = 4,
        .serial = 0x60504030202,
        .part = "030-1244-001",
        .pci = console_test_m1_baseio_pci,
        .pci_count = ARRAY_SIZE(console_test_m1_baseio_pci),
        .ioc3_population = {
            .ethernet = true,
            .serial_a = true,
            .serial_b = true,
            .keyboard_mouse = true,
            .parallel = true,
            .timekeeper = true,
            .onewire_identity = true,
        },
    },
};

static const unsigned int console_test_population_0[] = { 2, 2 };

static const SGIModelCpuPopulation console_test_populations[] = {
    {
        .cpus_per_node = console_test_population_0,
        .node_count = 2,
    },
};

static const SGIModelTopology console_test_topology = {
    .name = "sn-builder-console-test",
    .generation = SGI_MODEL_GENERATION_SN0,
    .description = "console field serial line reordering test fixture",
    .nodes = console_test_nodes,
    .node_count = ARRAY_SIZE(console_test_nodes),
    .modules = console_test_modules,
    .module_count = ARRAY_SIZE(console_test_modules),
    .links = console_test_links,
    .link_count = ARRAY_SIZE(console_test_links),
    .xtalk_attachments = console_test_xtalk,
    .xtalk_attachment_count = ARRAY_SIZE(console_test_xtalk),
    .io_boards = console_test_io_boards,
    .io_board_count = ARRAY_SIZE(console_test_io_boards),
    .console = 1,
    .junk_uart = -1,
    .supported_cpu_populations = console_test_populations,
    .supported_cpu_population_count = ARRAY_SIZE(console_test_populations),
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

/*
 * Console field test machine: a real sgi-sn-machine product whose test_
 * topology is console_test_topology, so an activated instance goes
 * through the real node, bridge and IOC3 construction sgi_machine_
 * compute_serial_line() feeds -- not a validator-only stand-in.
 * test_topology bypasses the topology= name lookup, since sgi_model_
 * entries[] never carries a fixture's own descriptor.
 *
 * "activate" gates the real builder the way sn-builder-fault-test's own
 * "fault" gates its validator: qom-test and test-hmp instantiate every
 * registered machine type with no properties at all, and this fixture's
 * topology has two nodes as two sockets, which the default one-socket
 * instantiation those two sweeps use does not satisfy. Off by default,
 * mc->init is a no-op, the same as the "none" machine there; the qtest
 * that wants the real two-node machine turns it on itself.
 */
#define TYPE_SN_BUILDER_CONSOLE_TEST \
    MACHINE_TYPE_NAME("sn-builder-console-test")

static bool console_test_activate_requested;

static void console_test_set_activate(Object *obj, bool value, Error **errp)
{
    console_test_activate_requested = value;
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
}

static const TypeInfo console_test_type = {
    .name = TYPE_SN_BUILDER_CONSOLE_TEST,
    .parent = TYPE_SGI_SN_MACHINE,
    .class_init = console_test_class_init,
};

static void console_test_register_types(void)
{
    type_register_static(&console_test_type);
}

type_init(console_test_register_types)

/*
 * Bank test fixture: one HUB node shipping 256 MiB in bank 0 and 512 MiB
 * in bank 1 (768 MiB total), a population no product's accepted_memory_
 * mib list carries -- origin200's largest accepted total is 512 MiB, so
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
 * match this node's shipped bank_mib[] total exactly, so only 768 MiB
 * is ever accepted here, the same "shipped population only" contract
 * origin2000-module's own nodes are documented to want.
 */
static const SGIModelCpuSlot bank_test_cpu_slots[] = {
    { .interface = 0, .slot = 0, .assembly = &console_test_r10000_180mhz },
};

static const SGIModelNode bank_test_nodes[] = {
    {
        .id = "n0",
        .module_index = 0,
        .slot = 0,
        .asic_kind = SGI_MODEL_ASIC_HUB,
        .cpu_slots = bank_test_cpu_slots,
        .cpu_slot_count = ARRAY_SIZE(bank_test_cpu_slots),
        .bank_mib = { 256, 512, 0, 0, 0, 0, 0, 0 },
    },
};

static const unsigned int bank_test_m0_nodes[] = { 0 };

static const SGIModelModule bank_test_modules[] = {
    {
        .id = "m0",
        .controller_index = -1,
        .nodes = bank_test_m0_nodes,
        .node_count = ARRAY_SIZE(bank_test_m0_nodes),
    },
};

static const unsigned int bank_test_population_0[] = { 1 };

static const SGIModelCpuPopulation bank_test_populations[] = {
    {
        .cpus_per_node = bank_test_population_0,
        .node_count = 1,
    },
};

/*
 * Every node needs its own board and crosstalk attachment --
 * sgi_machine_populate_node() asserts both -- so this fixture reuses
 * the console fixture's node 0 board and attachment verbatim: a
 * plain BASEIO with one IOC3, nothing this test touches.
 */
static const SGIModelTopology bank_test_topology = {
    .name = "sn-builder-bank-test",
    .generation = SGI_MODEL_GENERATION_SN0,
    .description = "one node shipping two banks (256+512 MiB) past any "
                   "product's accepted total, for the IP29 backdoor's "
                   "per-bank aliasing",
    .nodes = bank_test_nodes,
    .node_count = ARRAY_SIZE(bank_test_nodes),
    .modules = bank_test_modules,
    .module_count = ARRAY_SIZE(bank_test_modules),
    .xtalk_attachments = console_test_xtalk,
    .xtalk_attachment_count = 1,
    .io_boards = console_test_io_boards,
    .io_board_count = 1,
    .console = 0,
    .junk_uart = -1,
    .supported_cpu_populations = bank_test_populations,
    .supported_cpu_population_count = ARRAY_SIZE(bank_test_populations),
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

static void bank_test_register_types(void)
{
    type_register_static(&bank_test_type);
}

type_init(bank_test_register_types)
