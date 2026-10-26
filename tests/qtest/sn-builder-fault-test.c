/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Two machine types that run the real builder (sgi_machine_init) over a
 * topology no shipped product carries, each turned on only by its own
 * "activate" property the way any qtest-only fixture here is: qom-test and
 * test-hmp instantiate every registered machine type with no properties at
 * all to probe generic QOM/HMP behavior, so leaving "activate" off makes
 * mc->init a no-op there, the same as the "none" machine.
 * SGIMachineClass.test_topology hands each fixture's topology straight to
 * sgi_machine_init, bypassing the topology= name lookup, since the compiled
 * catalogue never carries a fixture's own descriptor.
 *
 * This file used to also carry sn-builder-fault-test, a fixture that called
 * sgi_machine_validate_topology() directly over a minimal two-node topology
 * to exercise its named faults. That validator (and the four it dispatched
 * to) is gone: hw/mips/sgi/sn-machine.c now trusts the compiled descriptor,
 * and tests/unit/test-sn-model.c checks its structural invariants once
 * instead of at every launch.
 *
 * Every fixture topology below is built once, at type-registration time,
 * from plain arrays into the QAPI-generated list types qapi/sgi-machines.json
 * declares (hand-building a SGIModelTopology in C has no JSON to visit, so
 * nothing here goes through qobject_input_visitor. The list nodes just
 * point at the arrays' own elements, the same sharing the old
 * array-and-count descriptor gave this fixture's node-1-attachment mutation
 * below).
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/util.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi/sn-machine.h"
#include "hw/mips/sgi/sn1-node.h"
#include "hw/mips/sgi/sn1-fabric.h"
#include "hw/misc/sgi-sn1-router.h"
#include "system/memory.h"
#include "cpu.h"

/*
 * Fixture lists are built once at type registration and live as long as the
 * process, but a construction fault then replaces some of them wholesale.
 * Nothing points at a replaced list, which LeakSanitizer reports when QEMU
 * exits on a refused launch. Remember every allocation so it stays reachable.
 */
static GPtrArray *fixture_allocations;

static void *fixture_remember(void *p)
{
    if (!fixture_allocations) {
        fixture_allocations = g_ptr_array_new();
    }
    g_ptr_array_add(fixture_allocations, p);
    return p;
}

#define fixture_new0(type) ((type *)fixture_remember(g_new0(type, 1)))

static SGIModelCpuSlotList *cpu_slot_list(SGIModelCpuSlot *items, size_t count)
{
    SGIModelCpuSlotList *head = NULL;
    SGIModelCpuSlotList **tail = &head;
    size_t i;

    for (i = 0; i < count; i++) {
        SGIModelCpuSlotList *entry = fixture_new0(SGIModelCpuSlotList);

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
        SGIModelModuleList *entry = fixture_new0(SGIModelModuleList);

        entry->value = &items[i];
        *tail = entry;
        tail = &entry->next;
    }
    return head;
}

static SGIModelXioAttachmentList *xio_attachment_list(
    SGIModelXioAttachment *items, size_t count)
{
    SGIModelXioAttachmentList *head = NULL;
    SGIModelXioAttachmentList **tail = &head;
    size_t i;

    for (i = 0; i < count; i++) {
        SGIModelXioAttachmentList *entry =
            fixture_new0(SGIModelXioAttachmentList);

        entry->value = &items[i];
        *tail = entry;
        tail = &entry->next;
    }
    return head;
}

static SGIModelPciHostBridgeList *pci_host_bridge_list(
    SGIModelPciHostBridge *items, size_t count)
{
    SGIModelPciHostBridgeList *head = NULL;
    SGIModelPciHostBridgeList **tail = &head;
    size_t i;

    for (i = 0; i < count; i++) {
        SGIModelPciHostBridgeList *entry =
            fixture_new0(SGIModelPciHostBridgeList);

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
        SGIModelIoBoardList *entry = fixture_new0(SGIModelIoBoardList);

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
            fixture_new0(SGIModelCpuPopulationList);

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
        SGIModelNodeList *entry = fixture_new0(SGIModelNodeList);

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
        SGIModelCableList *entry = fixture_new0(SGIModelCableList);

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
        SGIModelLinkEndList *entry = fixture_new0(SGIModelLinkEndList);

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
        uint32List *entry = fixture_new0(uint32List);

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
        uint64List *entry = fixture_new0(uint64List);

        entry->value = items[i];
        *tail = entry;
        tail = &entry->next;
    }
    return head;
}

/*
 * A fixture's one node-population row: it fits every node board, Router
 * and cable the fixture names, uses no empty XIO slot, and accepts
 * @populations. The builder reads a machine's populations from its row.
 */
static SGIModelNodePopulationList *whole_population(
    const SGIModelTopology *t, SGIModelCpuPopulationList *populations)
{
    SGIModelNodePopulationList *entry =
        fixture_new0(SGIModelNodePopulationList);
    SGIModelNodePopulation *row = fixture_new0(SGIModelNodePopulation);
    uint32List **tail;
    unsigned int i;

    tail = &row->nodes;
    for (i = 0; i < QAPI_LIST_LENGTH(t->nodes); i++) {
        QAPI_LIST_APPEND(tail, i);
    }
    tail = &row->routers;
    for (i = 0; i < QAPI_LIST_LENGTH(t->routers); i++) {
        QAPI_LIST_APPEND(tail, i);
    }
    tail = &row->cables;
    for (i = 0; i < QAPI_LIST_LENGTH(t->cables); i++) {
        QAPI_LIST_APPEND(tail, i);
    }
    row->supported_cpu_populations = populations;
    row->grade = SGI_MODEL_EVIDENCE_GRADE_ESTABLISHED;
    entry->value = row;
    return entry;
}

/*
 * A second row, ahead of @rows, fitting only node 0 and accepting one
 * processor, graded @grade: the builder must refuse building it, for
 * the reason each fixture's row gives it.
 */
static SGIModelNodePopulationList *node0_row(SGIModelNodePopulationList *rows,
                                             SGIModelEvidenceGrade grade)
{
    SGIModelNodePopulationList *entry =
        fixture_new0(SGIModelNodePopulationList);
    SGIModelNodePopulation *row = fixture_new0(SGIModelNodePopulation);
    SGIModelCpuPopulation *one = fixture_new0(SGIModelCpuPopulation);

    QAPI_LIST_PREPEND(row->nodes, 0);
    QAPI_LIST_PREPEND(one->cpus_per_node, 1);
    QAPI_LIST_PREPEND(row->supported_cpu_populations, one);
    row->grade = grade;
    entry->value = row;
    entry->next = rows;
    return entry;
}

/*
 * Console field test fixture: a two-node topology with console=1, each
 * node carrying its own base I/O controller the way Origin 200's two-tower
 * wiring does (node/module/XIO/board values copied from the
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
    .id = (char *)"r10000-180mhz-1mb",
    .kind = (char *)"R10000",
    .prid = 0x926,
    .fpu_prid = 0x900,
    .clock_hz = 0xaba9500,
    .cache_clock_hz = 0x7270e00,
    .cache_mib = 1,
    .mode_bits = 0x1211a785,
};

static SGIModelCpuSlot console_test_cpu_slots[] = {
    { .interface = 0, .slot = 0, .assembly = &console_test_r10000_180mhz },
    { .interface = 0, .slot = 1, .assembly = &console_test_r10000_180mhz },
};

static uint64_t console_test_accepted_mib[] = { 512, 256, 128, 64 };

static SGIModelAsicIdentity console_test_identity0 = {
    .has_ni_revision = true,
    .ni_revision = 3,
    .has_widget_revision = true,
    .widget_revision = 1,
    .serial = 0xc0b0a090807,
    .i2c_controller = true,
    .system_controller = true,
};

static SGIModelAsicIdentity console_test_identity1 = {
    .has_ni_revision = true,
    .ni_revision = 3,
    .has_widget_revision = true,
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
static uint64_t console_test_heterogeneous_bank_mib[2][8] = {
    { 64, 0, 0, 0, 0, 0, 0, 0 },
    { 32, 0, 0, 0, 0, 0, 0, 0 },
};
static uint64List *console_test_heterogeneous_bank_lists[2];

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
 * IP29 Bridge decode mask and discovery widget are fixed device facts
 * in sn0.c, not topology fields.
 */
static SGIModelXioAttachment console_test_xio[] = {
    { .crossbar_index = -1, .widget = 8, .node_index = 0 },
    { .crossbar_index = -1, .widget = 8, .node_index = 1 },
};

/*
 * Node0's and node1's boards each their own fitted-part and IOC3 serial so
 * the two devices are distinguishable; no ISP1040 function. A PCI
 * function's own position is a device fact sgi-baseio's preset table now
 * carries, not this fixture's concern, and the serial line contract is all
 * this fixture exists to exercise.
 */
static SGIModelIoBoard console_test_io_boards[] = {
    {
        .id = (char *)"baseio",
        .module_index = 0,
        .populated = true,
        .preset = SGI_MODEL_IO_BOARD_PRESET_IP29_IO,
        .carrier = SGI_MODEL_IO_BOARD_CARRIER_ONBOARD,
        .attachment = 0,
        .has_attachment = true,
        .has_revision = true,
        .revision = 4,
        .has_serial = true,
        .serial = 0x60504030201,
        .part = (char *)"030-1244-001",
        .has_ioc3_serial = true,
        .ioc3_serial = 0x5e7000000001,
    },
    {
        .id = (char *)"baseio",
        .module_index = 1,
        .populated = true,
        .preset = SGI_MODEL_IO_BOARD_PRESET_IP29_IO,
        .carrier = SGI_MODEL_IO_BOARD_CARRIER_ONBOARD,
        .attachment = 0,
        .has_attachment = true,
        .has_revision = true,
        .revision = 4,
        .has_serial = true,
        .serial = 0x60504030202,
        .part = (char *)"030-1244-001",
        .has_ioc3_serial = true,
        .ioc3_serial = 0x5e7000000002,
    },
};

/* Unpopulated synthetic IO9, used only to test descriptor preflight. */
static SGIModelIoBoard console_test_io9_board = {
    .id = (char *)"fixture-io9",
    .module_index = 0,
    .populated = false,
    .preset = SGI_MODEL_IO_BOARD_PRESET_IO9,
    .carrier = SGI_MODEL_IO_BOARD_CARRIER_MACHINE_PLACED,
    .has_pci_host_index = true,
    .pci_host_index = 0,
    .has_pci_head = true,
    .pci_head = 0,
};

static SGIModelIoBoard console_test_pic_io_boards[4];

static SGIModelXioAttachment console_test_pic_xio[] = {
    { .crossbar_index = -1, .widget = 8, .node_index = 0 },
    { .crossbar_index = -1, .widget = 8, .node_index = 1 },
    { .crossbar_index = -1, .widget = 9, .node_index = 0 },
};

static SGIModelPciHostBridge console_test_pic_hosts[2] = {
    {
        .id = (char *)"fixture-pic",
        .kind = SGI_MODEL_PCI_HOST_KIND_PIC,
        .xio_attachment_index = 2,
    },
    {
        .id = (char *)"fixture-pic-second",
        .kind = SGI_MODEL_PCI_HOST_KIND_PIC,
        .xio_attachment_index = 2,
    },
};

static unsigned int console_test_population_0[] = { 2, 2 };

static SGIModelCpuPopulation console_test_populations[] = {
    { },
};

/* An IP29 board's flash configuration values, as Origin 200 carries. */
static SGIModelIp27FirmwareConfig console_test_firmware = {
    .machine_type = 1,
    .flash_read_cycles = 15,
    .flash_write_cycles = 4,
};

static SGIModelTopology console_test_topology = {
    .name = (char *)"sn-builder-console-test",
    .generation = SGI_MODEL_GENERATION_SN0,
    .description = (char *)"console field serial line reordering test fixture",
    .console = 1,
    .junk_uart = -1,
    .ip27_firmware = &console_test_firmware,
};

/*
 * Console field test machine: a real sgi-sn-machine product whose test_
 * topology is console_test_topology, so an activated instance goes through
 * the real node, bridge and IOC3 construction sgi_machine_
 * compute_serial_line() feeds, instead of a validator-only stand-in.
 * test_topology bypasses the topology= name lookup, since the compiled
 * catalogue never carries a fixture's own descriptor.
 *
 * "activate" turns on the real builder the way every other qtest-only
 * fixture's own switch does: qom-test and test-hmp instantiate every
 * registered machine type with no properties at all, and this fixture's
 * topology has two nodes as two sockets, which the default one-socket
 * instantiation those two use does not satisfy. Off by default, mc->init is
 * a no-op, the same as the "none" machine there; the qtest that wants the
 * real two-node machine turns it on itself.
 */
#define TYPE_SN_BUILDER_CONSOLE_TEST \
    MACHINE_TYPE_NAME("sn-builder-console-test")

static bool console_test_activate_requested;

static void console_test_set_activate(Object *obj, bool value, Error **errp)
{
    console_test_activate_requested = value;
}

static void console_test_install_pic_descriptor(bool include_io9,
                                                size_t host_count)
{
    memcpy(console_test_pic_io_boards, console_test_io_boards,
           sizeof(console_test_io_boards));
    console_test_pic_io_boards[2] = console_test_io9_board;
    console_test_pic_io_boards[3] = console_test_io9_board;
    console_test_pic_io_boards[3].id = (char *)"fixture-io9-second";
    console_test_topology.xio_attachments =
        xio_attachment_list(console_test_pic_xio,
                            ARRAY_SIZE(console_test_pic_xio));
    console_test_topology.pci_host_bridges =
        pci_host_bridge_list(console_test_pic_hosts, host_count);
    console_test_topology.has_pci_host_bridges = true;
    console_test_topology.io_boards = io_board_list(
        console_test_pic_io_boards, 2 + (include_io9 ? 1 : 0));
}

/*
 * A descriptor fault sgi_machine_index_xio_attachments catches (the
 * function that splits the topology's xio-attachments into each node's own
 * entry and each io-boards entry's placement, still run at every realize,
 * unlike sgi_machine_validate_topology, it is not just a validator: it
 * computes the indices the builder needs to place each board): a node with
 * no matching XIO attachment reaches it only once construction is
 * already under way. Unlike sn-builder-test.c's user-option rejection tests,
 * this runs the real builder (sgi_machine_init), the only place this gap is
 * reachable, so it forces activation the way console-test's own "activate"
 * does.
 */
static void console_test_set_construction_fault(Object *obj,
                                                 const char *value,
                                                 Error **errp)
{
    if (!strcmp(value, "node-without-xio-attachment")) {
        /* Node 1 loses its own attachment; node 0's now answers for both. */
        console_test_xio[1].node_index = 0;
    } else if (!strcmp(value, "board-selector-on-non-bedrock")) {
        console_test_nodes[0].has_board_kind = true;
        console_test_nodes[0].board_kind = SGI_MODEL_BOARD_KIND_IP34;
    } else if (!strcmp(value, "heterogeneous-memory")) {
        for (unsigned int i = 0; i < ARRAY_SIZE(console_test_nodes); i++) {
            if (!console_test_heterogeneous_bank_lists[i]) {
                console_test_heterogeneous_bank_lists[i] = uint64_list(
                    console_test_heterogeneous_bank_mib[i],
                    ARRAY_SIZE(console_test_heterogeneous_bank_mib[i]));
            }
            console_test_nodes[i].bank_mib =
                console_test_heterogeneous_bank_lists[i];
        }
    } else if (!strcmp(value, "pic-io9")) {
        console_test_install_pic_descriptor(true, 1);
    } else if (!strcmp(value, "pic-host-only")) {
        console_test_install_pic_descriptor(false, 1);
    } else if (!strcmp(value, "io9-with-xio-reference")) {
        console_test_install_pic_descriptor(true, 1);
        console_test_pic_io_boards[2].has_attachment = true;
        console_test_pic_io_boards[2].attachment = 0;
    } else if (!strcmp(value, "io9-missing-pci-head")) {
        console_test_install_pic_descriptor(true, 1);
        console_test_pic_io_boards[2].has_pci_head = false;
    } else if (!strcmp(value, "io9-no-pci-host")) {
        console_test_install_pic_descriptor(true, 1);
        console_test_pic_io_boards[2].has_pci_host_index = false;
    } else if (!strcmp(value, "io9-bad-pci-host")) {
        console_test_install_pic_descriptor(true, 1);
        console_test_pic_io_boards[2].pci_host_index = 1;
    } else if (!strcmp(value, "io9-bad-pci-head")) {
        console_test_install_pic_descriptor(true, 1);
        console_test_pic_io_boards[2].pci_head = 2;
    } else if (!strcmp(value, "io9-duplicate-host-head")) {
        console_test_install_pic_descriptor(true, 1);
        console_test_topology.io_boards = io_board_list(
            console_test_pic_io_boards, 4);
    } else if (!strcmp(value, "pic-duplicate-xio")) {
        console_test_install_pic_descriptor(false, 2);
    } else if (!strcmp(value, "pic-xio-out-of-range")) {
        console_test_install_pic_descriptor(false, 1);
        console_test_pic_hosts[0].xio_attachment_index = 3;
    } else if (!strcmp(value, "io9-empty-host-array")) {
        console_test_install_pic_descriptor(true, 0);
    } else if (!strcmp(value, "legacy-without-attachment")) {
        console_test_io_boards[0].has_attachment = false;
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
        "Introduce a test-only descriptor fault and run the real builder. "
        "Implies activate.");
}

static const TypeInfo console_test_type = {
    .name = TYPE_SN_BUILDER_CONSOLE_TEST,
    .parent = TYPE_SGI_SN_MACHINE,
    .class_init = console_test_class_init,
};

/*
 * Bank test fixture: one HUB node of 768 MiB, which the IP29 bank rule lays
 * out as 128 MiB in bank 0, 512 MiB in bank 1 and 128 MiB in bank 2. The
 * IP29 backdoor window's quarter-size aliasing scales with each bank's own
 * size (tests/qtest/sn0-address-map-test.c's check_backdoor_bank_span), so
 * proving bank 1 aliases at ITS size's own quarter (128 MiB), not bank 0's
 * (32 MiB), needs a bank 1 this large. That is generic sgi_sn0_hub_hardware
 * decode behavior, identical for every SN0 node regardless of product, so
 * exercising it here over a fixture-only population is as hardware-accurate
 * as exercising it over a real product would be. It is the product
 * catalogue, not the Hub, that happens not to sell this combination today.
 *
 * accepted_memory_mib is left absent (empty): sgi_machine_validate_
 * memory_population() then requires the requested per-node size to match
 * this node's shipped bank-mib total exactly, so only 768 MiB is ever
 * accepted here, the same "shipped population only" contract
 * origin2000-rack's own nodes are documented to want.
 */
static SGIModelCpuSlot bank_test_cpu_slots[] = {
    { .interface = 0, .slot = 0, .assembly = &console_test_r10000_180mhz },
};

static uint64_t bank_test_bank_mib[] = { 128, 512, 128, 0, 0, 0, 0, 0 };

/* The fixture tests memory-bank placement, so revision values stay zero. */
static SGIModelAsicIdentity bank_test_identity = {
    .has_ni_revision = true,
    .has_widget_revision = true,
};

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
 * sgi_machine_populate_node() requires a board and XIO attachment
 * per node. Reuse the console fixture's node 0 IP29 BaseIO and IOC3.
 */
static SGIModelTopology bank_test_topology = {
    .name = (char *)"sn-builder-bank-test",
    .generation = SGI_MODEL_GENERATION_SN0,
    .description = (char *)"one node shipping three banks (128+512+128 MiB), "
                   "for the IP29 backdoor's per-bank aliasing",
    .console = 0,
    .junk_uart = -1,
    .ip27_firmware = &console_test_firmware,
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

/*
 * Two-node Bedrock fixture: a Bedrock topology may carry more than one node
 * in the descriptor (the two-chassis Origin 300 is the shipped one, with a
 * cable this fixture leaves out). This fixture proves the builder's
 * validation and its node loop accept a two-node Bedrock topology and build
 * both nodes through it: each node carries its own populated IO-8 board, at
 * its own attachment, so the node loop's per-node widget and board-selection
 * filtering is exercised for both. No controllers (no L1 is expected) and no
 * crossbars (Bedrock's own XBridge crossbar is not a real Crossbow chip the
 * builder instantiates). Each node still needs a real boot flash and a
 * stock-PROM image the same size, since sn1_node_realize's own PROM policy
 * is unconditional; the qtest below supplies a blank one sized to
 * bedrock_test_flash.
 */
/* Synthetic register fixtures use an implemented R14000 identity. */
static SGIModelProcessor bedrock_test_processor = {
    .id = (char *)"test-r14000",
    .clock_hz = 500000000,
    .kind = (char *)"R14000",
    .cache_clock_hz = 250000000,
    .cache_mib = 2,
    .has_mode_bits = true,
    .mode_bits = 0,
    .prid = 0x0f24,
    .fpu_prid = 0,
};

static SGIModelBedrockConfig bedrock_test_config = {
    .has_board_id_word = true,
    .board_id_word = 0x8001,
    .local_region = 0,
    .initiator_region = 0,
    .installed_bank_mask = 0x0c,
};

static SGIModelAsicIdentity bedrock_test_identity = {
    .has_widget_revision = true,
    .widget_revision = 2,
    .has_clock_hz = true,
    .clock_hz = 200000000,
};

static SGIModelL1Revision bedrock_test_l1_revision = {
    .major = 1, .minor = 18, .patch = 5,
};

static SGIModelL1Config bedrock_test_l1 = {
    .has_reported_type = true,
    .reported_type = 0x34,
    .revision = &bedrock_test_l1_revision,
};

static SGIModelController bedrock_test_controllers[] = {
    { .id = (char *)"l1-n0", .module_index = 0,
      .kind = SGI_MODEL_CONTROLLER_KIND_L1, .l1 = &bedrock_test_l1 },
    { .id = (char *)"l1-n1", .module_index = 1,
      .kind = SGI_MODEL_CONTROLLER_KIND_L1, .l1 = &bedrock_test_l1 },
};

static SGIModelModule bedrock_test_modules[] = {
    { .id = (char *)"module-n0", .controller_index = 0 },
    { .id = (char *)"module-n1", .controller_index = 1 },
};

static SGIModelPciFunction bedrock_test_pci_functions[] = {
    { .kind = SGI_MODEL_PCI_FUNCTION_KIND_ISP12160, .revision = 6 },
    { .kind = SGI_MODEL_PCI_FUNCTION_KIND_IOC3, .revision = 1 },
    { .kind = SGI_MODEL_PCI_FUNCTION_KIND_LUCENT_OHCI, .revision = 16 },
};

static SGIModelCpuSlot bedrock_test_cpu_slots[] = {
    { .interface = 0, .slot = 0, .assembly = &bedrock_test_processor },
};

static SGIModelFlash bedrock_test_flash = {
    .kind = SGI_MODEL_FLASH_KIND_AM29F080,
    .size = 0x100000,
};

static uint64_t bedrock_test_bank_mib[] = { 64, 0, 0, 0, 0, 0, 0, 0 };
static uint64_t bedrock_unfingerprinted_bank_mib[2][8] = {
    { 64, 0, 0, 0, 0, 0, 0, 0 },
    { 32, 0, 0, 0, 0, 0, 0, 0 },
};
static uint64List *bedrock_unfingerprinted_bank_lists[2];

static SGIModelNode bedrock_test_nodes[] = {
    {
        .id = (char *)"n0",
        .module_index = 0,
        .slot = 0,
        .asic_kind = SGI_MODEL_ASIC_KIND_BEDROCK,
        .flash = &bedrock_test_flash,
        .identity = &bedrock_test_identity,
        .bedrock = &bedrock_test_config,
    },
    {
        .id = (char *)"n1",
        .module_index = 1,
        .slot = 1,
        .asic_kind = SGI_MODEL_ASIC_KIND_BEDROCK,
        .flash = &bedrock_test_flash,
        .identity = &bedrock_test_identity,
        .bedrock = &bedrock_test_config,
    },
};

static SGIModelXioAttachment bedrock_test_xio[] = {
    { .crossbar_index = -1, .widget = 15, .node_index = 0 },
    { .crossbar_index = -1, .widget = 15, .node_index = 1 },
};

static SGIModelIoBoard bedrock_test_io_boards[] = {
    {
        .id = (char *)"io8-n0",
        .module_index = 0,
        .populated = true,
        .preset = SGI_MODEL_IO_BOARD_PRESET_IO8,
        .carrier = SGI_MODEL_IO_BOARD_CARRIER_MACHINE_PLACED,
        .attachment = 0,
        .has_attachment = true,
        .has_revision = true,
        .revision = 2,
        .has_serial = true,
        .serial = 0x60504030201,
        .part = (char *)"030-1673-003",
        .has_ioc3_subsystem_id = true,
        .ioc3_subsystem_id = 0xffff,
        .has_mac_source = true,
        .mac_source = SGI_MODEL_MAC_SOURCE_NONE,
        .has_ioc3_serial = true,
        .ioc3_serial = 0x5e7000000001,
    },
    {
        .id = (char *)"io8-n1",
        .module_index = 1,
        .populated = true,
        .preset = SGI_MODEL_IO_BOARD_PRESET_IO8,
        .carrier = SGI_MODEL_IO_BOARD_CARRIER_MACHINE_PLACED,
        .attachment = 1,
        .has_attachment = true,
        .has_revision = true,
        .revision = 2,
        .has_serial = true,
        .serial = 0x60504030202,
        .part = (char *)"030-1673-003",
        .has_ioc3_subsystem_id = true,
        .ioc3_subsystem_id = 0xffff,
        .has_mac_source = true,
        .mac_source = SGI_MODEL_MAC_SOURCE_NONE,
        .has_ioc3_serial = true,
        .ioc3_serial = 0x5e7000000002,
    },
};

static unsigned int bedrock_test_population_0[] = { 1, 1 };

static SGIModelCpuPopulation bedrock_test_populations[] = {
    { },
};

static SGIModelTopology bedrock_test_topology = {
    .name = (char *)"sn-builder-bedrock-test",
    .generation = SGI_MODEL_GENERATION_SN1,
    .description = (char *)"two-node Bedrock topology fixture, for the node "
                   "loop and validation only",
    .console = 0,
    .junk_uart = -1,
};

static unsigned int ip53_test_population_cpus[] = { 1 };
static SGIModelCpuPopulation ip53_test_population;
static SGIModelPciFunction ip53_test_ioc4 = {
    .kind = SGI_MODEL_PCI_FUNCTION_KIND_IOC4,
    .revision = 0x53,
    .has_pci_device = true,
    .pci_device = 1,
    .has_pci_function = true,
    .pci_function = 0,
    .has_intx_pin = true,
    .intx_pin = SGI_MODEL_PCI_INTX_PIN_INTA,
    .has_pic_input = true,
    .pic_input = 3,
    .has_clock_hz = true,
    .clock_hz = 33333333,
    .has_serial_dma_experimental = true,
    .serial_dma_experimental = true,
};
static SGIModelPciFunction ip53_test_isp12160 = {
    .kind = SGI_MODEL_PCI_FUNCTION_KIND_ISP12160,
    .revision = 0x06,
    .has_pci_device = true,
    .pci_device = 2,
    .has_pci_function = true,
    .pci_function = 0,
    .has_intx_pin = true,
    .intx_pin = SGI_MODEL_PCI_INTX_PIN_INTA,
    .has_pic_input = true,
    .pic_input = 4,
};
static SGIModelPciFunction ip53_test_bcm5701 = {
    .kind = SGI_MODEL_PCI_FUNCTION_KIND_BCM5701,
    .revision = 0x00,
    .has_pci_device = true,
    .pci_device = 3,
    .has_pci_function = true,
    .pci_function = 0,
    .has_intx_pin = true,
    .intx_pin = SGI_MODEL_PCI_INTX_PIN_INTA,
    .has_pic_input = true,
    .pic_input = 5,
};
static SGIModelPciFunctionList ip53_test_function_list[] = {
    { .value = &ip53_test_ioc4, .next = &ip53_test_function_list[1] },
    { .value = &ip53_test_isp12160, .next = &ip53_test_function_list[2] },
    { .value = &ip53_test_bcm5701 },
};
static SGIModelPciHostBridge ip53_test_pic = {
    .id = (char *)"fixture-pic",
    .kind = SGI_MODEL_PCI_HOST_KIND_PIC,
    .xio_attachment_index = 0,
    .has_compatibility_profile = true,
    .compatibility_profile =
        SGI_MODEL_PIC_COMPATIBILITY_PROFILE_PIC_REV3_BEDROCK,
};
static SGIModelPciHostBridge ip53_test_pic_second = {
    .id = (char *)"pic-xio-f",
    .kind = SGI_MODEL_PCI_HOST_KIND_PIC,
    .xio_attachment_index = 1,
    .has_compatibility_profile = true,
    .compatibility_profile =
        SGI_MODEL_PIC_COMPATIBILITY_PROFILE_PIC_REV3_BEDROCK,
};
static uint64_t ip53_test_bank_mib[] = { 0, 0, 64, 0, 0, 0, 0, 0 };
static SGIModelNode ip53_test_node;
static SGIModelBedrockConfig ip53_test_bedrock_config;
static uint64List *ip53_test_bank_mib_list;
static SGIModelNode ip53_memory_test_nodes[2];
static SGIModelBedrockConfig ip53_memory_test_configs[2];
static uint64_t ip53_memory_test_bank_mib[2][8] = {
    { 0, 0, 16, 16, 0, 0, 0, 0 },
    { 0, 0, 16, 0, 0, 0, 0, 0 },
};
static uint64_t ip53_memory_test_accepted_mib[] = { 32 };
static uint64List *ip53_memory_test_bank_lists[2];
static unsigned int ip53_memory_test_population[] = { 1, 1 };
static unsigned int ip53_memory_test_module_nodes[2][1] = { { 0 }, { 1 } };
static SGIModelControllerList ip53_memory_test_controllers[2];

static SGIModelXioAttachment ip53_test_xio = {
    .crossbar_index = -1,
    .widget = 9,
    .node_index = 0,
};
static SGIModelXioAttachment ip53_test_xio_second = {
    .crossbar_index = -1,
    .widget = 15,
    .node_index = 0,
};
static SGIModelXioAttachment ip53_test_unowned_xio = {
    .crossbar_index = -1,
    .widget = 14,
    .node_index = 0,
};
static SGIModelCrossbar ip53_test_crossbar = {
    .id = (char *)"fixture-crossbar",
    .module_index = 0,
    .kind = SGI_MODEL_CROSSBAR_KIND_XBOW,
    .widget_revision = 1,
};
static SGIModelXioAttachmentList ip53_test_attachment_entry = {
    .value = &ip53_test_xio,
};
static SGIModelXioAttachmentList ip53_test_unowned_attachment_entry = {
    .value = &ip53_test_unowned_xio,
};
static SGIModelCrossbarList ip53_test_crossbar_entry = {
    .value = &ip53_test_crossbar,
};
static SGIModelIoBoard ip53_test_io9 = {
    .id = (char *)"fixture-io9",
    .module_index = 0,
    .preset = SGI_MODEL_IO_BOARD_PRESET_IO9,
    .carrier = SGI_MODEL_IO_BOARD_CARRIER_MACHINE_PLACED,
    .has_pci_host_index = true,
    .pci_host_index = 0,
    .has_pci_head = true,
    .pci_head = 0,
    .populated = true,
    .has_ds1742 = true,
    .ds1742 = true,
    .pci_functions = &ip53_test_function_list[0],
};

static SGIModelAsicIdentity ip53_test_identity = {
    .has_widget_revision = true,
    .widget_revision = 2,
    .has_clock_hz = true,
    .clock_hz = 200000000,
};

static void bedrock_test_install_ip53_descriptor(bool include_profile)
{
    static unsigned int one_node[] = { 0 };
    static SGIModelControllerList controller_entry;
    static SGIModelPciHostBridgeList host_entry;
    static SGIModelIoBoardList board_entry;

    ip53_test_pic.id = (char *)"fixture-pic";
    ip53_test_pic.xio_attachment_index = 0;
    ip53_test_xio.widget = 9;
    ip53_test_io9.populated = true;
    ip53_test_io9.has_ds1742 = true;
    ip53_test_io9.ds1742 = true;
    ip53_test_io9.pci_host_index = 0;
    ip53_test_io9.pci_head = 0;
    ip53_test_node = bedrock_test_nodes[0];
    ip53_test_bedrock_config = bedrock_test_config;
    ip53_test_bedrock_config.installed_bank_mask = 0x04;
    ip53_test_node.has_board_kind = true;
    ip53_test_node.board_kind = SGI_MODEL_BOARD_KIND_IP53;
    ip53_test_identity.has_ni_revision = true;
    ip53_test_identity.ni_revision = 2;
    ip53_test_identity.has_widget_revision = true;
    ip53_test_identity.widget_revision = 2;
    ip53_test_identity.clock_hz = 200000000;
    ip53_test_node.identity = &ip53_test_identity;
    ip53_test_node.bedrock = &ip53_test_bedrock_config;
    if (!ip53_test_bank_mib_list) {
        ip53_test_bank_mib_list = uint64_list(ip53_test_bank_mib,
                                               ARRAY_SIZE(ip53_test_bank_mib));
    }
    ip53_test_node.bank_mib = ip53_test_bank_mib_list;
    bedrock_test_topology.nodes = node_list(&ip53_test_node, 1);
    bedrock_test_topology.modules = module_list(bedrock_test_modules, 1);
    bedrock_test_topology.modules->value->nodes = uint32_list(one_node, 1);
    controller_entry.value = &bedrock_test_controllers[0];
    bedrock_test_topology.controllers = &controller_entry;
    ip53_test_attachment_entry.next = NULL;
    bedrock_test_topology.xio_attachments = &ip53_test_attachment_entry;
    ip53_test_unowned_xio.crossbar_index = -1;
    ip53_test_unowned_attachment_entry.next = NULL;
    bedrock_test_topology.crossbars = NULL;
    bedrock_test_topology.pci_host_bridges = NULL;
    ip53_test_pic.has_compatibility_profile = include_profile;
    host_entry.value = &ip53_test_pic;
    bedrock_test_topology.pci_host_bridges = &host_entry;
    bedrock_test_topology.has_pci_host_bridges = true;
    board_entry.value = &ip53_test_io9;
    bedrock_test_topology.io_boards = &board_entry;
    ip53_test_population.cpus_per_node =
        uint32_list(ip53_test_population_cpus, 1);
    bedrock_test_topology.node_populations = whole_population(
        &bedrock_test_topology,
        cpu_population_list(&ip53_test_population, 1));
}

static void bedrock_test_install_ip53_memory_descriptor(bool swapped)
{
    bedrock_test_install_ip53_descriptor(true);
    ip53_test_identity.has_ni_revision = true;
    ip53_test_identity.ni_revision = 2;
    ip53_memory_test_nodes[0] = ip53_test_node;
    ip53_memory_test_nodes[1] = bedrock_test_nodes[1];
    ip53_memory_test_nodes[1].has_board_kind = true;
    ip53_memory_test_nodes[1].board_kind = SGI_MODEL_BOARD_KIND_IP53;
    ip53_memory_test_nodes[1].identity = &ip53_test_identity;
    ip53_memory_test_configs[0] = ip53_test_bedrock_config;
    ip53_memory_test_configs[1] = ip53_test_bedrock_config;
    ip53_memory_test_configs[0].installed_bank_mask = 0x0c;
    ip53_memory_test_configs[1].installed_bank_mask = 0x04;
    ip53_memory_test_nodes[0].bedrock = &ip53_memory_test_configs[0];
    ip53_memory_test_nodes[1].bedrock = &ip53_memory_test_configs[1];
    if (swapped) {
        ip53_memory_test_bank_mib[0][3] = 0;
        ip53_memory_test_bank_mib[1][3] = 16;
        ip53_memory_test_configs[0].installed_bank_mask = 0x04;
        ip53_memory_test_configs[1].installed_bank_mask = 0x0c;
    } else {
        ip53_memory_test_bank_mib[0][3] = 16;
        ip53_memory_test_bank_mib[1][3] = 0;
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(ip53_memory_test_nodes); i++) {
        uint64List *bank;

        if (!ip53_memory_test_bank_lists[i]) {
            ip53_memory_test_bank_lists[i] = uint64_list(
                ip53_memory_test_bank_mib[i],
                ARRAY_SIZE(ip53_memory_test_bank_mib[i]));
        }
        bank = ip53_memory_test_bank_lists[i];
        for (unsigned int j = 0; j < ARRAY_SIZE(ip53_memory_test_bank_mib[i]);
             j++, bank = bank->next) {
            bank->value = ip53_memory_test_bank_mib[i][j];
        }
        ip53_memory_test_nodes[i].bank_mib = ip53_memory_test_bank_lists[i];
        ip53_memory_test_nodes[i].has_board_kind = true;
        ip53_memory_test_nodes[i].board_kind = SGI_MODEL_BOARD_KIND_IP53;
        ip53_memory_test_nodes[i].identity = &ip53_test_identity;
    }
    bedrock_test_topology.nodes = node_list(
        ip53_memory_test_nodes, ARRAY_SIZE(ip53_memory_test_nodes));
    bedrock_test_modules[0].nodes = uint32_list(
        ip53_memory_test_module_nodes[0], 1);
    bedrock_test_modules[1].nodes = uint32_list(
        ip53_memory_test_module_nodes[1], 1);
    bedrock_test_topology.modules = module_list(
        bedrock_test_modules, ARRAY_SIZE(bedrock_test_modules));
    ip53_memory_test_controllers[0].value = &bedrock_test_controllers[0];
    ip53_memory_test_controllers[0].next = &ip53_memory_test_controllers[1];
    ip53_memory_test_controllers[1].value = &bedrock_test_controllers[1];
    ip53_memory_test_controllers[1].next = NULL;
    bedrock_test_topology.controllers = ip53_memory_test_controllers;
    /* The sole PIC/IO9 owner is node 0; node 1 contributes memory only. */
    ip53_test_pic.xio_attachment_index = 0;
    ip53_test_population.cpus_per_node = uint32_list(
        ip53_memory_test_population, ARRAY_SIZE(ip53_memory_test_population));
    bedrock_test_topology.node_populations = whole_population(
        &bedrock_test_topology,
        cpu_population_list(&ip53_test_population, 1));
}

#define TYPE_SN_BUILDER_BEDROCK_TEST \
    MACHINE_TYPE_NAME("sn-builder-bedrock-test")

static const char * const bedrock_test_r14000_cpu_types[] = {
    MIPS_CPU_TYPE_NAME("R14000"),
    NULL,
};

static bool bedrock_test_activate_requested;

static void bedrock_test_set_activate(Object *obj, bool value, Error **errp)
{
    bedrock_test_activate_requested = value;
}

static void bedrock_test_set_construction_fault(Object *obj,
                                                 const char *value,
                                                 Error **errp)
{
    if (!strcmp(value, "ip53")) {
        bedrock_test_install_ip53_descriptor(false);
    } else if (!strcmp(value, "ip53-two-pic-hosts")) {
        static SGIModelPciHostBridge hosts[2];
        static SGIModelXioAttachment attachments[2];

        bedrock_test_install_ip53_descriptor(true);
        hosts[0] = ip53_test_pic;
        hosts[0].id = (char *)"pic-xio-b";
        hosts[1] = ip53_test_pic_second;
        attachments[0] = ip53_test_xio;
        attachments[0].widget = 11;
        attachments[1] = ip53_test_xio_second;
        bedrock_test_topology.pci_host_bridges = pci_host_bridge_list(
            hosts, ARRAY_SIZE(hosts));
        bedrock_test_topology.xio_attachments = xio_attachment_list(
            attachments, ARRAY_SIZE(attachments));
        ip53_test_io9.populated = true;
        ip53_test_io9.has_ds1742 = false;
        ip53_test_io9.ds1742 = false;
        ip53_test_io9.pci_host_index = 1;
    } else if (!strcmp(value, "ip53-pic-io9") ||
               !strcmp(value, "ip53-pic-io9-d1-h0")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_ioc4.pci_device = 1;
        ip53_test_io9.pci_head = 0;
    } else if (!strcmp(value, "ip53-asymmetric") ||
               !strcmp(value, "ip53-asymmetric-swapped") ||
               !strcmp(value, "ip53-asymmetric-overflow") ||
               !strcmp(value, "ip53-asymmetric-mixed") ||
               !strcmp(value, "ip53-asymmetric-accepted-mismatch")) {
        bedrock_test_install_ip53_memory_descriptor(
            !strcmp(value, "ip53-asymmetric-swapped"));
        if (!strcmp(value, "ip53-asymmetric-overflow")) {
            ip53_memory_test_bank_mib[0][2] = UINT64_MAX;
            ip53_memory_test_bank_mib[0][3] = 1;
            ip53_memory_test_bank_lists[0]->next->next->value = UINT64_MAX;
            ip53_memory_test_bank_lists[0]->next->next->next->value = 1;
        } else if (!strcmp(value, "ip53-asymmetric-mixed")) {
            ip53_memory_test_nodes[1].asic_kind = SGI_MODEL_ASIC_KIND_HUB;
        } else if (!strcmp(value, "ip53-asymmetric-accepted-mismatch")) {
            ip53_memory_test_nodes[1].accepted_memory_mib = uint64_list(
                ip53_memory_test_accepted_mib,
                ARRAY_SIZE(ip53_memory_test_accepted_mib));
        }
    } else if (!strcmp(value, "asymmetric-no-ip53")) {
        for (unsigned int i = 0; i < ARRAY_SIZE(bedrock_test_nodes); i++) {
            if (!bedrock_unfingerprinted_bank_lists[i]) {
                bedrock_unfingerprinted_bank_lists[i] = uint64_list(
                    bedrock_unfingerprinted_bank_mib[i],
                    ARRAY_SIZE(bedrock_unfingerprinted_bank_mib[i]));
            }
            bedrock_test_nodes[i].bank_mib =
                bedrock_unfingerprinted_bank_lists[i];
        }
    } else if (!strcmp(value, "ip53-pic-io9-ioc4-input0")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_ioc4.pci_device = 1;
        ip53_test_io9.pci_head = 0;
        ip53_test_ioc4.pic_input = 0;
    } else if (!strcmp(value, "ip53-pic-io9-d4-h0")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_ioc4.pci_device = 4;
        ip53_test_io9.pci_head = 0;
    } else if (!strcmp(value, "ip53-pic-io9-d1-h1")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_ioc4.pci_device = 1;
        ip53_test_io9.pci_head = 1;
    } else if (!strcmp(value, "ip53-pic-io9-d4-h1")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_ioc4.pci_device = 4;
        ip53_test_io9.pci_head = 1;
    } else if (!strcmp(value, "ip53-slot0")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_ioc4.pci_device = 0;
    } else if (!strcmp(value, "ip53-slot5")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_ioc4.pci_device = 5;
    } else if (!strcmp(value, "ip53-slot31")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_ioc4.pci_device = 31;
    } else if (!strcmp(value, "ip53-intxb")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_ioc4.intx_pin = SGI_MODEL_PCI_INTX_PIN_INTB;
    } else if (!strcmp(value, "ip53-alt-clock")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_identity.clock_hz = 150000000;
    } else if (!strcmp(value, "ip53-alt-cpu-reset")) {
        bedrock_test_install_ip53_descriptor(true);
        bedrock_test_processor.mode_bits = 1;
    } else if (!strcmp(value, "ip53-alt-cpu-cache")) {
        bedrock_test_install_ip53_descriptor(true);
        bedrock_test_processor.cache_mib = 4;
    } else if (!strcmp(value, "ip53-alt-bank")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_bank_mib[2] = 0;
        ip53_test_bank_mib[3] = 64;
        ip53_test_bank_mib_list->next->next->value = 0;
        ip53_test_bank_mib_list->next->next->next->value = 64;
        ip53_test_bedrock_config.installed_bank_mask = 0x08;
    } else if (!strcmp(value, "ip53-alt-board-id")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_bedrock_config.board_id_word = 0x2001;
    } else if (!strcmp(value, "ip53-alt-l1-type")) {
        bedrock_test_install_ip53_descriptor(true);
        bedrock_test_l1.reported_type = 0x35;
    } else if (!strcmp(value, "ip53-alt-product")) {
        bedrock_test_install_ip53_descriptor(true);
        bedrock_test_topology.product = (char *)"tezro";
    } else if (!strcmp(value, "ip53-ni3-widget2")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_identity.ni_revision = 3;
    } else if (!strcmp(value, "ip53-ni2-widget3")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_identity.widget_revision = 3;
    } else if (!strcmp(value, "ip53-missing-profile")) {
        bedrock_test_install_ip53_descriptor(false);
    } else if (!strcmp(value, "ip53-extra-attachment")) {
        bedrock_test_install_ip53_descriptor(true);
        ip53_test_attachment_entry.next = &ip53_test_unowned_attachment_entry;
    } else if (!strcmp(value, "ip53-extra-crossbar")) {
        bedrock_test_install_ip53_descriptor(true);
        /* The population view retains Crossbows reached by an attachment. */
        ip53_test_unowned_xio.crossbar_index = 0;
        ip53_test_attachment_entry.next =
            &ip53_test_unowned_attachment_entry;
        ip53_test_crossbar_entry.next = NULL;
        bedrock_test_topology.crossbars = &ip53_test_crossbar_entry;
    } else {
        error_setg(errp, "unknown sn-builder-bedrock-test construction "
                   "fault '%s'", value);
        return;
    }
    bedrock_test_activate_requested = true;
}

static void bedrock_test_machine_init(MachineState *machine)
{
    if (!bedrock_test_activate_requested) {
        return;
    }
    sgi_machine_init(machine);
}

static void bedrock_test_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);

    mc->desc = "two-node Bedrock topology fixture";
    mc->init = bedrock_test_machine_init;
    mc->valid_cpu_types = bedrock_test_r14000_cpu_types;
    mc->max_cpus = 2;
    mc->default_ram_id = "sn-builder-bedrock-test.ram";
    smc->test_topology = &bedrock_test_topology;
    object_class_property_add_bool(oc, "activate", NULL,
                                   bedrock_test_set_activate);
    object_class_property_set_description(oc, "activate",
        "Build the fixture's two nodes through the real builder. Off by "
        "default so qom-test and test-hmp's blind run over every "
        "registered machine type leaves this inert; the qtest that wants "
        "a real machine turns it on.");
    object_class_property_add_str(oc, "construction-fault", NULL,
                                  bedrock_test_set_construction_fault);
    object_class_property_set_description(oc, "construction-fault",
        "Select IP53 in a test-only descriptor and run the real builder.");
}

static const TypeInfo bedrock_test_type = {
    .name = TYPE_SN_BUILDER_BEDROCK_TEST,
    .parent = TYPE_SGI_SN_MACHINE,
    .class_init = bedrock_test_class_init,
};

/*
 * A second Bedrock fixture, bypassing sgi_machine_init entirely: the
 * machine-level refusal above is unconditional (no topology, however
 * addressed, may realize two Bedrock nodes together yet), so nothing fed
 * through the real builder can ever reach two realized nodes to prove their
 * address-space windows actually land clear of each other. This fixture
 * repeats the family builder's own per-node property set for a Bedrock node
 * (hw/mips/sgi/sn-machine.c's SN1 node loop) directly for nasid 0 and nasid
 * 1, reusing bedrock_test_nodes' two descriptor records, then realizes both
 * with the builder's sysbus_realize call. This path is reached here
 * without its node-count check. Each node gets its own RAM region:
 * sn1-node.c maps a node's RAM as a subregion of the global address space at
 * its own nasid-strided address, and one MemoryRegion object cannot be a
 * subregion of two different (offset, container) placements, so sharing
 * machine->ram the way today's single realized node does would abort the
 * second add_subregion call. Neither node's I/O board is built: this fixture
 * checks addressing only, so each carries just its own local XIO view
 * (widget zero), the one widget every Bedrock always decodes.
 */
#define TYPE_SN_BUILDER_BEDROCK_NASID_TEST \
    MACHINE_TYPE_NAME("sn-builder-bedrock-nasid-test")

static bool bedrock_nasid_test_activate_requested;
static bool bedrock_nasid_test_bad_revision_requested;
static char *bedrock_nasid_test_realize_error;
static SN1NodeState *bedrock_nasid_test_node[ARRAY_SIZE(bedrock_test_nodes)];

static char *bedrock_nasid_test_get_realize_error(Object *obj, Error **errp)
{
    return g_strdup(bedrock_nasid_test_realize_error ?: "");
}

static void bedrock_nasid_test_set_activate(Object *obj, bool value,
                                            Error **errp)
{
    bedrock_nasid_test_activate_requested = value;
}

static void bedrock_nasid_test_set_construction_fault(Object *obj,
                                                       const char *value,
                                                       Error **errp)
{
    if (!strcmp(value, "bedrock-revision")) {
        bedrock_nasid_test_bad_revision_requested = true;
    } else if (!strcmp(value, "ip53")) {
        bedrock_test_nodes[0].has_board_kind = true;
        bedrock_test_nodes[0].board_kind = SGI_MODEL_BOARD_KIND_IP53;
    } else {
        error_setg(errp, "unknown sn-builder-bedrock-nasid-test construction "
                   "fault '%s'", value);
        return;
    }
    bedrock_nasid_test_activate_requested = true;
}

static void bedrock_nasid_test_machine_init(MachineState *machine)
{
    Clock *cpu_refclk;
    unsigned int i;

    if (!bedrock_nasid_test_activate_requested) {
        return;
    }

    cpu_refclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpu_refclk, bedrock_test_processor.clock_hz);

    for (i = 0; i < ARRAY_SIZE(bedrock_test_nodes); i++) {
        g_autofree char *name = g_strdup_printf("node%u", i);
        g_autofree char *ram_name =
            g_strdup_printf("sn-builder-bedrock-nasid-test.node%u.ram", i);
        Object *obj = object_new(TYPE_SGI_SN1_NODE);
        SN1NodeState *node = SN1_NODE(obj);
        MemoryRegion *ram = g_new0(MemoryRegion, 1);

        /*
         * memory_region_init_ram needs a device owner for migration state;
         * the machine is not a device, so use the node.
         */
        memory_region_init_ram(ram, OBJECT(node), ram_name, 64 * MiB,
                               &error_fatal);

        object_property_add_child(OBJECT(machine), name, obj);
        object_unref(obj);

        qdev_prop_set_uint32(DEVICE(node), "nasid", i);
        node->model_node = &bedrock_test_nodes[i];
        qdev_prop_set_uint64(DEVICE(node), "board-id-word", 0x8001);
        qdev_prop_set_uint32(DEVICE(node), "l1-type-code", 0x34);
        qdev_prop_set_int32(DEVICE(node), "l1-revision-major", 1);
        qdev_prop_set_int32(DEVICE(node), "l1-revision-minor", 18);
        qdev_prop_set_int32(DEVICE(node), "l1-revision-patch", 5);
        qdev_prop_set_uint8(DEVICE(node), "bedrock-revision",
                            i == 0 && bedrock_nasid_test_bad_revision_requested
                            ? 16 : 2);
        qdev_prop_set_uint8(DEVICE(node), "installed-bank-mask", 0x0c);
        node->xbridge_widgets = g_new0(unsigned int, 1);
        node->xbridge_widgets[0] = 0;
        node->xbridge_widget_count = 1;
        object_property_set_link(OBJECT(node), "ram", OBJECT(ram),
                                 &error_abort);
        qdev_prop_set_uint32(DEVICE(node), "nr-cpus", 1);
        qdev_connect_clock_in(DEVICE(node), "cpu-refclk", cpu_refclk);

        if (bedrock_nasid_test_bad_revision_requested && i == 0) {
            Error *local_err = NULL;

            sysbus_realize(SYS_BUS_DEVICE(node), &local_err);
            bedrock_nasid_test_realize_error = local_err
                ? g_strdup(error_get_pretty(local_err))
                : g_strdup("unexpected successful realization");
            error_free(local_err);
            object_unparent(OBJECT(node));
            return;
        }
        sysbus_realize(SYS_BUS_DEVICE(node), &error_fatal);
        bedrock_nasid_test_node[i] = node;
    }
}

static void bedrock_nasid_test_machine_reset(MachineState *machine,
                                             ResetType type)
{
    unsigned int i;

    /*
 * This fixture never populates a topology, so the inherited
 * sgi_machine_reset would fall back to its Hub-shaped path and reset devices
 * before CPUs, reversing what a Bedrock node's "start-powered-off"
 * CPUs need (sn1-node.h's sn1_node_full_reset comment). Reset each node
 * itself instead, exactly as sgi_machine_reset's own multi-node branch does
 * for a real Bedrock topology.
 */
    for (i = 0; i < ARRAY_SIZE(bedrock_nasid_test_node); i++) {
        if (bedrock_nasid_test_node[i]) {
            sn1_node_full_reset(bedrock_nasid_test_node[i], type);
        }
    }
}

/* Stop after nodes and a router exist, before cable construction. */
static void bedrock_nasid_test_stop_partial(Object *obj, bool value,
                                           Error **errp)
{
    DeviceState *nodes[ARRAY_SIZE(bedrock_nasid_test_node)];
    DeviceState *dev;
    SGISN1RouterState *router;
    SN1MemoryMappings *maps;

    if (!value) {
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (!bedrock_nasid_test_node[i]) {
            error_setg(errp, "partial mapping test needs activated nodes");
            return;
        }
        nodes[i] = DEVICE(bedrock_nasid_test_node[i]);
    }
    dev = qdev_new(TYPE_SGI_SN1_ROUTER);
    object_property_add_child(obj, "partial-router", OBJECT(dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_abort);
    router = SGI_SN1_ROUTER(dev);
    maps = sn1_memory_mappings_new(nodes, ARRAY_SIZE(nodes), &router, 1);
    object_unparent(OBJECT(router));
    sn1_memory_mappings_stop(maps);
    sn1_memory_mappings_stop(maps);
    g_free(maps);
}

static void bedrock_nasid_test_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "two-node Bedrock addressing fixture, bypassing the builder";
    object_class_property_add_bool(oc, "stop-partial-mappings", NULL,
                                   bedrock_nasid_test_stop_partial);
    mc->init = bedrock_nasid_test_machine_init;
    mc->reset = bedrock_nasid_test_machine_reset;
    mc->valid_cpu_types = bedrock_test_r14000_cpu_types;
    mc->max_cpus = 2;
    object_class_property_add_bool(oc, "activate", NULL,
                                   bedrock_nasid_test_set_activate);
    object_class_property_set_description(oc, "activate",
        "Build two sgi-sn1-node devices directly, at nasid 0 and 1, "
        "skipping sgi_machine_init and its node-count refusal entirely. "
        "Off by default so qom-test and test-hmp's blind run over every "
        "registered machine type leaves this inert; the qtest that wants "
        "a real machine turns it on.");
    object_class_property_add_str(oc, "construction-fault", NULL,
                                  bedrock_nasid_test_set_construction_fault);
    object_class_property_set_description(oc, "construction-fault",
        "Select a test construction fault for node 0.");
    object_class_property_add_str(oc, "realize-error",
                                  bedrock_nasid_test_get_realize_error, NULL);
}

static const TypeInfo bedrock_nasid_test_type = {
    .name = TYPE_SN_BUILDER_BEDROCK_NASID_TEST,
    .parent = TYPE_SGI_SN_MACHINE,
    .class_init = bedrock_nasid_test_class_init,
};

/* Clone catalogue ownership; change only these synthetic identity values. */
static SGIModelTopology origin300_config_topology;
static SGIModelNode origin300_config_node;
static SGIModelNodeList origin300_config_nodes;
static SGIModelNodePopulation origin300_config_population;
static SGIModelNodePopulationList origin300_config_populations;
static SGIModelAsicIdentity origin300_config_identity;
static SGIModelBedrockConfig origin300_config_bedrock;
static SGIModelController origin300_config_controller;
static SGIModelControllerList origin300_config_controllers;
static SGIModelL1Config origin300_config_l1;
static SGIModelL1Revision origin300_config_revision = {
    .major = 7, .minor = 8, .patch = 9,
};
static SGIModelController origin300_config_unused_controller = {
    .id = (char *)"unused-l1", .kind = SGI_MODEL_CONTROLLER_KIND_L1,
    .module_index = 0,
};
static SGIModelControllerList origin300_config_unused_controllers = {
    .value = &origin300_config_unused_controller,
};
static bool origin300_config_unused_l1;
static bool origin300_config_activate;
static void (*origin300_config_parent_init)(MachineState *machine);

static void origin300_config_set_activate(Object *obj, bool value, Error **errp)
{
    origin300_config_activate = value;
}

static void origin300_config_set_unused_l1(Object *obj, bool value,
                                           Error **errp)
{
    origin300_config_unused_l1 = value;
}

static void origin300_config_init(MachineState *machine)
{
    if (origin300_config_activate) {
        if (origin300_config_unused_l1) {
            origin300_config_unused_controller.l1 = &origin300_config_l1;
            origin300_config_controllers.next =
                &origin300_config_unused_controllers;
        }
        origin300_config_parent_init(machine);
    }
}

static void origin300_config_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const SGIModelTopology *source = sgi_model_find_topology("origin300");

    origin300_config_topology = *source;
    origin300_config_topology.name = (char *)"sn-builder-origin300-config-test";
    origin300_config_node = *source->nodes->value;
    origin300_config_identity = *origin300_config_node.identity;
    origin300_config_identity.widget_revision = 9;
    origin300_config_node.identity = &origin300_config_identity;
    origin300_config_bedrock = *origin300_config_node.bedrock;
    origin300_config_bedrock.board_id_word = 0x8abc;
    origin300_config_node.bedrock = &origin300_config_bedrock;
    origin300_config_nodes.value = &origin300_config_node;
    origin300_config_topology.nodes = &origin300_config_nodes;
    /* Population views are cached by row address, so own this row too. */
    g_assert_null(source->node_populations->next);
    origin300_config_population = *source->node_populations->value;
    origin300_config_populations.value = &origin300_config_population;
    origin300_config_topology.node_populations = &origin300_config_populations;
    origin300_config_controller = *source->controllers->value;
    origin300_config_l1 = *origin300_config_controller.l1;
    origin300_config_l1.reported_type = 0x41;
    origin300_config_l1.revision = &origin300_config_revision;
    origin300_config_controller.l1 = &origin300_config_l1;
    origin300_config_controllers.value = &origin300_config_controller;
    origin300_config_topology.controllers = &origin300_config_controllers;
    smc->test_topology = &origin300_config_topology;
    origin300_config_parent_init = mc->init;
    mc->init = origin300_config_init;
    mc->desc = "synthetic Origin 300 identity configuration";
    object_class_property_add_bool(oc, "activate", NULL,
                                   origin300_config_set_activate);
    object_class_property_add_bool(oc, "unused-l1", NULL,
                                   origin300_config_set_unused_l1);
}

static const TypeInfo origin300_config_type = {
    .name = MACHINE_TYPE_NAME("sn-builder-origin300-config-test"),
    .parent = MACHINE_TYPE_NAME("origin300"),
    .class_init = origin300_config_class_init,
};

#define TYPE_SN_BUILDER_EXPANSION_TEST \
    MACHINE_TYPE_NAME("sn-builder-expansion-test")

static bool expansion_test_activate;

static void expansion_test_set_activate(Object *obj, bool value, Error **errp)
{
    expansion_test_activate = value;
}

static void expansion_test_set_identity_mismatch(Object *obj, bool value,
                                                  Error **errp)
{
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(obj);

    if (value) {
        smc->test_topology->l1_management->owners->value->identity += 17;
    }
}

static void expansion_test_set_attachment_mismatch(Object *obj, bool value,
                                                    Error **errp)
{
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(obj);

    if (value) {
        SGIModelL1Attachment *selection =
            smc->test_topology->l1_management->attachments->value;

        selection->issuer = 1;
        selection->owner = 0;
    }
}

static void expansion_test_set_ownership_mismatch(Object *obj, bool value,
                                                   Error **errp)
{
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(obj);
    SGIModelL1OwnerList *owners = smc->test_topology->l1_management->owners;

    if (value) {
        owners->value->controller_index = 1;
        owners->next->value->controller_index = 0;
    }
}

static void expansion_test_machine_init(MachineState *machine)
{
    if (expansion_test_activate) {
        sgi_machine_init(machine);
    }
}

static void expansion_test_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const SGIModelTopology *compute = sgi_model_find_topology("origin300");
    SGIModelTopology *t = g_memdup2(compute, sizeof(*t));
    SGIModelModule *module = g_new0(SGIModelModule, 1);
    SGIModelController *controller = g_new0(SGIModelController, 1);
    SGIModelIoPeer *peer = g_new0(SGIModelIoPeer, 1);
    SGIModelIoAssembly *assembly = g_new0(SGIModelIoAssembly, 1);
    SGIModelCable *cable = g_new0(SGIModelCable, 1);
    SGIModelNodePopulation *row =
        g_memdup2(compute->node_populations->value, sizeof(*row));
    SGIModelModuleList **module_tail = &t->modules;
    SGIModelControllerList **controller_tail = &t->controllers;
    SGIModelL1Management *management = g_new0(SGIModelL1Management, 1);
    uint8List **port_tail = &assembly->head_ports;
    static const uint8_t ports[] = { 8, 9, 12, 13 };
    static const unsigned zero[] = { 0 };

    t->name = (char *)"sn-builder-expansion-test";
    t->modules = NULL;
    t->controllers = NULL;
    QAPI_LIST_APPEND(module_tail, compute->modules->value);
    module->id = (char *)"synthetic-expansion";
    module->controller_index = 1;
    module->io_peers = uint32_list(zero, 1);
    QAPI_LIST_APPEND(module_tail, module);
    QAPI_LIST_APPEND(controller_tail, compute->controllers->value);
    controller->id = (char *)"synthetic-expansion-l1";
    controller->kind = SGI_MODEL_CONTROLLER_KIND_L1;
    controller->module_index = 1;
    QAPI_LIST_APPEND(controller_tail, controller);
    peer->id = (char *)"synthetic-bic";
    peer->module_index = 1;
    peer->asic_kind = SGI_MODEL_ASIC_KIND_BEDROCK;
    peer->identity = g_new0(SGIModelAsicIdentity, 1);
    peer->identity->ni_revision = peer->identity->widget_revision = 2;
    peer->identity->serial = 0x123456;
    t->has_io_peers = true;
    t->io_peers = g_new0(SGIModelIoPeerList, 1);
    t->io_peers->value = peer;
    assembly->version = 1;
    assembly->kind = SGI_MODEL_IO_ASSEMBLY_KIND_ORIGIN300_PCI_EXPANSION;
    assembly->module_index = assembly->controller_index = 1;
    assembly->placement = 1;
    assembly->initial_node_number = 0;
    assembly->bedrock_revision = assembly->xbridge_revision = 2;
    assembly->controller_profile = (char *)"pci-expansion";
    assembly->bic_identity = UINT64_C(0x123400000000c000);
    for (unsigned i = 0; i < ARRAY_SIZE(ports); i++) {
        QAPI_LIST_APPEND(port_tail, ports[i]);
    }
    t->has_io_assemblies = true;
    t->io_assemblies = g_new0(SGIModelIoAssemblyList, 1);
    t->io_assemblies->value = assembly;
    cable->name = (char *)"expansion-link";
    cable->kind = SGI_MODEL_CABLE_KIND_CABLE;
    cable->ends = g_new0(SGIModelLinkEndList, 1);
    cable->ends->value = g_new0(SGIModelLinkEnd, 1);
    cable->ends->value->kind = SGI_MODEL_LINK_END_KIND_NODE;
    cable->ends->next = g_new0(SGIModelLinkEndList, 1);
    cable->ends->next->value = g_new0(SGIModelLinkEnd, 1);
    cable->ends->next->value->kind = SGI_MODEL_LINK_END_KIND_IO_PEER;
    t->cables = g_new0(SGIModelCableList, 1);
    t->cables->value = cable;
    row->cables = uint32_list(zero, 1);
    t->node_populations = g_new0(SGIModelNodePopulationList, 1);
    t->node_populations->value = row;
    for (unsigned i = 0; i < 2; i++) {
        SGIModelL1OwnerList **owner_tail = &management->owners;
        SGIModelL1AssociationList **edge_tail = &management->associations;
        SGIModelL1Owner *owner = g_new0(SGIModelL1Owner, 1);
        SGIModelL1Association *edge = g_new0(SGIModelL1Association, 1);

        while (*owner_tail) {
            owner_tail = &(*owner_tail)->next;
        }
        while (*edge_tail) {
            edge_tail = &(*edge_tail)->next;
        }
        owner->controller_index = i;
        owner->identity = 101 + i;
        edge->from = i;
        edge->to = i ^ 1;
        edge->reachable = true;
        QAPI_LIST_APPEND(owner_tail, owner);
        QAPI_LIST_APPEND(edge_tail, edge);
    }
    management->attachments = g_new0(SGIModelL1AttachmentList, 1);
    management->attachments->value = g_new0(SGIModelL1Attachment, 1);
    management->attachments->value->owner = 1;
    t->l1_management = management;
    smc->test_topology = t;
    smc->product = "origin300";
    smc->default_topology = "origin300";
    mc->desc = "synthetic Origin 300 expansion builder fixture";
    mc->init = expansion_test_machine_init;
    mc->valid_cpu_types = bedrock_test_r14000_cpu_types;
    mc->default_cpus = sgi_machine_topology_default_cpus(compute);
    mc->min_cpus = mc->default_cpus;
    mc->max_cpus = sgi_machine_product_max_cpus("origin300");
    sgi_machine_class_set_default_ram(
        mc, sgi_machine_topology_default_ram(compute));
    mc->default_ram_id = "sn-builder-expansion-test.ram";
    object_class_property_add_bool(oc, "activate", NULL,
                                   expansion_test_set_activate);
    object_class_property_add_bool(oc, "identity-mismatch", NULL,
                                   expansion_test_set_identity_mismatch);
    object_class_property_add_bool(oc, "attachment-mismatch", NULL,
                                   expansion_test_set_attachment_mismatch);
    object_class_property_add_bool(oc, "ownership-mismatch", NULL,
                                   expansion_test_set_ownership_mismatch);
}

static const TypeInfo expansion_test_type = {
    .name = TYPE_SN_BUILDER_EXPANSION_TEST,
    .parent = TYPE_SGI_SN_MACHINE,
    .class_init = expansion_test_class_init,
};

/* QMP has no child-release command. Keep this operation test-only. */
static bool routed_cleanup_activate;
static void (*routed_cleanup_parent_init)(MachineState *machine);

static void routed_cleanup_set_activate(Object *obj, bool value, Error **errp)
{
    routed_cleanup_activate = value;
}

static void routed_cleanup_init(MachineState *machine)
{
    if (routed_cleanup_activate) {
        routed_cleanup_parent_init(machine);
    }
}

static void routed_cleanup_release_child(Object *obj, const char *value,
                                          Error **errp)
{
    Object *parent = g_str_equal(value, "router") ?
        object_resolve_path_component(obj, "module3") : obj;
    Object *child = parent ?
        object_resolve_path_component(parent, value) : NULL;

    if (!child || (!g_str_equal(value, "master-route") &&
                   !g_str_equal(value, "slave-route") &&
                   !g_str_equal(value, "expansion-cable") &&
                   !g_str_equal(value, "router"))) {
        error_setg(errp, "unsupported cleanup child '%s'", value);
        return;
    }
    object_unparent(child);
}

static void routed_cleanup_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    routed_cleanup_parent_init = mc->init;
    mc->init = routed_cleanup_init;
    object_class_property_add_bool(oc, "activate", NULL,
                                   routed_cleanup_set_activate);
    object_class_property_add_str(oc, "release-child", NULL,
                                  routed_cleanup_release_child);
}

static const TypeInfo routed_cleanup_type = {
    .name = MACHINE_TYPE_NAME("sn-builder-routed-cleanup-test"),
    .parent = MACHINE_TYPE_NAME("origin300"),
    .class_init = routed_cleanup_class_init,
};

/* Synthetic two-router chain; it makes no product-topology claim. */
static bool routed_memory_activate;
static void (*routed_memory_parent_init)(MachineState *machine);

static void routed_memory_set_activate(Object *obj, bool value, Error **errp)
{
    routed_memory_activate = value;
}

static void routed_memory_init(MachineState *machine)
{
    if (routed_memory_activate) {
        routed_memory_parent_init(machine);
    }
}

static void routed_memory_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const SGIModelTopology *source = sgi_model_find_topology(
        "origin300-numalink-pci-expansion-experiment");
    SGIModelTopology *t = g_memdup2(source, sizeof(*t));
    SGIModelNodeList **node_tail = &t->nodes;
    SGIModelModuleList **module_tail = &t->modules;
    SGIModelControllerList **controller_tail = &t->controllers;
    SGIModelRouterList **router_tail = &t->routers;
    SGIModelCableList **cable_tail = &t->cables;
    SGIModelNodePopulationList **population_tail = &t->node_populations;
    SGIModelL1Management *management =
        g_memdup2(source->l1_management, sizeof(*management));
    SGIModelL1OwnerList **owner_tail = &management->owners;
    SGIModelModule *module = g_new0(SGIModelModule, 1);
    SGIModelController *controller = g_new0(SGIModelController, 1);
    SGIModelRouter *router =
        g_memdup2(source->routers->value, sizeof(*router));
    SGIModelL1Owner *owner = g_new0(SGIModelL1Owner, 1);
    SGIModelCable *inter = g_new0(SGIModelCable, 1);
    SGIModelLinkEndList **end_tail = &inter->ends;
    static const unsigned routers[] = { 0, 1 };
    static const unsigned cables[] = { 0, 1, 2, 3 };

    t->name = (char *)"sn-builder-routed-memory-test";
    t->nodes = NULL;
    for (const SGIModelNodeList *entry = source->nodes;
         entry; entry = entry->next) {
        SGIModelNode *node = g_memdup2(entry->value, sizeof(*node));

        node->has_option_intx_map = false;
        node->option_intx_map = NULL;
        QAPI_LIST_APPEND(node_tail, node);
    }
    t->modules = NULL;
    t->controllers = NULL;
    t->routers = NULL;
    t->cables = NULL;
    t->node_populations = NULL;
    management->owners = NULL;
    for (const SGIModelModuleList *entry = source->modules;
         entry; entry = entry->next) {
        QAPI_LIST_APPEND(module_tail, entry->value);
    }
    module->id = (char *)"synthetic-interior-router";
    module->initial_module_number = 21;
    module->controller_index = 4;
    QAPI_LIST_APPEND(module_tail, module);
    for (const SGIModelControllerList *entry = source->controllers;
         entry; entry = entry->next) {
        QAPI_LIST_APPEND(controller_tail, entry->value);
    }
    controller->id = (char *)"synthetic-interior-router-l1";
    controller->module_index = 4;
    controller->kind = SGI_MODEL_CONTROLLER_KIND_L1;
    QAPI_LIST_APPEND(controller_tail, controller);
    for (const SGIModelRouterList *entry = source->routers;
         entry; entry = entry->next) {
        QAPI_LIST_APPEND(router_tail, entry->value);
    }
    router->id = (char *)"synthetic-interior-router-chip";
    router->module_index = 4;
    router->serial = 0x12345678;
    QAPI_LIST_APPEND(router_tail, router);
    for (const SGIModelCableList *entry = source->cables;
         entry; entry = entry->next) {
        SGIModelCable *cable = g_memdup2(entry->value, sizeof(*cable));
        SGIModelLinkEndList **tail = &cable->ends;

        cable->ends = NULL;
        for (const SGIModelLinkEndList *end = entry->value->ends;
             end; end = end->next) {
            SGIModelLinkEnd *endpoint =
                g_memdup2(end->value, sizeof(*endpoint));

            if (g_str_equal(cable->name, "slave-route") &&
                endpoint->kind == SGI_MODEL_LINK_END_KIND_ROUTER) {
                endpoint->index = 1;
            }
            QAPI_LIST_APPEND(tail, endpoint);
        }
        QAPI_LIST_APPEND(cable_tail, cable);
    }
    inter->name = (char *)"inter-router";
    inter->kind = SGI_MODEL_CABLE_KIND_CABLE;
    for (unsigned i = 0; i < 2; i++) {
        SGIModelLinkEnd *endpoint = g_new0(SGIModelLinkEnd, 1);

        endpoint->kind = SGI_MODEL_LINK_END_KIND_ROUTER;
        endpoint->index = i;
        endpoint->port = i ? 1 : 6;
        QAPI_LIST_APPEND(end_tail, endpoint);
    }
    QAPI_LIST_APPEND(cable_tail, inter);
    for (const SGIModelNodePopulationList *entry = source->node_populations;
         entry; entry = entry->next) {
        SGIModelNodePopulation *population =
            g_memdup2(entry->value, sizeof(*population));

        population->routers = uint32_list(routers, ARRAY_SIZE(routers));
        population->cables = uint32_list(cables, ARRAY_SIZE(cables));
        QAPI_LIST_APPEND(population_tail, population);
    }
    for (const SGIModelL1OwnerList *entry = source->l1_management->owners;
         entry; entry = entry->next) {
        QAPI_LIST_APPEND(owner_tail, entry->value);
    }
    owner->controller_index = 4;
    owner->identity = 0x55;
    QAPI_LIST_APPEND(owner_tail, owner);
    t->l1_management = management;
    smc->test_topology = t;
    routed_memory_parent_init = mc->init;
    mc->init = routed_memory_init;
    mc->desc = "Synthetic routed memory two-router chain";
    machine_class_allow_dynamic_sysbus_dev(mc, "sgi-board-test-fixture");
    object_class_property_add_bool(oc, "activate", NULL,
                                   routed_memory_set_activate);
}

static const TypeInfo routed_memory_type = {
    .name = MACHINE_TYPE_NAME("sn-builder-routed-memory-test"),
    .parent = MACHINE_TYPE_NAME("origin300"),
    .class_init = routed_memory_class_init,
};

/*
 * Keep the older notification fixture's cables while testing the public
 * two-router route on its specified ports.
 */
static void routed_memory_public_class_init(ObjectClass *oc, const void *data)
{
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const SGIModelTopology *source = smc->test_topology;
    SGIModelTopology *t = g_memdup2(source, sizeof(*t));
    SGIModelCableList **cable_tail = &t->cables;

    t->name = (char *)"sn-builder-routed-memory-public-test";
    t->cables = NULL;
    for (const SGIModelCableList *entry = source->cables;
         entry; entry = entry->next) {
        SGIModelCable *cable = g_memdup2(entry->value, sizeof(*cable));
        SGIModelLinkEndList **end_tail = &cable->ends;

        cable->ends = NULL;
        for (const SGIModelLinkEndList *end = entry->value->ends;
             end; end = end->next) {
            SGIModelLinkEnd *endpoint =
                g_memdup2(end->value, sizeof(*endpoint));

            if (endpoint->kind == SGI_MODEL_LINK_END_KIND_ROUTER) {
                if (g_str_equal(cable->name, "master-route")) {
                    endpoint->port = 2;
                } else if (g_str_equal(cable->name, "slave-route")) {
                    endpoint->port = 3;
                } else if (g_str_equal(cable->name, "inter-router")) {
                    endpoint->port = endpoint->index ? 7 : 5;
                }
            }
            QAPI_LIST_APPEND(end_tail, endpoint);
        }
        QAPI_LIST_APPEND(cable_tail, cable);
    }
    smc->test_topology = t;
}

static const TypeInfo routed_memory_public_type = {
    .name = MACHINE_TYPE_NAME("sn-builder-routed-memory-public-test"),
    .parent = MACHINE_TYPE_NAME("sn-builder-routed-memory-test"),
    .class_init = routed_memory_public_class_init,
};

static void sn_builder_fault_test_register_types(void)
{
    /*
 * Wire the QAPI list fields both fixture topologies need before either type
 * registers: class_init (which reads test_topology) only ever runs later, on
 * first use, but building the lists here keeps every array-to-list
 * conversion in one place instead of split across two type_init functions.
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
    console_test_topology.xio_attachments =
        xio_attachment_list(console_test_xio, ARRAY_SIZE(console_test_xio));
    console_test_topology.io_boards =
        io_board_list(console_test_io_boards, ARRAY_SIZE(console_test_io_boards));
    /*
     * nodes=1 fits node 0 alone and leaves out node 1, whose own board
     * and console the fixture carries: a row the builder cannot make,
     * whatever its grade.
     */
    console_test_topology.node_populations = node0_row(whole_population(
        &console_test_topology,
        cpu_population_list(console_test_populations,
                            ARRAY_SIZE(console_test_populations))),
        SGI_MODEL_EVIDENCE_GRADE_ESTABLISHED);

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
    bank_test_topology.xio_attachments =
        xio_attachment_list(console_test_xio, 1);
    bank_test_topology.io_boards =
        io_board_list(console_test_io_boards, 1);
    bank_test_topology.node_populations = whole_population(
        &bank_test_topology,
        cpu_population_list(bank_test_populations,
                            ARRAY_SIZE(bank_test_populations)));

    bedrock_test_nodes[0].cpu_slots = bedrock_test_nodes[1].cpu_slots =
        cpu_slot_list(bedrock_test_cpu_slots,
                     ARRAY_SIZE(bedrock_test_cpu_slots));
    bedrock_test_nodes[0].bank_mib = bedrock_test_nodes[1].bank_mib =
        uint64_list(bedrock_test_bank_mib, ARRAY_SIZE(bedrock_test_bank_mib));

    bedrock_test_populations[0].cpus_per_node =
        uint32_list(bedrock_test_population_0,
                   ARRAY_SIZE(bedrock_test_population_0));

    bedrock_test_modules[0].nodes =
        uint32_list(console_test_m0_nodes, ARRAY_SIZE(console_test_m0_nodes));
    bedrock_test_modules[1].nodes =
        uint32_list(console_test_m1_nodes, ARRAY_SIZE(console_test_m1_nodes));
    bedrock_test_topology.modules =
        module_list(bedrock_test_modules, ARRAY_SIZE(bedrock_test_modules));
    {
        SGIModelControllerList **tail = &bedrock_test_topology.controllers;
        SGIModelPciFunctionList *functions = NULL;
        SGIModelPciFunctionList **function_tail = &functions;

        for (unsigned int i = 0;
             i < ARRAY_SIZE(bedrock_test_controllers); i++) {
            QAPI_LIST_APPEND(tail, &bedrock_test_controllers[i]);
        }
        fixture_remember(bedrock_test_topology.controllers);
        for (unsigned int i = 0;
             i < ARRAY_SIZE(bedrock_test_pci_functions); i++) {
            QAPI_LIST_APPEND(function_tail, &bedrock_test_pci_functions[i]);
        }
        for (unsigned int i = 0; i < ARRAY_SIZE(bedrock_test_io_boards); i++) {
            bedrock_test_io_boards[i].pci_functions = functions;
        }
    }

    bedrock_test_topology.nodes =
        node_list(bedrock_test_nodes, ARRAY_SIZE(bedrock_test_nodes));
    bedrock_test_topology.xio_attachments =
        xio_attachment_list(bedrock_test_xio,
                            ARRAY_SIZE(bedrock_test_xio));
    bedrock_test_topology.io_boards =
        io_board_list(bedrock_test_io_boards,
                     ARRAY_SIZE(bedrock_test_io_boards));
    /* nodes=1 is a row whose evidence does not settle it. */
    bedrock_test_topology.node_populations = node0_row(whole_population(
        &bedrock_test_topology,
        cpu_population_list(bedrock_test_populations,
                            ARRAY_SIZE(bedrock_test_populations))),
        SGI_MODEL_EVIDENCE_GRADE_UNKNOWN);

    type_register_static(&console_test_type);
    type_register_static(&bank_test_type);
    type_register_static(&bedrock_test_type);
    type_register_static(&bedrock_nasid_test_type);
    type_register_static(&origin300_config_type);
    type_register_static(&expansion_test_type);
    type_register_static(&routed_cleanup_type);
    type_register_static(&routed_memory_type);
    type_register_static(&routed_memory_public_type);
}

type_init(sn_builder_fault_test_register_types)
