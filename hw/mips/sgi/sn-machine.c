/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The abstract SGI Scalable Node family machine and its builder over a
 * format-2 SGIModelTopology (sn-model.h). Origin 200 is the first
 * product on this base; see docs/plans/sn-machine-model.md, "QEMU".
 *
 * Fixed here rather than carried in the descriptor, because they are Hub
 * hardware facts rather than per-configuration data: the Hub's physical
 * address map, its memory-bank geometry, its three hardwired CPU
 * interrupt inputs, and the PROM PI-error probe's Bridge offset.
 *
 * Origin 200's onboard board (sgi-ip29-io) sits directly on its one
 * node's crosstalk port; the Origin 2000 module's onboard board
 * (sgi-baseio) sits behind a Crossbow this builder also owns. Both are
 * sgi-xtalk-board instances built and realized here, before the nodes'
 * Crossbows realize. sgi-pci-expansion (board-pci-expansion.c) is not
 * built here or by anything else yet: see that class's own header
 * comment for why it stays onboard-only, not `-device`-creatable, until
 * a machine-owned spare fabric proxy exists for it to attach through.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/host-utils.h"
#include "qemu/datadir.h"
#include "qapi/error.h"
#include "qapi/qapi-builtin-types.h"
#include "qapi/qapi-visit-common.h"
#include "qapi/visitor.h"
#include "qobject/qlist.h"
#include "qemu/error-report.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi-xtalk-port.h"
#include "hw/mips/sgi/xtalk-board.h"
#include "hw/mips/sgi/xtalk-link.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/misc/sgi-hub.h"
#include "hw/misc/sgi-router.h"
#include "hw/misc/sgi-sn0-module-management.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/pci-host/sgi-bridge.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/system.h"
#include "trace/control.h"
#include "cpu.h"
#include "hw/mips/sgi/board-io8.h"
#include "hw/mips/sgi/sn-machine.h"
#include "hw/mips/sgi/sn1-node.h"
#include "hw/mips/sn0-fabric-routed.h"
#include "sn0.h"

/*
 * The board classes have no shared header; each names its own QOM type,
 * except sgi-io8 (board-io8.h): Origin 300's Bedrock needs its own
 * per-widget views, which the base class's one-view-per-board contract
 * does not carry.
 */
#define TYPE_SGI_IP29_IO "sgi-ip29-io"
#define TYPE_SGI_BASEIO "sgi-baseio"

/* The Hub's flash controller offers this view whenever the board wires it. */
static const SN0ModelRegion sgi_sn0_hub_lboot = {
    .offset = 0x10000000ULL,
    .size = 0x10000000ULL,
};

/* The Hub's physical address map: identical on every SN0 node measured. */
static const SN0ModelAddressMap sgi_sn0_hub_address_map = {
    .node_stride = 0x100000000ULL,
    .memory = {
        .selector = 3,
        .carrier_base = 0x0ULL,
        .unmapped_base = 0x9600000000000000ULL,
    },
    .hub_special = {
        .selector = 0,
        .carrier_base = 0x10000000000ULL,
        .unmapped_base = 0x9000000000000000ULL,
    },
    .xtalk = {
        .selector = 1,
        .carrier_base = 0x20000000000ULL,
        .unmapped_base = 0x9200000000000000ULL,
    },
    .memory_special = {
        .selector = 2,
        .carrier_base = 0x30000000000ULL,
        .unmapped_base = 0x9400000000000000ULL,
    },
    .node_memory = { .offset = 0x0ULL, .size = 0x100000000ULL },
    .hub_registers_local = { .offset = 0x1000000ULL, .size = 0x800000ULL },
    .hub_registers_node = { .offset = 0x1800000ULL, .size = 0x800000ULL },
    .hub_local_memory = { .offset = 0x0ULL, .size = 0x10000000ULL },
    .hub_remote_memory = { .offset = 0x20000000ULL, .size = 0x10000000ULL },
    .backdoor_ecc = { .offset = 0x80000000ULL, .size = 0x40000000ULL },
    .backdoor_directory = { .offset = 0xc0000000ULL, .size = 0x40000000ULL },
    .flash_reset = { .offset = 0x1fc00000ULL, .size = 0x100000ULL },
    .lboot = &sgi_sn0_hub_lboot,
    .rboot = { .offset = 0x30000000ULL, .size = 0x10000000ULL },
    .bridge_registers = { .offset = 0x8000000ULL, .size = 0x1000000ULL },
};

/* The Hub's memory-bank geometry: also identical across every SN0 node. */
static const uint64_t sgi_sn0_hub_memory_sizes_mib[] = {
    0x200ULL, 0x100ULL, 0x80ULL, 0x40ULL,
};

static const SN0ModelHardware sgi_sn0_hub_hardware = {
    .cpu_kind = SN0_CPU_R10000,
    .cpu_slots_per_node = 2,
    .hub_clock_hz = 0x55d4a80ULL,
    .rtc_clock_hz = 0x1312d0ULL,
    .memory = {
        .physical_banks = 4,
        .architectural_banks = 8,
        .supported_mib = sgi_sn0_hub_memory_sizes_mib,
        .supported_size_count = ARRAY_SIZE(sgi_sn0_hub_memory_sizes_mib),
        .first_bank_max_mib = 0x100ULL,
        .bank_stride_mib = 0x200ULL,
        .interleave_slot_stride_mib = 0x80ULL,
        .interleave_slots_per_bank = 4,
    },
};

/* The Hub's three CPU interrupt inputs: hardwired, not configuration. */
#define SGI_SN0_HUB_CPU_IRQ_BANK0 0
#define SGI_SN0_HUB_CPU_IRQ_BANK1 1
#define SGI_SN0_HUB_CPU_IRQ_RT    2

/*
 * The PROM PI-error probe's fixed target: widget 2 (a synthetic address,
 * not a populated widget) at the Bridge's own device-window offset for
 * its error-status decode. Origin 2000 does not use this probe at all.
 */
#define SGI_SN0_PROM_PI_PROBE_WIDGET        2
#define SGI_SN0_PROM_PI_PROBE_OFFSET        0x400208ULL

struct SGIMachineState {
    MachineState parent_obj;

    char *topology_name;

    unsigned int num_nodes;
    SN0NodeState **nodes;
    SN0ModelPhysicalNode *physical_nodes;  /* Read by sn0.c's node builder. */
    SN0ModelComponent *hub_components;
    MemoryRegion *big_window_route;

    SN0FabricState *fabric;
    Clock *cpu_refclk;
    Clock *rt_refclk;

    /* Parented and realized as soon as every port attached to it is. */
    SGIXbowState **crossbars;

    /*
     * One sgi-xtalk-board per io_boards[] entry, built before its own
     * Crossbow, if any.
     */
    SGIXtalkBoardState **io_boards;

    /*
     * A directly attached board's own crosstalk wiring: one link per
     * io_boards[] entry that attaches without a Crossbow (Origin 200's
     * onboard I/O). Unused entries (a Crossbow-attached board's own)
     * stay zeroed.
     */
    SGIXtalkDirectLink *direct_links;

    /*
     * A Crossbow-attached endpoint's own link: one per node, then one
     * per io_boards[] entry, in that order (index node_count + board
     * index for a board's own). Unused entries (a direct board's, or a
     * Hub-product topology's io board on a Bedrock node) stay zeroed.
     */
    SGIXtalkCrossbowLink *crossbow_links;

    bool debug_leds;
    bool junk_uart_present;
    uint8_t elsc_dip_switches;

    /* The Bedrock node, for a topology whose node kind is Bedrock. */
    DeviceState *bedrock_node;

    /*
     * The Origin 300/350 L1's identity-record file lists. One machine
     * property set regardless of product (per-kind properties split
     * across classes is exactly what the family base replaces); refused
     * at init if the topology carries no L1 controller to hand them to.
     */
    strList *chassis_eeprom;
    strList *board_eeprom;
    strList *spd_eeprom;
};

/*
 * The controller holds one identification record per file, in order. The
 * machine names the files and claims nothing about what is in them: no
 * default record, and no record implied by the memory size.
 */
typedef struct SGIMachineIdentityRecords {
    const char *name;
    ptrdiff_t offset;
    const char *description;
} SGIMachineIdentityRecords;

static const SGIMachineIdentityRecords sgi_machine_identity_records[] = {
    {
        "chassis-eeprom", offsetof(SGIMachineState, chassis_eeprom),
        "Files holding the chassis identification records, in order",
    },
    {
        "board-eeprom", offsetof(SGIMachineState, board_eeprom),
        "Files holding the board identification records, in order",
    },
    {
        "spd-eeprom", offsetof(SGIMachineState, spd_eeprom),
        "Files holding the memory module presence-detect records, in order",
    },
};

static strList **sgi_machine_identity_slot(Object *obj, void *opaque)
{
    const SGIMachineIdentityRecords *records = opaque;

    return (strList **)((char *)SGI_MACHINE(obj) + records->offset);
}

static void sgi_machine_get_identity_records(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    visit_type_strList(v, name, sgi_machine_identity_slot(obj, opaque), errp);
}

static void sgi_machine_set_identity_records(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    strList **slot = sgi_machine_identity_slot(obj, opaque);
    strList *replacement = NULL;

    if (!visit_type_strList(v, name, &replacement, errp)) {
        return;
    }
    qapi_free_strList(*slot);
    *slot = replacement;
}

static bool sgi_machine_has_identity_records(SGIMachineState *s)
{
    return s->chassis_eeprom || s->board_eeprom || s->spd_eeprom;
}

/*
 * Copies the machine's own identity-record file lists onto an L1 device
 * before it is realized. Takes the L1 explicitly because it lives on the
 * node (sgi-sn1-node), while the file lists it reads are machine
 * properties -- one identity per machine, one L1 per node.
 */
void sgi_machine_set_l1_identity_records(MachineState *machine,
                                         DeviceState *l1)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    size_t i;

    for (i = 0; i < ARRAY_SIZE(sgi_machine_identity_records); i++) {
        const SGIMachineIdentityRecords *records =
            &sgi_machine_identity_records[i];
        strList *file = *sgi_machine_identity_slot(OBJECT(s), (void *)records);
        QList *files;

        if (!file) {
            continue;
        }
        files = qlist_new();
        for (; file; file = file->next) {
            qlist_append_str(files, file->value);
        }
        qdev_prop_set_array(l1, records->name, files);
    }
}

/* ---- Topology resolution and the CPU-population contract ---- */

static bool sgi_machine_topology_matches_product(const SGIModelTopology *t,
                                                  const char *product)
{
    size_t len = strlen(product);
    size_t name_len = strlen(t->name);

    return name_len >= len && !strncmp(t->name, product, len) &&
           (t->name[len] == '\0' || t->name[len] == '-');
}

/*
 * sgi_model_entries[] holds every product's topologies in one table, so
 * a name match alone is not enough: topology= is product-scoped
 * (origin200 never builds origin2000's wiring).
 */
static const SGIModelTopology *sgi_machine_topology(MachineState *machine)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(machine);
    const char *name;
    const SGIModelTopology *t;

    if (smc->test_topology) {
        return smc->test_topology;
    }

    name = s->topology_name ? s->topology_name : smc->default_topology;
    t = sgi_model_find_topology(name);

    if (t && !sgi_machine_topology_matches_product(t, smc->product)) {
        return NULL;
    }
    return t;
}

static unsigned int sgi_machine_population_total(
    const SGIModelCpuPopulation *population)
{
    unsigned int total = 0;
    size_t i;

    for (i = 0; i < population->node_count; i++) {
        total += population->cpus_per_node[i];
    }
    return total;
}

unsigned int sgi_machine_product_max_cpus(const char *product)
{
    unsigned int max_cpus = 0;
    size_t i, j;

    for (i = 0; i < sgi_model_entry_count; i++) {
        const SGIModelTopology *t = &sgi_model_entries[i];

        if (!sgi_machine_topology_matches_product(t, product)) {
            continue;
        }
        for (j = 0; j < t->supported_cpu_population_count; j++) {
            unsigned int total =
                sgi_machine_population_total(&t->supported_cpu_populations[j]);

            max_cpus = MAX(max_cpus, total);
        }
    }
    return max_cpus;
}

/*
 * The NASID a node seeds with: its slot, relative to the topology's
 * lowest slot, wherever every node has a distinct slot. Origin 200's
 * slots all read zero, so a topology whose slots collide falls back to
 * node array order instead of colliding every node on NASID zero.
 */
static unsigned int sgi_machine_topology_nasid(const SGIModelTopology *t,
                                               unsigned int node_index)
{
    unsigned int slot_min;
    size_t i, j;

    for (i = 0; i < t->node_count; i++) {
        for (j = 0; j < i; j++) {
            if (t->nodes[i].slot == t->nodes[j].slot) {
                return node_index;
            }
        }
    }
    slot_min = t->nodes[0].slot;
    for (i = 1; i < t->node_count; i++) {
        slot_min = MIN(slot_min, t->nodes[i].slot);
    }
    return t->nodes[node_index].slot - slot_min;
}

uint64_t sgi_machine_topology_default_ram(const SGIModelTopology *t)
{
    uint64_t total_mib = 0;
    size_t i, b;

    for (i = 0; i < t->node_count; i++) {
        for (b = 0; b < SGI_MODEL_MAX_MEMORY_BANKS; b++) {
            total_mib += t->nodes[i].bank_mib[b];
        }
    }
    return total_mib * MiB;
}

unsigned int sgi_machine_topology_default_cpus(
    const SGIModelTopology *t)
{
    g_assert(t->supported_cpu_population_count > 0);
    return sgi_machine_population_total(&t->supported_cpu_populations[0]);
}

/* Fill requested[node_count] from -smp's shape; -smp validity, not fit. */
static void sgi_machine_population_shape(const SGIModelTopology *t,
                                         MachineState *machine,
                                         unsigned int *requested)
{
    unsigned int i;

    if (t->node_count == 1) {
        requested[0] = machine->smp.cpus;
        return;
    }
    for (i = 0; i < t->node_count; i++) {
        requested[i] = machine->smp.cores;
    }
}

static const SGIModelCpuPopulation *sgi_machine_find_population(
    const SGIModelTopology *t, const unsigned int *requested)
{
    size_t i;
    unsigned int n;

    for (i = 0; i < t->supported_cpu_population_count; i++) {
        const SGIModelCpuPopulation *population =
            &t->supported_cpu_populations[i];

        if (population->node_count != t->node_count) {
            continue;
        }
        for (n = 0; n < population->node_count; n++) {
            if (population->cpus_per_node[n] != requested[n]) {
                break;
            }
        }
        if (n == population->node_count) {
            return population;
        }
    }
    return NULL;
}

/*
 * Validate that the per-node memory size (-m split evenly across every
 * node) matches this node's accepted population, per sn-model.h's
 * SGIModelNode.accepted_memory_mib: a present list is the DIMM kit
 * sizes the node accepts, and an absent one means the node's own
 * shipped bank_mib[] total is the only accepted size, not an open
 * fitting rule.
 */
static bool sgi_machine_validate_memory_population(
    const SGIModelTopology *t, uint64_t per_node_mib)
{
    unsigned int node_index;

    for (node_index = 0; node_index < t->node_count; node_index++) {
        const SGIModelNode *node = &t->nodes[node_index];
        size_t i;

        if (!node->accepted_memory_mib || !node->accepted_memory_mib_count) {
            uint64_t shipped_mib = 0;
            unsigned int bank;

            for (bank = 0; bank < SGI_MODEL_MAX_MEMORY_BANKS; bank++) {
                shipped_mib += node->bank_mib[bank];
            }
            if (per_node_mib != shipped_mib) {
                return false;
            }
            continue;
        }
        for (i = 0; i < node->accepted_memory_mib_count; i++) {
            if (node->accepted_memory_mib[i] == per_node_mib) {
                break;
            }
        }
        if (i == node->accepted_memory_mib_count) {
            return false;
        }
    }
    return true;
}

/* ---- possible_cpu_arch_ids and friends ---- */

static const CPUArchIdList *sgi_machine_possible_cpu_arch_ids(
    MachineState *machine)
{
    const SGIModelTopology *t = sgi_machine_topology(machine);
    unsigned int max_cpus = machine->smp.max_cpus;
    unsigned int node_count = t ? t->node_count : machine->smp.sockets;
    g_autofree unsigned int *requested = NULL;
    g_autofree unsigned int *node_of = NULL;
    g_autofree unsigned int *core_of = NULL;
    unsigned int node_index, slice, index;

    if (machine->possible_cpus) {
        assert(machine->possible_cpus->len == max_cpus);
        return machine->possible_cpus;
    }
    machine->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                       sizeof(CPUArchId) * max_cpus);
    machine->possible_cpus->len = max_cpus;

    requested = g_new0(unsigned int, node_count ? node_count : 1);
    if (t) {
        sgi_machine_population_shape(t, machine, requested);
    } else if (node_count) {
        requested[0] = max_cpus;
    }

    /*
     * One (socket, core) pair per slot in shape order, then socket 0
     * for whatever -smp overshoots the population: sgi_machine_realize
     * reports a mismatch clearly, so NUMA parsing just needs a complete
     * table to reject from meanwhile.
     */
    node_of = g_new0(unsigned int, max_cpus);
    core_of = g_new0(unsigned int, max_cpus);
    index = 0;
    for (node_index = 0; node_index < node_count && index < max_cpus;
         node_index++) {
        for (slice = 0; slice < requested[node_index] && index < max_cpus;
             slice++, index++) {
            node_of[index] = node_index;
            core_of[index] = slice;
        }
    }
    for (; index < max_cpus; index++) {
        node_of[index] = 0;
        core_of[index] = index;
    }

    for (index = 0; index < max_cpus; index++) {
        CPUArchId *slot = &machine->possible_cpus->cpus[index];

        slot->type = machine->cpu_type;
        slot->vcpus_count = 1;
        slot->arch_id = index;
        slot->props.has_socket_id = true;
        slot->props.socket_id = node_of[index];
        slot->props.has_core_id = true;
        slot->props.core_id = core_of[index];
        slot->props.has_thread_id = true;
        slot->props.thread_id = 0;
        slot->props.has_node_id = true;
        slot->props.node_id = node_of[index];
    }
    return machine->possible_cpus;
}

static CpuInstanceProperties sgi_machine_cpu_index_to_props(
    MachineState *machine, unsigned int cpu_index)
{
    const CPUArchIdList *possible_cpus =
        sgi_machine_possible_cpu_arch_ids(machine);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}

static int64_t sgi_machine_get_default_cpu_node_id(
    const MachineState *machine, int index)
{
    const CPUArchIdList *possible_cpus =
        sgi_machine_possible_cpu_arch_ids((MachineState *)machine);

    assert(index >= 0 && (unsigned int)index < possible_cpus->len);
    return possible_cpus->cpus[index].props.socket_id;
}

/* ---- Onboard and Crossbow-attached I/O boards ---- */

/*
 * Split t->xtalk_attachments[] into each node's own entry and each
 * io_boards[] entry's placement. A direct board (crossbar_index < 0)
 * shares its one entry with its node. A Crossbow-attached node's first
 * entry in array order is always its own host entry, so a later entry
 * repeating a node index already seen is that node's board.
 */
static bool sgi_machine_index_xtalk_attachments(
    const SGIModelTopology *t, unsigned int *node_attachment,
    unsigned int *board_attachment, Error **errp)
{
    g_autofree bool *claimed = g_new0(bool, t->node_count);
    unsigned int board = 0;
    size_t i;

    for (i = 0; i < t->xtalk_attachment_count; i++) {
        const SGIModelXtalkAttachment *attachment = &t->xtalk_attachments[i];
        unsigned int node_index = attachment->node_index;

        if (node_index >= t->node_count) {
            error_setg(errp, "topology '%s' crosstalk attachment %zu names "
                      "node %u, past its %zu nodes", t->name, i, node_index,
                      t->node_count);
            return false;
        }
        if (attachment->crossbar_index < 0) {
            node_attachment[node_index] = i;
            claimed[node_index] = true;
            if (board >= t->io_board_count) {
                error_setg(errp, "topology '%s' names more direct-attach or "
                          "board-owning crosstalk attachments than its %zu "
                          "io board(s)", t->name, t->io_board_count);
                return false;
            }
            board_attachment[board++] = i;
            continue;
        }
        if (!claimed[node_index]) {
            claimed[node_index] = true;
            node_attachment[node_index] = i;
        } else {
            if (board >= t->io_board_count) {
                error_setg(errp, "topology '%s' names more direct-attach or "
                          "board-owning crosstalk attachments than its %zu "
                          "io board(s)", t->name, t->io_board_count);
                return false;
            }
            board_attachment[board++] = i;
        }
    }
    /*
     * A node no attachment entry ever claimed: the same fault the
     * deleted sgi_machine_populate_node used to catch with
     * g_assert(board && xtalk), relocated here now that this function
     * resolves each node's own attachment before the board path builds
     * anything.
     */
    for (i = 0; i < t->node_count; i++) {
        if (!claimed[i]) {
            error_setg(errp, "topology '%s' node %zu names no direct-attach "
                      "I/O board or crosstalk attachment", t->name, i);
            return false;
        }
    }
    if (board != t->io_board_count) {
        error_setg(errp, "topology '%s' names %zu io board(s) but only %u "
                  "crosstalk attachment(s) own one", t->name,
                  t->io_board_count, board);
        return false;
    }
    return true;
}

static const char *sgi_machine_io_board_type(const SGIModelIoBoard *record)
{
    /*
     * board_kind alone decides the class: the descriptor already names
     * which board this is. Origami trunk eb6eff401f corrected the
     * exporter's own defect that once made inferring the class from how
     * a board attaches (direct vs. Crossbow) agree with board_kind on
     * every topology shipped so far -- hw/mips/sgi/sn-model.c is now that
     * corrected exporter's exact output; tests/unit/test-sn-model.c
     * checks every entry's board_kind once, so this function trusts the
     * descriptor and only asserts on a kind it does not recognize.
     */
    switch (record->board_kind) {
    case SGI_MODEL_IO_IP29_IO:
        return TYPE_SGI_IP29_IO;
    case SGI_MODEL_IO_BASEIO:
        return TYPE_SGI_BASEIO;
    case SGI_MODEL_IO_IO8:
        return TYPE_SGI_IO8;
    default:
        g_assert_not_reached();
    }
}

/*
 * Builds and realizes the topology's one Bedrock-attached I/O board
 * (today always sgi-io8) from its populated io_boards[] entry, wiring its
 * "fabric" link to the region the caller supplies. Every I/O board's
 * device is created here regardless of node kind, matching the Hub
 * products' sgi_machine_instantiate_io_board() above; what differs is
 * the caller and the timing. Origin 300's Bedrock has no Crossbow, so there
 * is no module Crossbow's own address space to hand this class at
 * machine setup time the way the Hub path does -- the region only exists
 * once the node's own Bedrock device has been created, which is
 * sn1-node.c's job, not this builder's. sn1-node.c calls this once its
 * own Bedrock exists (created, not yet realized: sysbus_init_mmio() has
 * already given it the crosstalk egress region this needs). "slot" has no
 * Origin 300 hardware meaning of its own (sgi-io8's own header comment);
 * the caller supplies the fixed crosstalk port the whole chip answers on
 * only to satisfy the base class's module+slot uniqueness registry.
 */
DeviceState *sgi_machine_build_bedrock_io_board(
    const SGIModelIoBoard *record, unsigned int slot, MemoryRegion *fabric,
    Error **errp)
{
    DeviceState *dev;
    size_t i;

    dev = qdev_new(sgi_machine_io_board_type(record));
    object_property_add_child(qdev_get_machine(), "io-board0", OBJECT(dev));
    object_unref(OBJECT(dev));
    qdev_prop_set_uint32(dev, "module", record->module_index);
    qdev_prop_set_uint32(dev, "slot", slot);
    qdev_prop_set_uint64(dev, "onewire-serial", record->serial);
    /*
     * The onboard IOC3's own one-wire identity part carries its own
     * separate serial, a fact about this specific fitted unit -- the
     * descriptor's own io_boards[].pci[] entry for the IOC3 function, not
     * the board's own "onewire-serial" above. Matches
     * sgi_machine_instantiate_io_board's identical comment.
     */
    for (i = 0; i < record->pci_count; i++) {
        if (record->pci[i].kind == SGI_MODEL_PCI_IOC3) {
            qdev_prop_set_uint64(dev, "ioc3-onewire-serial",
                                 record->pci[i].serial);
            break;
        }
    }
    /*
     * The onboard IOC3's Timekeeper storage: an indexed `-drive if=mtd`
     * first, else a bare `-drive if=none,id=sgi-nvram0` by name -- one
     * Bedrock node, so always index/id zero, unlike the Hub path's
     * per-board index. Passed as the resolved backend's own name, not the
     * backend itself, for the claim-timing reason
     * sgi_machine_instantiate_io_board's identical comment gives.
     */
    {
        DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, 0);
        BlockBackend *nvram_blk = nvram_dinfo ?
            blk_by_legacy_dinfo(nvram_dinfo) : blk_by_name("sgi-nvram0");

        if (nvram_blk) {
            qdev_prop_set_string(dev, "nvram-drive", blk_name(nvram_blk));
        }
    }
    /*
     * The onboard IOC3's serial and parallel backends, named by chardev
     * id for the same claim-timing reason. serial_hd(0) is the L1
     * console -- this node's own controller, not this board's -- so the
     * onboard IOC3 takes indices one through four rather than the Hub
     * path's sgi_machine_compute_serial_line() console-first ordering,
     * which assumes a Hub node's own six-line block with no console
     * device of its own ahead of it.
     */
    {
        Chardev *chr;

        chr = serial_hd(1);
        if (chr) {
            qdev_prop_set_string(dev, "chardev-a", chr->label);
        }
        chr = serial_hd(2);
        if (chr) {
            qdev_prop_set_string(dev, "chardev-b", chr->label);
        }
        chr = serial_hd(3);
        if (chr) {
            qdev_prop_set_string(dev, "record-a", chr->label);
        }
        chr = serial_hd(4);
        if (chr) {
            qdev_prop_set_string(dev, "record-b", chr->label);
        }
        if (parallel_hds[0]) {
            qdev_prop_set_string(dev, "parallel-chardev",
                                 parallel_hds[0]->label);
        }
    }
    object_property_set_link(OBJECT(dev), "fabric", OBJECT(fabric),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(dev), errp)) {
        return NULL;
    }
    return dev;
}

/*
 * The console node's IOC3 port A is serial line 0; other nodes follow
 * in index order from line 6.
 */
static unsigned int sgi_machine_compute_serial_line(
    const SGIModelTopology *t, unsigned int node_index, unsigned int offset)
{
    unsigned int position;

    if (node_index == t->console) {
        position = 0;
    } else if (node_index < t->console) {
        position = node_index + 1;
    } else {
        position = node_index;
    }
    return position * SN0_SERIAL_LINES_PER_NODE + offset;
}

/*
 * Every widget position a Crossbow physically wires (docs/datasheets/
 * xbow.md: one or two host connections plus six I/O ports), regardless
 * of what boot populates: sgi_xbow_realize requires startup-links-alive
 * (below) to stay within this. startup-links-alive reports only what is
 * actually populated.
 */
#define SGI_MACHINE_CROSSBAR_LINKS_PRESENT 0xff00

static void sgi_machine_build_crossbars(SGIMachineState *s,
                                        const SGIModelTopology *t)
{
    size_t i;

    for (i = 0; i < t->crossbar_count; i++) {
        const SGIModelCrossbar *descriptor = &t->crossbars[i];
        DeviceState *crossbow = qdev_new(TYPE_SGI_XBOW);
        g_autofree char *name = g_strdup_printf("crossbow%zu", i);
        uint16_t startup_alive = 0;
        size_t attachment_index;

        /*
         * The widget mask this Crossbow's own attachments populate,
         * reported as startup-links-alive for software to read.
         */
        for (attachment_index = 0;
             attachment_index < t->xtalk_attachment_count;
             attachment_index++) {
            const SGIModelXtalkAttachment *attachment =
                &t->xtalk_attachments[attachment_index];

            if ((unsigned int)attachment->crossbar_index == i) {
                startup_alive |= 1u << attachment->widget;
            }
        }

        s->crossbars[i] = SGI_XBOW(crossbow);
        object_property_add_child(OBJECT(s), name, OBJECT(crossbow));
        object_unref(OBJECT(crossbow));
        qdev_prop_set_uint64(crossbow, "span", SN0_XTALK_WIDGET_SIZE);
        qdev_prop_set_bit(crossbow,
                          "complete-posted-error-address-writes", true);
        qdev_prop_set_uint32(crossbow, "revision",
                             descriptor->widget_revision);
        qdev_prop_set_uint32(crossbow, "part", descriptor->widget_part);
        qdev_prop_set_uint32(crossbow, "manufacturer",
                             descriptor->widget_manufacturer);
        qdev_prop_set_uint16(crossbow, "links-present",
                             SGI_MACHINE_CROSSBAR_LINKS_PRESENT);
        qdev_prop_set_uint16(crossbow, "startup-links-alive", startup_alive);
        /*
         * Stock PROM resets populated links and expects one alive before
         * the reset to return to service after the pulse.
         */
        qdev_prop_set_uint16(crossbow, "link-reset-restore-alive",
                             startup_alive);
    }
}

/* A simple uint64 qdev property, applied by object_property_set_uint. */
struct SGIMachineUintProperty {
    const char *name;
    uint64_t value;
};

/*
 * Build one io_boards[] entry, wire it to its own crosstalk position and
 * realize it. A Crossbow-attached board attaches through
 * sgi_xtalk_crossbow_link_attach_widget(); the Crossbow itself realizes
 * later, once every board and node that attaches to it has (see
 * sgi_machine_realize's own crossbar-realize pass, after this function's
 * every caller has returned).
 */
static bool sgi_machine_instantiate_io_board(
    SGIMachineState *s, const SGIModelTopology *t, unsigned int board_index,
    const SGIModelXtalkAttachment *attachment, Error **errp)
{
    const SGIModelIoBoard *record = &t->io_boards[board_index];
    bool direct = attachment->crossbar_index < 0;
    DeviceState *dev = qdev_new(sgi_machine_io_board_type(record));
    g_autofree char *name = g_strdup_printf("io-board%u", board_index);
    /*
     * "module" is the descriptor's own module index, unique per module
     * by construction, unlike a direct-attach product's shipped module
     * number (a two-chassis Origin 200's onboard boards would otherwise
     * collide on the base class's module+slot uniqueness check, since
     * slot is the same fixed widget 8 on both).
     */
    struct SGIMachineUintProperty uint_props[4] = {
        { "module", record->module_index },
        { "slot", attachment->widget },
        { "onewire-serial", record->serial },
    };
    static const struct {
        unsigned int offset;
        const char *prop;
    } serial_bindings[] = {
        { SN0_IOC3_SERIAL_A_CHARDEV, "chardev-a" },
        { SN0_IOC3_SERIAL_B_CHARDEV, "chardev-b" },
        { SN0_IOC3_RECORD_A_CHARDEV, "record-a" },
        { SN0_IOC3_RECORD_B_CHARDEV, "record-b" },
    };
    /*
     * "pci"/"pci.<N>" direct, or "pci.baseio<N>" behind a Crossbow: two
     * boards of the same class (Origin 200's two onboard sgi-ip29-io
     * instances) would otherwise share a migration-visible PCI bus name.
     */
    g_autofree char *bus_name = direct ?
        (board_index == 0 ? g_strdup("pci")
                          : g_strdup_printf("pci.%u", board_index)) :
        g_strdup_printf("pci.baseio%u", board_index);
    g_autofree char *nvram_id = g_strdup_printf("sgi-nvram%u", board_index);
    DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, board_index);
    BlockBackend *nvram_blk = nvram_dinfo ?
        blk_by_legacy_dinfo(nvram_dinfo) : blk_by_name(nvram_id);
    unsigned int uint_prop_count = 3;
    SGIXtalkCrossbowLink *crossbow_link =
        &s->crossbow_links[s->num_nodes + board_index];
    SGIXtalkBoardState *board;
    size_t i;

    object_property_add_child(OBJECT(s), name, OBJECT(dev));
    object_unref(OBJECT(dev));

    /*
     * "ioc3-onewire-serial" is the onboard IOC3's own separate one-wire
     * identity part (Bridge's NIC ROM identity is a different part),
     * left at its class default when this record's pci[] names no IOC3.
     */
    for (i = 0; i < record->pci_count; i++) {
        if (record->pci[i].kind == SGI_MODEL_PCI_IOC3) {
            uint_props[uint_prop_count++] = (struct SGIMachineUintProperty){
                "ioc3-onewire-serial", record->pci[i].serial,
            };
            break;
        }
    }
    for (i = 0; i < uint_prop_count; i++) {
        object_property_set_uint(OBJECT(dev), uint_props[i].name,
                                 uint_props[i].value, &error_fatal);
    }
    qdev_prop_set_string(dev, "pci-bus-name", bus_name);

    /*
     * The onboard IOC3's Timekeeper storage: an indexed `-drive if=mtd`
     * first, else a bare `-drive if=none,id=sgi-nvram<N>` by name.
     * Passed down as the resolved backend's own name, not the backend
     * itself: a "drive" property claims its backend the moment it is
     * set, and the board's own "ioc3" child has to be the one that
     * claims it, on realize -- the same claim-timing problem the
     * chardev properties below avoid the same way.
     */
    if (nvram_blk) {
        qdev_prop_set_string(dev, "nvram-drive", blk_name(nvram_blk));
    }
    /*
     * The onboard IOC3's serial backends: named by chardev id, not
     * passed as a "chardev" property, since setting one of those claims
     * the backend immediately, and the board's own build_widget() has
     * to be the one call that claims it, on the "ioc3" child it is
     * about to realize. The topology's console field orders these by
     * node (sgi_machine_compute_serial_line).
     */
    for (i = 0; i < ARRAY_SIZE(serial_bindings); i++) {
        Chardev *chr = serial_hd(sgi_machine_compute_serial_line(
            t, attachment->node_index, serial_bindings[i].offset));

        if (chr) {
            qdev_prop_set_string(dev, serial_bindings[i].prop, chr->label);
        }
    }
    /*
     * Only sgi-ip29-io exposes "parallel-chardev" (a parallel function);
     * BaseIO's board carries no parallel port, matching sgi-baseio's
     * own class.
     */
    if (direct && board_index < MAX_PARALLEL_PORTS &&
        parallel_hds[board_index]) {
        qdev_prop_set_string(dev, "parallel-chardev",
                             parallel_hds[board_index]->label);
    }

    if (direct) {
        const SN0ModelAddressMap *map = &sgi_sn0_hub_address_map;
        SN0NodeState *node = s->nodes[attachment->node_index];

        /*
         * Interrupt packets travel a processor's claim view, which
         * resolves a node-qualified address by programmed number, not by
         * the board's position. View zero speaks for the node; a node
         * always has a processor. This is a Hub/Origin-200 fact the
         * direct link's own port interface never has to carry, since it
         * is set once here rather than once per widget the link aliases.
         */
        object_property_set_link(OBJECT(dev), "fabric",
                                 OBJECT(&node->claim_view[0]), &error_fatal);
        qdev_prop_set_uint64(dev, "fabric-base", map->xtalk.carrier_base);
        qdev_prop_set_uint32(dev, "fabric-node-shift",
                             ctz64(map->node_stride));
    } else {
        /*
         * A switched fabric carries the destination widget in its own
         * packet header, so the Bridge keeps it in bits 51:48 of the
         * address it composes rather than spending address bits on a
         * node number the switch would not read. This Crossbow's own
         * packet ingress does not exist until it realizes, so the link's
         * own proxy stands in; sgi_xtalk_crossbow_link_finish() aliases
         * the real ingress onto it once the Crossbow is built.
         */
        g_autofree char *proxy_name =
            g_strdup_printf("io-board%u-packet-proxy", board_index);

        object_property_set_link(
            OBJECT(dev), "fabric",
            OBJECT(sgi_xtalk_crossbow_link_prepare(crossbow_link, OBJECT(s),
                                                   proxy_name)),
            &error_fatal);
        qdev_prop_set_uint32(dev, "fabric-widget-shift",
                             SGI_BRIDGE_PACKET_ADDRESS_BITS);
    }

    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    board = SGI_XTALK_BOARD(dev);
    s->io_boards[board_index] = board;

    /*
     * Direct attachment installs the board's whole widget face into its
     * node's xtalk container, aliased at every other decoded widget too
     * (docs/datasheets/bridge.md: the Bridge answers its full 16 MB face
     * at every non-zero widget the Hub sends out its link), and wires
     * the PROM's fixed widget-2 PI-error probe through it; a Crossbow-
     * attached board instead attaches through
     * sgi_xtalk_crossbow_link_attach_widget().
     */
    if (direct) {
        SN0NodeState *node = s->nodes[attachment->node_index];

        if (!sgi_xtalk_direct_link_connect(
                &s->direct_links[board_index], OBJECT(s), node, OBJECT(board),
                attachment->discovery_widget, attachment->widget,
                attachment->decoded_widget_mask, SGI_SN0_PROM_PI_PROBE_WIDGET,
                SGI_SN0_PROM_PI_PROBE_OFFSET, errp)) {
            return false;
        }
    } else {
        if (!sgi_xtalk_crossbow_link_attach_widget(
                crossbow_link, OBJECT(board),
                s->crossbars[attachment->crossbar_index], attachment->widget,
                errp)) {
            return false;
        }
        board->attached = true;
    }
    return true;
}

/*
 * Realizes every Crossbow this topology built, once every node and board
 * that attaches to one has (sgi_xbow_attach_endpoint() refuses on an
 * already-realized Crossbow, so this can run no earlier), then installs
 * each attached link's own real ingress views -- the first moment they
 * exist -- as what its outer port's own outbound traffic reaches.
 * node_attachment and board_attachment are sgi_machine_realize's own,
 * already resolved by sgi_machine_index_xtalk_attachments there; nothing
 * here waits for a `-device`-plugged board, unlike the deferred
 * machine-init-done hook this replaces: a crosstalk board realized after
 * the machine returns is not attached to anything (see the crosstalk
 * port experiment's own report for why).
 */
static bool sgi_machine_realize_crossbars(
    SGIMachineState *s, const SGIModelTopology *t,
    const unsigned int *node_attachment, const unsigned int *board_attachment,
    Error **errp)
{
    size_t i;

    for (i = 0; i < t->crossbar_count; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(s->crossbars[i]), errp)) {
            return false;
        }
    }
    for (i = 0; i < t->node_count; i++) {
        const SGIModelXtalkAttachment *attachment =
            &t->xtalk_attachments[node_attachment[i]];
        g_autofree char *name = g_strdup_printf("node%zu", i);

        if (attachment->crossbar_index < 0) {
            continue;
        }
        if (!sgi_xtalk_crossbow_link_finish(
                &s->crossbow_links[i], OBJECT(s), name,
                s->crossbars[attachment->crossbar_index], attachment->widget,
                errp)) {
            return false;
        }
    }
    for (i = 0; i < t->io_board_count; i++) {
        const SGIModelXtalkAttachment *attachment =
            &t->xtalk_attachments[board_attachment[i]];
        g_autofree char *name = g_strdup_printf("io-board%zu", i);

        if (attachment->crossbar_index < 0) {
            continue;
        }
        if (!sgi_xtalk_crossbow_link_finish(
                &s->crossbow_links[s->num_nodes + i], OBJECT(s), name,
                s->crossbars[attachment->crossbar_index], attachment->widget,
                errp)) {
            return false;
        }
    }
    return true;
}

/* ---- Module identity, shared controllers and routers ---- */

/*
 * A module naming a shared system controller shares one MicroLAN
 * module-identity responder across every node it carries. A module
 * whose nodes each carry their own controller (Origin 200) has none.
 */
static void sgi_machine_attach_module_identities(SGIMachineState *s,
                                                  const SGIModelTopology *t)
{
    size_t module_index;

    for (module_index = 0; module_index < t->module_count; module_index++) {
        const SGIModelModule *module = &t->modules[module_index];
        SGIHubModuleIdentity identity;
        size_t i;

        if (module->controller_index < 0) {
            continue;
        }
        if (!sgi_sn0_module_management_identity_pages(module->serial,
                                                      identity.page)) {
            error_report("module '%s' has an invalid identity serial",
                        module->id);
            exit(EXIT_FAILURE);
        }
        for (i = 0; i < module->node_count; i++) {
            unsigned int node_index = module->nodes[i];

            sgi_sn0_module_management_identity_rom(
                t->nodes[node_index].slot, module->initial_module_number,
                identity.rom);
            if (!sgi_hub_attach_module_identity(&s->nodes[node_index]->hub,
                                                &identity, &error_fatal)) {
                exit(EXIT_FAILURE);
            }
        }
    }
}

/*
 * One shared controller per module that names one: an MSC with one
 * PCF8584 adapter reflected into each of the module's nodes. The
 * adapter array is capacity SGI_O2000_ADAPTER_COUNT (every module
 * measured ships four nodes); asserted here rather than truncated.
 */
static bool sgi_machine_build_controllers(SGIMachineState *s,
                                          const SGIModelTopology *t,
                                          Error **errp)
{
    size_t controller_index;

    for (controller_index = 0; controller_index < t->controller_count;
         controller_index++) {
        const SGIModelController *controller =
            &t->controllers[controller_index];
        const SGIModelModule *module = NULL;
        DeviceState *dev;
        Chardev *aux;
        size_t i;

        for (i = 0; i < t->module_count; i++) {
            if (t->modules[i].controller_index == (int)controller_index) {
                module = &t->modules[i];
                break;
            }
        }
        if (!module) {
            error_setg(errp, "topology '%s' controller %zu names no module",
                      t->name, controller_index);
            return false;
        }
        if (controller->kind != SGI_MODEL_CONTROLLER_MSC) {
            error_setg(errp, "topology '%s' controller %zu names an unknown "
                      "controller kind", t->name, controller_index);
            return false;
        }
        if (module->node_count > SGI_O2000_ADAPTER_COUNT) {
            error_setg(errp, "topology '%s' module '%s' has %zu nodes, "
                      "beyond its controller's %u adapters", t->name,
                      module->id, module->node_count,
                      SGI_O2000_ADAPTER_COUNT);
            return false;
        }

        dev = qdev_new(TYPE_SGI_SN0_MODULE_MANAGEMENT);
        object_property_add_child(OBJECT(s), "module-management",
                                  OBJECT(dev));
        qdev_prop_set_uint8(dev, "module-number",
                            module->initial_module_number);
        qdev_prop_set_string(dev, "model-serial", module->serial);
        qdev_prop_set_uint8(dev, "dip-switches", s->elsc_dip_switches);
        aux = serial_hd(sgi_machine_compute_serial_line(
            t, module->nodes[0], SN0_ELSC_CHARDEV));
        if (aux) {
            qdev_prop_set_chr(dev, "chardev", aux);
        }
        qdev_realize_and_unref(dev, NULL, &error_fatal);

        for (i = 0; i < module->node_count; i++) {
            unsigned int node_index = module->nodes[i];
            const SGIModelNode *model_node = &t->nodes[node_index];
            SGIHubState *hub = &s->nodes[node_index]->hub;
            SysBusDevice *adapter = SYS_BUS_DEVICE(
                sgi_sn0_module_management_get_adapter(
                    SGI_SN0_MODULE_MANAGEMENT(dev), i));

            memory_region_add_subregion_overlap(
                &hub->regs, model_node->management_adapter_offset,
                sysbus_mmio_get_region(adapter, 0), 1);
        }
    }
    return true;
}

/* Added in array order so the fabric's vertex index matches router index. */
static void sgi_machine_build_routers(SGIMachineState *s,
                                      const SGIModelTopology *t)
{
    size_t i;

    for (i = 0; i < t->router_count; i++) {
        const SGIModelRouter *router = &t->routers[i];
        g_autofree char *name = g_strdup_printf("router%zu", i);
        DeviceState *dev = qdev_new(TYPE_SGI_ROUTER);
        unsigned int vertex;

        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        qdev_prop_set_uint8(dev, "chip-inputs", router->chip_inputs);
        qdev_prop_set_uint32(dev, "slot", router->slot);
        qdev_prop_set_uint64(dev, "nic-clock-hz",
                            sgi_sn0_hub_hardware.hub_clock_hz);
        qdev_prop_set_uint64(dev, "nic-serial", router->serial);
        vertex = sn0_fabric_routed_add_router(s->fabric, SGI_ROUTER(dev));
        g_assert(vertex == i);
        qdev_realize_and_unref(dev, NULL, &error_fatal);
    }
}

/* ---- The builder ---- */

/*
 * The IP27 PROM jump table at flash offset zero: ten entries, real
 * hardware's own layout.
 */
#define SGI_MACHINE_FLASH_ENTRIES       10
#define SGI_MACHINE_FLASH_ENTRY_STRIDE  8

/*
 * The firmware configuration block, at flash offset 0x60 -- real
 * hardware's own layout.
 */
#define SGI_MACHINE_CONFIG_OFFSET              0x60
/* "ip27conf" */
#define SGI_MACHINE_CONFIG_MAGIC               0x69703237636f6e66ULL
#define SGI_MACHINE_CONFIG_SIZE                0x50
#define SGI_MACHINE_CONFIG_FPROM_CYC_OFFSET    0x2c
#define SGI_MACHINE_CONFIG_MACHINE_TYPE_OFFSET 0x30
#define SGI_MACHINE_CONFIG_CHECKSUM_OFFSET     0x34
#define SGI_MACHINE_CONFIG_FPROM_WR_OFFSET     0x3c

/* Where the factory-fresh environment log sits in the flash image. */
#define SGI_MACHINE_PROMLOG_SECTOR    14
#define SGI_MACHINE_PROMLOG_MAGIC     0x504c4f47
#define SGI_MACHINE_PROMLOG_VERSION   1

/*
 * The default flash template: entry zero spins (an unprogrammed reset
 * vector), the rest return, plus a configuration block and factory-
 * fresh environment log in the real IP27 firmware's own layout. A
 * -bios image shorter than the flash overlays only its own bytes, so a
 * short image still finds these pre-formatted underneath.
 */
static void sgi_machine_build_flash(const SGIModelTopology *t,
                                    const SGIMachineClass *smc,
                                    uint8_t *flash, uint64_t flash_size)
{
    static const uint32_t jr_ra = 0x03e00008;      /* jr $31 */
    static const uint32_t branch_to_self = 0x1000ffff; /* b . */
    const SGIModelProcessor *cpu = t->nodes[0].cpu_slots[0].assembly;
    uint8_t *config = flash + SGI_MACHINE_CONFIG_OFFSET;
    int i;

    g_assert(flash_size >=
             (SGI_MACHINE_PROMLOG_SECTOR + 1) * sn0_flash.sector_size);
    memset(flash, 0xff, flash_size);

    for (i = 0; i < SGI_MACHINE_FLASH_ENTRIES; i++) {
        uint8_t *entry = flash + i * SGI_MACHINE_FLASH_ENTRY_STRIDE;

        stl_be_p(entry, i == 0 ? branch_to_self : jr_ra);
        stl_be_p(entry + 4, 0);                    /* the delay slot */
    }

    /* The field order and widths are the IP27 firmware's. */
    stl_be_p(config + 0x00, 0xb);                  /* timing constant */
    stl_be_p(config + 0x04, cpu->mode_bits);
    stq_be_p(config + 0x08, SGI_MACHINE_CONFIG_MAGIC);
    stq_be_p(config + 0x10, cpu->clock_hz);
    stq_be_p(config + 0x18, sgi_sn0_hub_hardware.hub_clock_hz);
    stq_be_p(config + 0x20, sgi_sn0_hub_hardware.rtc_clock_hz);
    stl_be_p(config + 0x28, 1);                    /* error correction on */
    stl_be_p(config + SGI_MACHINE_CONFIG_FPROM_CYC_OFFSET,
             smc->flash_read_cycles);
    stl_be_p(config + SGI_MACHINE_CONFIG_MACHINE_TYPE_OFFSET,
             smc->firmware_machine_type);
    /* checksum adjustment */
    stl_be_p(config + SGI_MACHINE_CONFIG_CHECKSUM_OFFSET, 0);
    stl_be_p(config + 0x38, 0);                    /* flash program count */
    stl_be_p(config + SGI_MACHINE_CONFIG_FPROM_WR_OFFSET,
             smc->flash_write_cycles);
    stl_be_p(config + 0x40, 6);                    /* firmware version */
    stl_be_p(config + 0x44, 11);                   /* firmware revision */
    stl_be_p(config + 0x48, 0);                    /* configuration flags */

    /* The factory-fresh environment log; an erased first entry ends it. */
    config = flash + SGI_MACHINE_PROMLOG_SECTOR * sn0_flash.sector_size;
    stl_be_p(config + 0x10, SGI_MACHINE_PROMLOG_MAGIC);
    stl_be_p(config + 0x14, SGI_MACHINE_PROMLOG_VERSION);
    stl_be_p(config + 0x18, 1);                    /* initial sequence */
}

static uint8_t sgi_machine_config_byte_sum(const uint8_t *config)
{
    uint8_t sum = 0;
    unsigned int i;

    for (i = 0; i < SGI_MACHINE_CONFIG_SIZE; i++) {
        sum += config[i];
    }
    return sum;
}

/*
 * The flasher's contract: the three product values in, every other field
 * and the old block sum preserved; no IP27 signature, no touch. Runs
 * whether the configuration block present is this file's own default
 * template or a real PROM image's -- every stock PROM image carries one
 * at this same offset with this same magic.
 */
static void sgi_machine_configure_flash(const SGIMachineClass *smc,
                                        uint8_t *flash)
{
    uint8_t *config = flash + SGI_MACHINE_CONFIG_OFFSET;
    uint8_t old_sum;
    uint8_t new_sum;

    if (ldq_be_p(config + 0x08) != SGI_MACHINE_CONFIG_MAGIC) {
        return;
    }

    old_sum = sgi_machine_config_byte_sum(config);
    stl_be_p(config + SGI_MACHINE_CONFIG_CHECKSUM_OFFSET, 0);
    stl_be_p(config + SGI_MACHINE_CONFIG_FPROM_CYC_OFFSET,
             smc->flash_read_cycles);
    stl_be_p(config + SGI_MACHINE_CONFIG_MACHINE_TYPE_OFFSET,
             smc->firmware_machine_type);
    stl_be_p(config + SGI_MACHINE_CONFIG_FPROM_WR_OFFSET,
             smc->flash_write_cycles);
    new_sum = sgi_machine_config_byte_sum(config);
    stl_be_p(config + SGI_MACHINE_CONFIG_CHECKSUM_OFFSET,
             (uint8_t)(old_sum - new_sum));
}

/*
 * -bios fills every node's boot flash unless a pflash drive already backs
 * it (set on the node before it realized); a short -bios image leaves the
 * default template's own bytes behind it, matching a factory-formatted
 * chip rather than a merely erased one.
 */
static bool sgi_machine_load_flash(MachineState *machine,
                                   const SGIModelTopology *t,
                                   SGIMachineState *s, Error **errp)
{
    const SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(machine);
    uint64_t flash_size = sgi_sn0_hub_address_map.flash_reset.size;
    g_autofree uint8_t *flash = NULL;
    unsigned int node_index;

    flash = g_malloc(flash_size);
    sgi_machine_build_flash(t, smc, flash, flash_size);
    if (machine->firmware) {
        g_autofree char *path =
            qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
        int64_t image_size = path ? get_image_size(path, NULL) : -1;
        ssize_t size;

        if (image_size < 0) {
            error_setg(errp, "%s: cannot read the firmware image '%s'",
                      MACHINE_GET_CLASS(machine)->name, machine->firmware);
            return false;
        }
        if ((uint64_t)image_size > flash_size) {
            error_setg(errp, "%s: firmware image '%s' exceeds flash "
                      "capacity", MACHINE_GET_CLASS(machine)->name,
                      machine->firmware);
            return false;
        }
        size = load_image_size(path, flash, flash_size);
        if (size != image_size) {
            error_setg(errp, "%s: cannot load firmware image '%s'",
                      MACHINE_GET_CLASS(machine)->name, machine->firmware);
            return false;
        }
    }
    sgi_machine_configure_flash(smc, flash);
    for (node_index = 0; node_index < s->num_nodes; node_index++) {
        SN0NodeState *node = s->nodes[node_index];
        MemoryRegion *chip;

        if (node->flash_drive) {
            continue;
        }
        chip = sysbus_mmio_get_region(SYS_BUS_DEVICE(node->flash), 0);
        memcpy(memory_region_get_ram_ptr(chip), flash, flash_size);
        memory_region_set_dirty(chip, 0, flash_size);
    }
    return true;
}

static bool sgi_machine_realize(MachineState *machine, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    const SGIModelTopology *t = sgi_machine_topology(machine);
    g_autofree unsigned int *requested = NULL;
    g_autofree unsigned int *node_attachment = NULL;
    g_autofree unsigned int *board_attachment = NULL;
    g_autofree BlockBackend **flash_drives = NULL;
    unsigned int flash_count = 0;
    unsigned int node_count, node_index, board_index;
    unsigned int cpu_index = 0, cpu_base = 0;
    uint64_t ram_offset = 0;
    size_t link_index;

    if (!t) {
        GString *names = g_string_new(NULL);
        size_t i;

        for (i = 0; i < sgi_model_entry_count; i++) {
            if (sgi_machine_topology_matches_product(&sgi_model_entries[i],
                                                      smc->product)) {
                if (names->len) {
                    g_string_append(names, ", ");
                }
                g_string_append(names, sgi_model_entries[i].name);
            }
        }
        error_setg(errp, "%s: unknown topology '%s'; known topologies: %s",
                  mc->name, s->topology_name ? s->topology_name : "(none)",
                  names->str);
        g_string_free(names, true);
        return false;
    }
    if (sgi_machine_has_identity_records(s)) {
        bool has_l1 = false;
        size_t i;

        for (i = 0; i < t->controller_count; i++) {
            if (t->controllers[i].kind == SGI_MODEL_CONTROLLER_L1) {
                has_l1 = true;
                break;
            }
        }
        if (!has_l1) {
            error_report("%s: chassis-eeprom/board-eeprom/spd-eeprom name "
                        "an L1 identity controller, which topology '%s' "
                        "does not have", mc->name, t->name);
            exit(EXIT_FAILURE);
        }
    }
    if (machine->kernel_filename) {
        error_setg(errp, "%s: stock-PROM boot only; boot a kernel through "
                  "the PROM's own TFTP netboot or a disk image, not -kernel",
                  mc->name);
        return false;
    }
    if (machine->numa_state && machine->numa_state->num_nodes) {
        error_setg(errp, "%s: -numa is refused; topology= selects the "
                  "wiring, -smp and -m select the population within it",
                  mc->name);
        return false;
    }
    if (machine->smp.threads != 1) {
        error_setg(errp, "%s: one thread per core", mc->name);
        return false;
    }

    node_count = t->node_count;
    requested = g_new0(unsigned int, node_count);
    if (node_count == 1) {
        if (machine->smp.sockets != 1) {
            error_setg(errp, "%s: topology '%s' is one node, one socket",
                      mc->name, t->name);
            return false;
        }
        requested[0] = machine->smp.cpus;
    } else if (machine->smp.sockets == 1 &&
               machine->smp.cores == machine->smp.cpus) {
        /*
         * A bare -smp N (no explicit topology) completes to one socket
         * of N cores before this runs, for a default topology of more
         * than one node: accept it as the even split across the
         * topology's nodes it names, the way -smp N alone always has for
         * this product, and correct smp.sockets/cores to match what was
         * actually built.
         */
        if (machine->smp.cpus % node_count) {
            error_setg(errp, "%s: topology '%s' splits -smp evenly across "
                      "%u nodes", mc->name, t->name, node_count);
            return false;
        }
        machine->smp.sockets = node_count;
        machine->smp.cores = machine->smp.cpus / node_count;
        for (node_index = 0; node_index < node_count; node_index++) {
            requested[node_index] = machine->smp.cores;
        }
    } else {
        if (machine->smp.sockets != node_count ||
            machine->smp.cpus != node_count * machine->smp.cores) {
            error_setg(errp, "%s: topology '%s' is %u nodes as %u sockets "
                      "with one core count each", mc->name, t->name,
                      node_count, node_count);
            return false;
        }
        for (node_index = 0; node_index < node_count; node_index++) {
            requested[node_index] = machine->smp.cores;
        }
    }
    if (!sgi_machine_find_population(t, requested)) {
        error_setg(errp, "%s: topology '%s' does not accept %u CPUs as %u "
                  "socket(s)", mc->name, t->name, machine->smp.cpus,
                  machine->smp.sockets);
        return false;
    }

    if (node_count > 1 && machine->ram_size % ((uint64_t)node_count * MiB)) {
        error_setg(errp, "%s: topology '%s' splits -m evenly across %u "
                  "nodes", mc->name, t->name, node_count);
        return false;
    }

    if (!sgi_machine_validate_memory_population(
            t, machine->ram_size / node_count / MiB)) {
        error_setg(errp, "%s: topology '%s' does not accept %" PRIu64
                  " MiB per node", mc->name, t->name,
                  machine->ram_size / node_count / MiB);
        return false;
    }

    /*
     * The boot-progress indicator is a Hub trace event; Bedrock has no
     * counterpart yet, so debug-leds (on by default) stays inert for it
     * rather than refusing every origin300 launch that does not turn it
     * off explicitly.
     */
    if (t->nodes[0].asic_kind == SGI_MODEL_ASIC_HUB && s->debug_leds) {
        TraceEvent *ev = trace_event_name("sgi_hub_led");

        if (!ev) {
            error_setg(errp, "the sgi_hub_led trace event is missing, so "
                      "the firmware's boot-progress indicator cannot be "
                      "shown; build with it or pass debug-leds=off");
            return false;
        }
        trace_event_set_state_dynamic(ev, true);
    }

    /* Cache machine->possible_cpus before the node loop fills in cpu. */
    sgi_machine_possible_cpu_arch_ids(machine);

    /* One clock per processor entry; both node kinds share this rate rule. */
    s->cpu_refclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(s->cpu_refclk, t->nodes[0].cpu_slots[0].assembly->clock_hz);

    if (t->nodes[0].asic_kind != SGI_MODEL_ASIC_HUB) {
        const SGIModelIoBoard *populated_board = NULL;
        g_autofree unsigned int *widgets = NULL;
        size_t widget_count = 0;
        size_t i;
        Object *node;

        g_assert(t->node_count == 1);
        for (i = 0; i < t->io_board_count; i++) {
            if (t->io_boards[i].pci_count) {
                populated_board = &t->io_boards[i];
            }
        }
        /*
         * Widget zero is the crossbar's own local view; the rest come one
         * per xtalk attachment (a board slot, populated or not -- see
         * sn1-node.c's widget-wiring comment for why an empty slot still
         * needs one). tests/unit/test-sn-model.c already checks that every
         * Bedrock topology in sgi_model_entries[] ships a populated_board.
         */
        widgets = g_new0(unsigned int, t->xtalk_attachment_count + 1);
        widgets[widget_count++] = 0;
        for (i = 0; i < t->xtalk_attachment_count; i++) {
            widgets[widget_count++] = t->xtalk_attachments[i].widget;
        }

        node = object_new(TYPE_SGI_SN1_NODE);
        object_property_add_child(OBJECT(machine), "node0", node);
        object_unref(node);
        s->bedrock_node = DEVICE(node);
        SN1_NODE(node)->model_node = &t->nodes[0];
        SN1_NODE(node)->io_board = populated_board;
        SN1_NODE(node)->xbridge_widgets = g_steal_pointer(&widgets);
        SN1_NODE(node)->xbridge_widget_count = widget_count;
        object_property_set_link(OBJECT(node), "ram", OBJECT(machine->ram),
                                 &error_abort);
        qdev_prop_set_uint32(DEVICE(node), "nr-cpus", requested[0]);
        qdev_connect_clock_in(DEVICE(node), "cpu-refclk", s->cpu_refclk);
        sysbus_realize(SYS_BUS_DEVICE(node), &error_fatal);
        for (i = 0; i < requested[0]; i++) {
            machine->possible_cpus->cpus[i].cpu =
                CPU(SN1_NODE(node)->cpu[i]);
        }
        return true;
    }

    flash_drives = g_new0(BlockBackend *, node_count);
    for (node_index = 0; node_index < node_count; node_index++) {
        DriveInfo *drive = drive_get_by_index(IF_PFLASH, node_index);

        if (drive) {
            flash_drives[node_index] = blk_by_legacy_dinfo(drive);
            flash_count++;
        }
    }
    if (!machine->firmware && !flash_count && !qtest_enabled()) {
        error_setg(errp, "%s: supports stock-PROM boot only: pass -bios or "
                  "a pflash drive per node", mc->name);
        return false;
    }

    s->num_nodes = node_count;
    s->nodes = g_new0(SN0NodeState *, node_count);
    s->physical_nodes = g_new0(SN0ModelPhysicalNode, node_count);
    s->hub_components = g_new0(SN0ModelComponent, node_count);
    /* Indexed by io_boards[] position, one entry per board, not by node. */
    s->io_boards = g_new0(SGIXtalkBoardState *, t->io_board_count);
    s->direct_links = g_new0(SGIXtalkDirectLink, t->io_board_count);
    s->crossbars = g_new0(SGIXbowState *, t->crossbar_count);
    /* One crossbow link per node, then one per io_boards[] position. */
    s->crossbow_links =
        g_new0(SGIXtalkCrossbowLink, node_count + t->io_board_count);

    node_attachment = g_new0(unsigned int, node_count);
    board_attachment = g_new0(unsigned int, t->io_board_count);

    for (node_index = 0; node_index < node_count; node_index++) {
        g_autofree char *name = g_strdup_printf("node%u", node_index);
        Object *node = object_new(TYPE_SGI_SN0_NODE);

        object_property_add_child(OBJECT(machine), name, node);
        object_unref(node);
        s->nodes[node_index] = SN0_NODE(node);
    }

    sgi_machine_attach_module_identities(s, t);

    s->rt_refclk = clock_new(OBJECT(machine), "rt-refclk");
    clock_set_hz(s->rt_refclk, sgi_sn0_hub_hardware.rtc_clock_hz);

    s->fabric = SN0_FABRIC(qdev_new(TYPE_SN0_FABRIC_ROUTED));
    object_property_add_child(OBJECT(machine), "fabric", OBJECT(s->fabric));
    for (node_index = 0; node_index < node_count; node_index++) {
        unsigned int endpoint = sn0_fabric_add_endpoint(
            s->fabric, &s->nodes[node_index]->hub, node_index);

        g_assert(endpoint == node_index);
    }
    sgi_machine_build_routers(s, t);
    for (link_index = 0; link_index < t->link_count; link_index++) {
        const SGIModelFabricLink *link = &t->links[link_index];
        SN0FabricVertex a = {
            .kind = link->ends[0].kind == SGI_MODEL_FABRIC_ROUTER ?
                SN0_FABRIC_VERTEX_ROUTER : SN0_FABRIC_VERTEX_HUB,
            .index = link->ends[0].index,
            .port = link->ends[0].port,
        };
        SN0FabricVertex b = {
            .kind = link->ends[1].kind == SGI_MODEL_FABRIC_ROUTER ?
                SN0_FABRIC_VERTEX_ROUTER : SN0_FABRIC_VERTEX_HUB,
            .index = link->ends[1].index,
            .port = link->ends[1].port,
        };

        sn0_fabric_routed_add_link(s->fabric, link->name, a, b);
    }
    qdev_realize_and_unref(DEVICE(s->fabric), NULL, &error_fatal);

    for (node_index = 0; node_index < node_count; node_index++) {
        SN0NodeState *node = s->nodes[node_index];
        const SGIModelNode *model_node = &t->nodes[node_index];
        const SGIModelAsicIdentity *identity = &model_node->identity;
        uint64_t ram_size = machine->ram_size / node_count;
        uint16_t bank_mb[SN0_MAX_MEMORY_BANKS];
        unsigned int populated_banks;
        QList *banks_mb;
        unsigned int bank;
        Chardev *aux_chr;

        if (!sn0_model_memory_layout(&sgi_sn0_hub_hardware.memory, ram_size,
                                     bank_mb, &populated_banks)) {
            error_setg(errp, "%s: node %u's %" PRIu64 " MiB cannot be "
                      "populated from IP29 memory banks", mc->name,
                      node_index, ram_size / MiB);
            return false;
        }
        banks_mb = qlist_new();
        for (bank = 0; bank < populated_banks; bank++) {
            qlist_append_int(banks_mb, bank_mb[bank]);
        }

        s->hub_components[node_index] = (SN0ModelComponent) {
            .kind = SN0_COMPONENT_HUB,
            .ni_revision = identity->ni_revision,
            .widget_revision = identity->widget_revision,
            .serial = identity->serial,
            .i2c_controller = identity->i2c_controller,
            .system_controller = identity->system_controller,
            .i2c_controller_offset = model_node->management_adapter_offset,
            .i2c_elsc_addr = identity->i2c_elsc_addr,
            .i2c_nvram_first_addr = identity->i2c_nvram_first_addr,
            .i2c_nvram_pages = identity->i2c_nvram_pages,
            .cpu_irq_bank0 = SGI_SN0_HUB_CPU_IRQ_BANK0,
            .cpu_irq_bank1 = SGI_SN0_HUB_CPU_IRQ_BANK1,
            .cpu_irq_rt = SGI_SN0_HUB_CPU_IRQ_RT,
        };
        s->physical_nodes[node_index] = (SN0ModelPhysicalNode) {
            .id = model_node->id,
            .slot = model_node->slot,
            .hub = &s->hub_components[node_index],
            .map = sgi_sn0_hub_address_map,
            .flash = { .kind = (SN0ModelFlashKind)model_node->flash.kind },
        };

        qdev_prop_set_uint32(DEVICE(node),
                             "nasid",
                             sgi_machine_topology_nasid(t, node_index));
        node->flash_drive = flash_drives[node_index];
        node->physical = &s->physical_nodes[node_index];
        node->hardware = &sgi_sn0_hub_hardware;
        node->slave_role = model_node->slave_role;
        qdev_prop_set_uint32(DEVICE(node), "cpu-base", cpu_base);
        qdev_prop_set_uint32(DEVICE(node), "nr-cpus", requested[node_index]);
        qdev_prop_set_string(DEVICE(node), "cpu-type", machine->cpu_type);
        object_property_set_link(OBJECT(node), "ram", OBJECT(machine->ram),
                                 &error_abort);
        qdev_prop_set_uint64(DEVICE(node), "ram-start", ram_offset);
        qdev_prop_set_array(DEVICE(node), "banks-mb", banks_mb);
        qdev_connect_clock_in(DEVICE(node), "cpu-refclk", s->cpu_refclk);
        qdev_connect_clock_in(DEVICE(node), "rt-refclk", s->rt_refclk);

        qdev_prop_set_uint32(DEVICE(&node->hub), "nasid", 0);
        qdev_prop_set_uint8(DEVICE(&node->hub), "elsc-dip-switches",
                            s->elsc_dip_switches);
        if (!model_node->slave_role) {
            int line = sgi_machine_compute_serial_line(t, node_index,
                                                        SN0_ELSC_CHARDEV);

            aux_chr = serial_hd(line);
            if (aux_chr) {
                object_property_set_link(OBJECT(&node->hub), "elsc-chardev",
                                         OBJECT(aux_chr), &error_fatal);
            }
        }
        object_property_set_link(OBJECT(&node->hub), "fabric",
                                 OBJECT(s->fabric), &error_fatal);
        qdev_prop_set_uint32(DEVICE(&node->hub), "fabric-endpoint",
                            node_index);

        {
            bool has_junk_uart = (int)node_index == t->junk_uart &&
                                 s->junk_uart_present;

            qdev_prop_set_bit(DEVICE(&node->hub), "junk-uart", has_junk_uart);
            if (has_junk_uart) {
                Chardev *junk_chr = serial_hd(
                    sgi_machine_compute_serial_line(t, node_index,
                                                    SN0_JUNK_UART_CHARDEV));

                if (junk_chr) {
                    object_property_set_link(OBJECT(&node->hub),
                                             "junk-uart-chardev",
                                             OBJECT(junk_chr), &error_fatal);
                }
            }
        }

        cpu_base += requested[node_index];
        ram_offset += ram_size;
    }
    g_assert(ram_offset == machine->ram_size);

    if (!sgi_machine_index_xtalk_attachments(t, node_attachment,
                                             board_attachment, errp)) {
        return false;
    }

    /*
     * Before any node realizes: the Hub's own "xtalk-io" property, once
     * set, cannot change after this node's realize, so a crossbow-attached
     * node's own link claims it here, on a proxy of its own -- the
     * Crossbow itself, and its real packet ingress, do not exist yet.
     */
    if (t->crossbar_count) {
        sgi_machine_build_crossbars(s, t);
        for (node_index = 0; node_index < node_count; node_index++) {
            if (t->xtalk_attachments[node_attachment[node_index]]
                    .crossbar_index >= 0) {
                g_autofree char *name =
                    g_strdup_printf("node%u-packet-proxy", node_index);

                if (!sgi_xtalk_crossbow_link_prepare_hub(
                        &s->crossbow_links[node_index], OBJECT(s), name,
                        s->nodes[node_index], errp)) {
                    return false;
                }
            }
        }
    }

    for (node_index = 0; node_index < node_count; node_index++) {
        SN0NodeState *node = s->nodes[node_index];
        unsigned int slice;

        sysbus_realize(SYS_BUS_DEVICE(node), &error_fatal);
        for (slice = 0; slice < requested[node_index]; slice++) {
            machine->possible_cpus->cpus[cpu_index++].cpu =
                CPU(node->cpu[slice]);
        }
    }
    g_assert(cpu_index == machine->smp.cpus);

    sn0_map_cpu_big_windows(OBJECT(s), s->nodes, s->num_nodes,
                            &s->big_window_route);

    /*
     * The two-chassis Origin 200's two Hub-embedded ELSCs share
     * arbitration; a shared-controller topology (Origin 2000) has no
     * per-node ELSC here (sgi_machine_build_controllers builds that).
     */
    if (node_count == 2) {
        SGIElscState *master = NULL;
        SGIElscState *slave = NULL;

        for (node_index = 0; node_index < node_count; node_index++) {
            if (t->nodes[node_index].slave_role) {
                slave = s->nodes[node_index]->hub.elsc;
            } else {
                master = s->nodes[node_index]->hub.elsc;
            }
        }
        g_assert(master);
        g_assert(slave);
        sgi_elsc_share_arbitration(master, slave, 5);
    }

    sn0_fabric_build(s->fabric);

    /*
     * Every node is realized now, so a direct board's claim view exists
     * and a Crossbow-attached node's physical map can be read.
     */
    for (board_index = 0; board_index < t->io_board_count; board_index++) {
        const SGIModelXtalkAttachment *attachment =
            &t->xtalk_attachments[board_attachment[board_index]];

        if (!sgi_machine_instantiate_io_board(s, t, board_index, attachment,
                                              errp)) {
            return false;
        }
    }
    for (node_index = 0; node_index < node_count; node_index++) {
        const SGIModelXtalkAttachment *attachment =
            &t->xtalk_attachments[node_attachment[node_index]];

        if (attachment->crossbar_index >= 0) {
            g_autofree char *name = g_strdup_printf("node%u", node_index);

            if (!sgi_xtalk_crossbow_link_attach_hub(
                    &s->crossbow_links[node_index], OBJECT(s), name,
                    s->crossbars[attachment->crossbar_index],
                    attachment->widget, errp)) {
                return false;
            }
        }
    }
    if (!sgi_machine_build_controllers(s, t, errp)) {
        return false;
    }

    if (!sgi_machine_load_flash(machine, t, s, errp)) {
        return false;
    }

    /*
     * Every port any node or board this run builds attaches through has
     * attached now, so every Crossbow can realize in one pass, here,
     * rather than waiting for a machine-init-done notifier: this
     * experiment does not support a crosstalk board attaching later
     * (see docs/plans/sn-machine-model.md, step 1's own report).
     */
    if (!sgi_machine_realize_crossbars(s, t, node_attachment, board_attachment,
                                       errp)) {
        return false;
    }
    return true;
}

void sgi_machine_init(MachineState *machine)
{
    sgi_machine_realize(machine, &error_fatal);
}

static void sgi_machine_reset(MachineState *machine, ResetType type)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    const SGIModelTopology *t = sgi_machine_topology(machine);
    uint32_t mode_bits;
    CPUState *cs;
    unsigned int node, slice;

    /*
     * The Bedrock node resets itself: "start-powered-off" needs its
     * cpu_reset() to run before qemu_devices_reset(), the reverse of
     * the Hub's own order below, so it cannot share this prefix.
     */
    if (t && t->nodes[0].asic_kind != SGI_MODEL_ASIC_HUB) {
        sn1_node_full_reset(SN1_NODE(s->bedrock_node), type);
        return;
    }

    mode_bits = t ? t->nodes[0].cpu_slots[0].assembly->mode_bits : 0;
    qemu_devices_reset(type);
    CPU_FOREACH(cs) {
        CPUMIPSState *env = &MIPS_CPU(cs)->env;

        cpu_reset(cs);
        env->CP0_Config0 =
            (env->CP0_Config0 & ~(uint32_t)SN0_PROCESSOR_MODE_MASK) |
            (mode_bits & SN0_PROCESSOR_MODE_MASK);
    }
    for (node = 0; node < s->num_nodes; node++) {
        SN0NodeState *node_state = s->nodes[node];

        for (slice = 0; slice < node_state->nr_cpus; slice++) {
            sgi_hub_set_cpu_enable(&node_state->hub, slice, true, true);
        }
    }
}

/* ---- Machine properties (once, on the family base) ---- */

static char *sgi_machine_get_topology(Object *obj, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    return g_strdup(s->topology_name ? s->topology_name : "");
}

static void sgi_machine_set_topology(Object *obj, const char *value,
                                     Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    g_free(s->topology_name);
    s->topology_name = g_strdup(value);
}

static bool sgi_machine_get_debug_leds(Object *obj, Error **errp)
{
    return SGI_MACHINE(obj)->debug_leds;
}

static void sgi_machine_set_debug_leds(Object *obj, bool value, Error **errp)
{
    SGI_MACHINE(obj)->debug_leds = value;
}

static bool sgi_machine_get_junk_uart(Object *obj, Error **errp)
{
    return SGI_MACHINE(obj)->junk_uart_present;
}

static void sgi_machine_set_junk_uart(Object *obj, bool value, Error **errp)
{
    SGI_MACHINE(obj)->junk_uart_present = value;
}

static void sgi_machine_get_elsc_dip_switches(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    uint8_t value = SGI_MACHINE(obj)->elsc_dip_switches;

    visit_type_uint8(v, name, &value, errp);
}

static void sgi_machine_set_elsc_dip_switches(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    uint8_t value;

    if (visit_type_uint8(v, name, &value, errp)) {
        SGI_MACHINE(obj)->elsc_dip_switches = value;
    }
}

static void sgi_machine_instance_init(Object *obj)
{
    SGI_MACHINE(obj)->debug_leds = true;
}

static void sgi_machine_instance_finalize(Object *obj)
{
    SGIMachineState *s = SGI_MACHINE(obj);
    size_t i;

    g_free(s->nodes);
    g_free(s->physical_nodes);
    g_free(s->hub_components);
    g_free(s->crossbars);
    g_free(s->io_boards);
    g_free(s->direct_links);
    g_free(s->crossbow_links);
    g_free(s->topology_name);
    for (i = 0; i < ARRAY_SIZE(sgi_machine_identity_records); i++) {
        qapi_free_strList(*sgi_machine_identity_slot(
            obj, (void *)&sgi_machine_identity_records[i]));
    }
}

static void sgi_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = sgi_machine_init;
    mc->reset = sgi_machine_reset;
    mc->possible_cpu_arch_ids = sgi_machine_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = sgi_machine_cpu_index_to_props;
    mc->get_default_cpu_node_id = sgi_machine_get_default_cpu_node_id;
    mc->default_nic = "sgi-ioc3-eth";
    mc->block_default_type = IF_SCSI;
    /* Node memory is memdev-backed -numa nodes; bare mem= has no backend. */
    mc->numa_mem_supported = false;

    object_class_property_add_str(oc, "topology", sgi_machine_get_topology,
                                  sgi_machine_set_topology);
    object_class_property_set_description(oc, "topology",
        "Select the product's wiring: nodes, modules, cabling and I/O "
        "population. Defaults to the product's shipped configuration; "
        "an unrecognized name (including 'help') reports every topology "
        "this product accepts.");

    object_class_property_add_bool(oc, "debug-leds", sgi_machine_get_debug_leds,
                                   sgi_machine_set_debug_leds);
    object_class_property_set_description(oc, "debug-leds",
        "Show the firmware's boot-progress indicator, decoded. On by "
        "default; the codes go to the log destination.");

    object_class_property_add_bool(oc, "junk-uart", sgi_machine_get_junk_uart,
                                   sgi_machine_set_junk_uart);
    object_class_property_set_description(oc, "junk-uart",
        "Fit the junk-bus UART, a manufacturing/bring-up serial port a "
        "shipped machine does not have. Off by default; on, it takes the "
        "fourth character device (serial_hd index 3).");

    object_class_property_add(oc, "elsc-dip-switches", "uint8",
                              sgi_machine_get_elsc_dip_switches,
                              sgi_machine_set_elsc_dip_switches, NULL, NULL);
    object_class_property_set_description(oc, "elsc-dip-switches",
        "Set the eight physical ELSC DIP switches as asserted bits. The "
        "controller reports their active-low wire value to firmware.");

    for (size_t i = 0; i < ARRAY_SIZE(sgi_machine_identity_records); i++) {
        const SGIMachineIdentityRecords *records =
            &sgi_machine_identity_records[i];

        object_class_property_add(oc, records->name, "strList",
                                  sgi_machine_get_identity_records,
                                  sgi_machine_set_identity_records, NULL,
                                  (void *)records);
        object_class_property_set_description(oc, records->name,
                                              records->description);
    }
}

/* ---- Product classes ---- */

static const char * const sgi_sn0_r10000_cpu_types[] = {
    MIPS_CPU_TYPE_NAME("R10000"),
    NULL,
};

static void origin200_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const SGIModelTopology *t = sgi_model_find_topology("origin200");

    g_assert(t);
    smc->product = "origin200";
    smc->default_topology = "origin200";
    /* sn0-origin200-model.c's own values, real IP27 PROM fields. */
    smc->firmware_machine_type = 1;
    smc->flash_read_cycles = 15;
    smc->flash_write_cycles = 4;
    mc->desc = t->description;
    mc->valid_cpu_types = sgi_sn0_r10000_cpu_types;
    mc->default_cpus = sgi_machine_topology_default_cpus(t);
    mc->max_cpus = sgi_machine_product_max_cpus("origin200");
    mc->default_ram_size = sgi_machine_topology_default_ram(t);
    mc->default_ram_id = "sn0.node0.ram";
}

static void origin2000_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const SGIModelTopology *t = sgi_model_find_topology("origin2000-module");

    g_assert(t);
    smc->product = "origin2000";
    smc->default_topology = "origin2000-module";
    mc->desc = t->description;
    mc->valid_cpu_types = sgi_sn0_r10000_cpu_types;
    mc->default_cpus = sgi_machine_topology_default_cpus(t);
    mc->max_cpus = sgi_machine_product_max_cpus("origin2000");
    mc->default_ram_size = sgi_machine_topology_default_ram(t);
    mc->default_ram_id = "sn0.ram";
}

/*
 * A bare -M origin2000 needs sockets/cores preset before smp_parse runs;
 * every other product's default topology is one node.
 */
static void origin2000_instance_init(Object *obj)
{
    MachineState *machine = MACHINE(obj);
    const SGIModelTopology *t = sgi_model_find_topology("origin2000-module");

    g_assert(t);
    machine->smp.sockets = t->node_count;
    machine->smp.cores = t->nodes[0].cpu_slot_count;
}

static const TypeInfo sgi_machine_types[] = {
    {
        .name              = TYPE_SGI_SN_MACHINE,
        .parent            = TYPE_MACHINE,
        .abstract          = true,
        .instance_size     = sizeof(SGIMachineState),
        .instance_init     = sgi_machine_instance_init,
        .instance_finalize = sgi_machine_instance_finalize,
        .class_size        = sizeof(SGIMachineClass),
        .class_init        = sgi_machine_class_init,
    },
    {
        .name       = MACHINE_TYPE_NAME("origin200"),
        .parent     = TYPE_SGI_SN_MACHINE,
        .class_init = origin200_class_init,
    },
    {
        .name          = MACHINE_TYPE_NAME("origin2000"),
        .parent        = TYPE_SGI_SN_MACHINE,
        .instance_init = origin2000_instance_init,
        .class_init    = origin2000_class_init,
    },
};

DEFINE_TYPES(sgi_machine_types)
