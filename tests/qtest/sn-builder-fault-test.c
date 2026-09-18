/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Two machine types that run the real builder (sgi_machine_init) over a
 * topology no shipped product carries, each gated behind its own
 * "activate" property the way any qtest-only fixture here is: qom-test
 * and test-hmp instantiate every registered machine type with no
 * properties at all to probe generic QOM/HMP behavior, so leaving
 * "activate" off makes mc->init a no-op there, the same as the "none"
 * machine. SGIMachineClass.test_topology hands each fixture's topology
 * straight to sgi_machine_init, bypassing the topology= name lookup,
 * since sgi_model_entries[] never carries a fixture's own descriptor.
 *
 * This file used to also carry sn-builder-fault-test, a fixture that
 * called sgi_machine_validate_topology() directly over a minimal
 * two-node topology to exercise its named faults. That validator (and
 * the four it dispatched to) is gone: hw/mips/sgi/sn-machine.c now
 * trusts the compiled descriptor, and tests/unit/test-sn-model.c checks
 * its structural invariants once instead of at every launch. See
 * docs/plans/sn-machine-model.md, "The builder trusts the descriptor".
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/mips/sgi/sn-machine.h"

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

static SGIModelXtalkAttachment console_test_xtalk[] = {
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
        .board_kind = SGI_MODEL_IO_IP29_IO,
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
        .board_kind = SGI_MODEL_IO_IP29_IO,
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
 * Console field test machine: a real sgi-sn-machine product whose test_
 * topology is console_test_topology, so an activated instance goes
 * through the real node, bridge and IOC3 construction sgi_machine_
 * compute_serial_line() feeds -- not a validator-only stand-in.
 * test_topology bypasses the topology= name lookup, since sgi_model_
 * entries[] never carries a fixture's own descriptor.
 *
 * "activate" gates the real builder the way every other qtest-only
 * fixture's own switch does: qom-test and test-hmp instantiate every
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

/*
 * A descriptor fault sgi_machine_index_xtalk_attachments catches (the
 * function that splits t->xtalk_attachments[] into each node's own entry
 * and each io_boards[] entry's placement, still run at every realize --
 * unlike sgi_machine_validate_topology, it is not just a validator: it
 * computes the indices the builder needs to place each board): a node
 * with no matching crosstalk attachment reaches it only once construction
 * is already under way. Unlike sn-builder-test.c's user-option rejection
 * tests, this runs the real builder (sgi_machine_init), the only place
 * this gap is reachable, so it forces activation the way console-test's
 * own "activate" does.
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
