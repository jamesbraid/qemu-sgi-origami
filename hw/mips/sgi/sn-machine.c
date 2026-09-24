/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The abstract SGI Scalable Node family machine and its builder over an
 * SGIModelTopology from the compiled catalogue (qapi/sgi-sn.json,
 * embedded and parsed by hw/mips/sgi/sn-catalogue.c). Origin 200 is the
 * first product on this base; see docs/plans/sn-machine-model.md, "QEMU".
 *
 * Fixed here rather than carried in the descriptor, because they are Hub
 * hardware facts rather than per-configuration data: the Hub's physical
 * address map, its memory-bank geometry and its three hardwired CPU
 * interrupt inputs.
 *
 * Origin 200's onboard base I/O sits directly on its one node's
 * crosstalk port: sn0.c owns it (sn0_node_build_baseio(), called from
 * this file's own io_boards[] loop below, once every node has realized)
 * since it is a fitted part of the node's own System Board, not a
 * removable card. The Origin 2000 module's BaseIO card (sgi-xio-baseio)
 * sits behind a Crossbow this builder owns, and is a sgi-xio-board
 * instance built and realized here, before the nodes' Crossbows realize,
 * like every other card class the base-io-device step's design table
 * names. sgi-pci-expansion (board-pci-expansion.c) is never built here:
 * it only exists as a `-device` board, plugged in after this builder has
 * returned. Each Crossbow this builder realizes gets one spare widget
 * held back from startup-links-alive (sgi_machine_build_crossbars())
 * with its own fabric proxy prepared against it, finished from a
 * machine-init-done notifier once every Crossbow has realized -- see
 * that notifier's own comment below for exactly what a late board's
 * proxy can and cannot reach through it.
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
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi-xtalk-port.h"
#include "hw/mips/sgi/baseio.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xtalk-link.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/misc/sgi-hub.h"
#include "hw/misc/sgi-mgras.h"
#include "hw/misc/sgi-router.h"
#include "hw/misc/sgi-sn0-module-management.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci/pci.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/system.h"
#include "trace/control.h"
#include "cpu.h"
#include "hw/mips/sgi/sn-catalogue.h"
#include "hw/mips/sgi/sn-machine.h"
#include "hw/mips/sgi/sn1-node.h"
#include "hw/mips/sgi/net-cable.h"
#include "sn0.h"

/*
 * The compiled catalogue's QAPI-generated types (qapi/sgi-sn.json) hold
 * every repeated field as a NULL-terminated linked list, where the old
 * hand-written sn-model.h used a flat array and a count; the builder
 * below still wants positional indexing (a node's own position in
 * t->nodes, a module's node by position in its own nodes list), so each
 * helper walks a list once into a freshly allocated array of pointers --
 * an index table built once per caller, per docs/plans/sn-machine-model.md's
 * own "list walks or index tables" instruction for this step -- rather
 * than threading list-walk logic through every call site below. Callers
 * hold the result no longer than their own scope (g_autofree).
 */
#define SGI_MODEL_DEFINE_LIST_ARRAY(Type, name)                             \
static const Type **sgi_model_##name##_array(const Type##List *list,        \
                                             size_t *count)                  \
{                                                                            \
    const Type##List *entry;                                                \
    Type **array;                                                           \
    size_t i = 0, n = 0;                                                    \
                                                                              \
    for (entry = list; entry; entry = entry->next) {                        \
        n++;                                                                \
    }                                                                       \
    array = g_new(Type *, n ? n : 1);                                       \
    for (entry = list; entry; entry = entry->next) {                        \
        array[i++] = entry->value;                                          \
    }                                                                       \
    *count = n;                                                             \
    return (const Type **)array;                                           \
}

SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelNode, node)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelModule, module)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelRouter, router)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelCable, cable)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelCrossbar, crossbar)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelXtalkAttachment, xtalk_attachment)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelIoBoard, io_board)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelController, controller)

static unsigned int *sgi_model_array_uint32(const uint32List *list, size_t *count)
{
    const uint32List *entry;
    unsigned int *array;
    size_t i = 0, n = 0;

    for (entry = list; entry; entry = entry->next) {
        n++;
    }
    array = g_new(unsigned int, n ? n : 1);
    for (entry = list; entry; entry = entry->next) {
        array[i++] = entry->value;
    }
    *count = n;
    return array;
}

static uint64_t *sgi_model_array_uint64(const uint64List *list, size_t *count)
{
    const uint64List *entry;
    uint64_t *array;
    size_t i = 0, n = 0;

    for (entry = list; entry; entry = entry->next) {
        n++;
    }
    array = g_new(uint64_t, n ? n : 1);
    for (entry = list; entry; entry = entry->next) {
        array[i++] = entry->value;
    }
    *count = n;
    return array;
}

#define TYPE_SGI_XIO_BASEIO "sgi-xio-baseio"
#define TYPE_SGI_PCI_EXPANSION "sgi-pci-expansion"
#define TYPE_SGI_PCI_SHOEHORN "sgi-pci-shoehorn"

struct SGIMachineState {
    MachineState parent_obj;

    char *topology_name;
    char *baseio_preset;

    unsigned int num_nodes;
    SN0NodeState **nodes;
    MemoryRegion *big_window_route;

    /* This machine's Routers, in descriptor order, so a cable's own
     * SGIModelLinkEnd index addresses them. */
    SGIRouterState **routers;
    unsigned int router_count;
    Clock *cpu_refclk;
    Clock *rt_refclk;

    /* Parented and realized as soon as every port attached to it is. */
    SGIXbowState **crossbars;

    /*
     * One sgi-xio-board per Crossbow-attached io_boards[] entry, built
     * before its own Crossbow. A direct-attach entry (Origin 200's
     * onboard I/O, built by sn0_node_build_baseio() instead) leaves its
     * own slot NULL: there is no board wrapper for it any more.
     */
    SGIXioBoardState **io_boards;

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

    SGIMgrasState *mgras;
    const SGIModelXtalkSlot *mgras_slot;
    MemoryRegion mgras_packet_proxy;
    MemoryRegion mgras_packet_alias;

    /*
     * One spare fabric proxy per crossbars[] entry, for a `-device` board
     * plugged in after this builder has returned: prepared here (against
     * a widget sgi_machine_build_crossbars() held out of
     * startup-links-alive) and finished from machine_done below, once
     * every Crossbow has realized. spare_fabric_widget holds the chosen
     * widget number, or 0 (never a valid widget: see
     * SGI_XBOW_FIRST_WIDGET) where a topology leaves no widget free.
     */
    SGIXtalkCrossbowLink *spare_fabric_links;
    unsigned int *spare_fabric_widget;
    Notifier machine_done;

    bool debug_leds;
    bool junk_uart_present;
    uint8_t elsc_dip_switches;

    /*
     * The Bedrock nodes, for a topology whose node kind is Bedrock: one
     * per t->nodes entry, the way s->nodes is for a Hub topology. The
     * shipped Origin 300 carries one; a second is descriptor data the
     * builder accepts without exercising it (see sn-builder-test.c's
     * synthetic two-node fixture).
     */
    DeviceState **bedrock_nodes;

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
 * The compiled catalogue holds every product's topologies in one list,
 * so a name match alone is not enough: topology= is product-scoped
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
    const uint32List *entry;

    for (entry = population->cpus_per_node; entry; entry = entry->next) {
        total += entry->value;
    }
    return total;
}

unsigned int sgi_machine_product_max_cpus(const char *product)
{
    unsigned int max_cpus = 0;
    const SGIModelTopologyList *entry;

    for (entry = sgi_model_catalogue()->topologies; entry; entry = entry->next) {
        const SGIModelTopology *t = entry->value;
        const SGIModelCpuPopulationList *population;

        if (!sgi_machine_topology_matches_product(t, product)) {
            continue;
        }
        for (population = t->supported_cpu_populations; population;
             population = population->next) {
            unsigned int total =
                sgi_machine_population_total(population->value);

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
    g_autofree const SGIModelNode **nodes = NULL;
    unsigned int slot_min;
    size_t node_count, i, j;

    nodes = sgi_model_node_array(t->nodes, &node_count);
    for (i = 0; i < node_count; i++) {
        for (j = 0; j < i; j++) {
            if (nodes[i]->slot == nodes[j]->slot) {
                return node_index;
            }
        }
    }
    slot_min = nodes[0]->slot;
    for (i = 1; i < node_count; i++) {
        slot_min = MIN(slot_min, nodes[i]->slot);
    }
    return nodes[node_index]->slot - slot_min;
}

uint64_t sgi_machine_topology_default_ram(const SGIModelTopology *t)
{
    g_autofree const SGIModelNode **nodes = NULL;
    uint64_t total_mib = 0;
    size_t node_count, i;

    nodes = sgi_model_node_array(t->nodes, &node_count);
    for (i = 0; i < node_count; i++) {
        g_autofree uint64_t *bank_mib = NULL;
        size_t bank_count, b;

        bank_mib = sgi_model_array_uint64(nodes[i]->bank_mib, &bank_count);
        for (b = 0; b < bank_count; b++) {
            total_mib += bank_mib[b];
        }
    }
    return total_mib * MiB;
}

unsigned int sgi_machine_topology_default_cpus(
    const SGIModelTopology *t)
{
    g_assert(t->supported_cpu_populations);
    return sgi_machine_population_total(t->supported_cpu_populations->value);
}

/* A topology's own first node: every product measured has at least one. */
static const SGIModelNode *sgi_machine_topology_node0(const SGIModelTopology *t)
{
    g_assert(t->nodes);
    return t->nodes->value;
}

/* Every topology's node_count, without an index table for one integer. */
static size_t sgi_machine_topology_node_count(const SGIModelTopology *t)
{
    const SGIModelNodeList *entry;
    size_t count = 0;

    for (entry = t->nodes; entry; entry = entry->next) {
        count++;
    }
    return count;
}

/* Likewise for io_boards, without allocating an index table for one count. */
static size_t sgi_machine_topology_io_board_count(const SGIModelTopology *t)
{
    const SGIModelIoBoardList *entry;
    size_t count = 0;

    for (entry = t->io_boards; entry; entry = entry->next) {
        count++;
    }
    return count;
}

/* Likewise for crossbars. */
static size_t sgi_machine_topology_crossbar_count(const SGIModelTopology *t)
{
    const SGIModelCrossbarList *entry;
    size_t count = 0;

    for (entry = t->crossbars; entry; entry = entry->next) {
        count++;
    }
    return count;
}

/*
 * Every R10000 and R14000 is a single-core socket, so -smp N names a
 * total, never a shape: which node each of the N sockets sits on comes
 * from the topology's own accepted population, the way the population
 * table already records it. Find the population whose total is
 * total_cpus; *second names a second match so the caller can refuse an
 * ambiguous total instead of guessing between them.
 */
static const SGIModelCpuPopulation *sgi_machine_find_population_by_total(
    const SGIModelTopology *t, unsigned int total_cpus,
    const SGIModelCpuPopulation **second)
{
    size_t node_count = sgi_machine_topology_node_count(t);
    const SGIModelCpuPopulation *match = NULL;
    const SGIModelCpuPopulationList *entry;

    *second = NULL;
    for (entry = t->supported_cpu_populations; entry; entry = entry->next) {
        const SGIModelCpuPopulation *population = entry->value;
        g_autofree unsigned int *cpus_per_node = NULL;
        size_t population_node_count;

        cpus_per_node = sgi_model_array_uint32(population->cpus_per_node,
                                               &population_node_count);
        if (population_node_count != node_count) {
            continue;
        }
        if (sgi_machine_population_total(population) != total_cpus) {
            continue;
        }
        if (!match) {
            match = population;
        } else {
            *second = population;
            break;
        }
    }
    return match;
}

/* "1, 2 or 4": the topology's accepted totals, for a refusal message. */
static char *sgi_machine_accepted_totals(const SGIModelTopology *t)
{
    GString *out = g_string_new(NULL);
    const SGIModelCpuPopulationList *entry;
    bool first = true;

    for (entry = t->supported_cpu_populations; entry; entry = entry->next) {
        unsigned int total = sgi_machine_population_total(entry->value);

        if (!first) {
            g_string_append(out, entry->next ? ", " : " or ");
        }
        g_string_append_printf(out, "%u", total);
        first = false;
    }
    return g_string_free(out, false);
}

/*
 * Validate that the per-node memory size (-m split evenly across every
 * node) matches this node's accepted population, per
 * SGIModelNode.accepted-memory-mib: a present list is the DIMM kit
 * sizes the node accepts, and an absent one means the node's own
 * shipped bank-mib total is the only accepted size, not an open
 * fitting rule.
 */
static bool sgi_machine_validate_memory_population(
    const SGIModelTopology *t, uint64_t per_node_mib)
{
    g_autofree const SGIModelNode **nodes = NULL;
    size_t node_count, node_index;

    nodes = sgi_model_node_array(t->nodes, &node_count);
    for (node_index = 0; node_index < node_count; node_index++) {
        const SGIModelNode *node = nodes[node_index];

        if (!node->accepted_memory_mib) {
            g_autofree uint64_t *bank_mib = NULL;
            uint64_t shipped_mib = 0;
            size_t bank_count, bank;

            bank_mib = sgi_model_array_uint64(node->bank_mib, &bank_count);
            for (bank = 0; bank < bank_count; bank++) {
                shipped_mib += bank_mib[bank];
            }
            if (per_node_mib != shipped_mib) {
                return false;
            }
        } else {
            g_autofree uint64_t *accepted = NULL;
            size_t accepted_count, i;
            bool found = false;

            accepted = sgi_model_array_uint64(node->accepted_memory_mib,
                                              &accepted_count);
            for (i = 0; i < accepted_count; i++) {
                if (accepted[i] == per_node_mib) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
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
    unsigned int node_count = t ? sgi_machine_topology_node_count(t) : 1;
    const SGIModelCpuPopulation *population = NULL;
    g_autofree unsigned int *cpus_per_node = NULL;
    g_autofree unsigned int *node_of = NULL;
    size_t population_node_count = 0;
    unsigned int node_index, slice, index;

    if (machine->possible_cpus) {
        assert(machine->possible_cpus->len == max_cpus);
        return machine->possible_cpus;
    }
    machine->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                       sizeof(CPUArchId) * max_cpus);
    machine->possible_cpus->len = max_cpus;

    if (t) {
        const SGIModelCpuPopulation *second;

        population = sgi_machine_find_population_by_total(t, max_cpus,
                                                           &second);
    }
    if (population) {
        cpus_per_node = sgi_model_array_uint32(population->cpus_per_node,
                                               &population_node_count);
    }

    /*
     * One node per slot in the selected population's order, then node 0
     * for whatever -smp overshoots it (an unrecognized total or a
     * second matching population): sgi_machine_realize reports the
     * mismatch clearly, so this table just needs to be complete for
     * NUMA parsing to reject from meanwhile. Every socket is its own
     * arch-id slot; core and thread are always 0.
     */
    node_of = g_new0(unsigned int, max_cpus);
    index = 0;
    if (population) {
        for (node_index = 0; node_index < node_count &&
             node_index < population_node_count && index < max_cpus;
             node_index++) {
            for (slice = 0; slice < cpus_per_node[node_index]
                 && index < max_cpus; slice++, index++) {
                node_of[index] = node_index;
            }
        }
    }
    for (; index < max_cpus; index++) {
        node_of[index] = 0;
    }

    for (index = 0; index < max_cpus; index++) {
        CPUArchId *slot = &machine->possible_cpus->cpus[index];

        slot->type = machine->cpu_type;
        slot->vcpus_count = 1;
        slot->arch_id = index;
        slot->props.has_socket_id = true;
        slot->props.socket_id = index;
        slot->props.has_core_id = true;
        slot->props.core_id = 0;
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
    size_t node_count = sgi_machine_topology_node_count(t);
    size_t io_board_count = sgi_machine_topology_io_board_count(t);
    g_autofree const SGIModelXtalkAttachment **attachments = NULL;
    size_t attachment_count;
    g_autofree bool *claimed = g_new0(bool, node_count);
    unsigned int board = 0;
    size_t i;

    attachments = sgi_model_xtalk_attachment_array(t->xtalk_attachments,
                                                    &attachment_count);

    for (i = 0; i < attachment_count; i++) {
        const SGIModelXtalkAttachment *attachment = attachments[i];
        unsigned int node_index = attachment->node_index;

        if (node_index >= node_count) {
            error_setg(errp, "topology '%s' crosstalk attachment %zu names "
                      "node %u, past its %zu nodes", t->name, i, node_index,
                      node_count);
            return false;
        }
        if (attachment->crossbar_index < 0) {
            node_attachment[node_index] = i;
            claimed[node_index] = true;
            if (board >= io_board_count) {
                error_setg(errp, "topology '%s' names more direct-attach or "
                          "board-owning crosstalk attachments than its %zu "
                          "io board(s)", t->name, io_board_count);
                return false;
            }
            board_attachment[board++] = i;
            continue;
        }
        if (!claimed[node_index]) {
            claimed[node_index] = true;
            node_attachment[node_index] = i;
        } else {
            if (board >= io_board_count) {
                error_setg(errp, "topology '%s' names more direct-attach or "
                          "board-owning crosstalk attachments than its %zu "
                          "io board(s)", t->name, io_board_count);
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
    for (i = 0; i < node_count; i++) {
        if (!claimed[i]) {
            error_setg(errp, "topology '%s' node %zu names no direct-attach "
                      "I/O board or crosstalk attachment", t->name, i);
            return false;
        }
    }
    if (board != io_board_count) {
        error_setg(errp, "topology '%s' names %zu io board(s) but only %u "
                  "crosstalk attachment(s) own one", t->name,
                  io_board_count, board);
        return false;
    }
    return true;
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
 * A crossbar's manufacturer id and part number are fixed by which chip
 * it is (docs/designs/sn-machine-model-architecture.md's own
 * SGIModelCrossbarKind split: a standalone Crossbow versus an XBridge's
 * internal crossbar section), the same on every machine fitting either,
 * so the compiled catalogue no longer carries them. The manufacturer id
 * is zero for both kinds measured so far; the part number is what every
 * measured configuration's descriptor named before the ownership-rule
 * trim.
 */
#define SGI_MACHINE_CROSSBAR_MANUFACTURER 0

static uint32_t sgi_machine_crossbar_part(SGIModelCrossbarKind kind)
{
    switch (kind) {
    case SGI_MODEL_CROSSBAR_KIND_XBOW:
        return 0;
    case SGI_MODEL_CROSSBAR_KIND_XBRIDGE:
        return 0xd000;
    default:
        g_assert_not_reached();
    }
}

static void sgi_machine_build_crossbars(SGIMachineState *s,
                                        const SGIModelTopology *t)
{
    g_autofree const SGIModelCrossbar **crossbars = NULL;
    g_autofree const SGIModelXtalkAttachment **attachments = NULL;
    size_t crossbar_count, attachment_count, i;
    unsigned int widget;

    crossbars = sgi_model_crossbar_array(t->crossbars, &crossbar_count);
    attachments = sgi_model_xtalk_attachment_array(t->xtalk_attachments,
                                                    &attachment_count);
    for (i = 0; i < crossbar_count; i++) {
        const SGIModelCrossbar *descriptor = crossbars[i];
        DeviceState *crossbow = qdev_new(TYPE_SGI_XBOW);
        g_autofree char *name = g_strdup_printf("crossbow%zu", i);
        uint16_t startup_alive = 0;
        size_t attachment_index;

        /*
         * The widgets this topology attaches: the only ports with a
         * widget physically present, and each reported alive at startup.
         * An empty XIO slot has no widget, so its port reads presence
         * clear (docs/datasheets/xbow.md, link auxiliary status bit 5);
         * reporting it present would make the PROM find a widget whose
         * link never came up and diagnose it as a bad IO link. The spare
         * widget held back below becomes present only when a board
         * claims it (sgi_machine_done()).
         */
        for (attachment_index = 0; attachment_index < attachment_count;
             attachment_index++) {
            const SGIModelXtalkAttachment *attachment =
                attachments[attachment_index];

            if ((unsigned int)attachment->crossbar_index == i) {
                startup_alive |= 1u << attachment->widget;
            }
        }
        if (s->mgras_slot && i == s->mgras_slot->crossbar_index) {
            startup_alive |= 1u << s->mgras_slot->widget;
        }

        s->crossbars[i] = SGI_XBOW(crossbow);
        object_property_add_child(OBJECT(s), name, OBJECT(crossbow));
        object_unref(OBJECT(crossbow));
        qdev_prop_set_uint64(crossbow, "span", SN0_XTALK_WIDGET_SIZE);
        qdev_prop_set_bit(crossbow,
                          "complete-posted-error-address-writes", true);
        qdev_prop_set_uint32(crossbow, "revision",
                             descriptor->widget_revision);
        qdev_prop_set_uint32(crossbow, "part",
                             sgi_machine_crossbar_part(descriptor->kind));
        qdev_prop_set_uint32(crossbow, "manufacturer",
                             SGI_MACHINE_CROSSBAR_MANUFACTURER);
        qdev_prop_set_uint16(crossbow, "links-present", startup_alive);
        qdev_prop_set_uint16(crossbow, "startup-links-alive", startup_alive);
        /*
         * Stock PROM resets populated links and expects one alive before
         * the reset to return to service after the pulse.
         */
        qdev_prop_set_uint16(crossbow, "link-reset-restore-alive",
                             startup_alive);

        /*
         * Hold one populated-at-startup widget back as a spare fabric
         * proxy for a `-device` board plugged in once this builder has
         * returned (see the file header comment and the machine-done
         * notifier below): the lowest widget this topology's own
         * attachments leave unpopulated, or none if the topology fills
         * every widget. spare_fabric_widget[i] stays 0 -- never a valid
         * widget number -- when none is free.
         */
        for (widget = SGI_XBOW_FIRST_WIDGET; widget < SGI_XBOW_WIDGETS;
             widget++) {
            if (!(startup_alive & (1u << widget))) {
                break;
            }
        }
        if (widget < SGI_XBOW_WIDGETS) {
            g_autofree char *spare_name =
                g_strdup_printf("crossbow%zu-spare-fabric", i);

            sgi_xtalk_crossbow_link_prepare(&s->spare_fabric_links[i],
                                            OBJECT(s), spare_name);
            s->spare_fabric_widget[i] = widget;
        }
    }
}

/*
 * The onboard IOC3's own separate one-wire identity part (the carrier's
 * chip identity is a different part); NULL when this preset's IOC3 head
 * carries no population, leaving sgi-baseio's own class default.
 */
static const uint64_t *sgi_machine_io_board_ioc3_serial(
    const SGIModelIoBoard *record)
{
    return record->has_ioc3_serial ? &record->ioc3_serial : NULL;
}

/*
 * IP29's onboard I/O: direct-attach, one per Origin 200 node. sn0.c owns
 * the Bridge, the sgi-baseio behind it and the direct-link wiring (see
 * sn0_node_build_baseio()'s own comment for why); this function resolves
 * every fitted-part value and backend id the descriptor and command line
 * carry, the same policy the Crossbow-attached BaseIO card below applies.
 */
static bool sgi_machine_instantiate_ip29_io(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelIoBoard *record, unsigned int board_index,
    const SGIModelXtalkAttachment *attachment, Error **errp)
{
    SN0NodeState *node = s->nodes[attachment->node_index];
    const uint64_t *ioc3_serial = sgi_machine_io_board_ioc3_serial(record);
    /*
     * "pci" for the first onboard board, "pci.<N>" for a second: a
     * two-chassis Origin 200's two onboard Bridges would otherwise share
     * a migration-visible PCI bus name.
     */
    g_autofree char *bus_name = board_index == 0 ? g_strdup("pci") :
        g_strdup_printf("pci.%u", board_index);
    g_autofree char *nvram_id = g_strdup_printf("sgi-nvram%u", board_index);
    DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, board_index);
    BlockBackend *nvram_blk = nvram_dinfo ?
        blk_by_legacy_dinfo(nvram_dinfo) : blk_by_name(nvram_id);
    Chardev *chardev_a = serial_hd(sgi_machine_compute_serial_line(
        t, attachment->node_index, SN0_IOC3_SERIAL_A_CHARDEV));
    Chardev *chardev_b = serial_hd(sgi_machine_compute_serial_line(
        t, attachment->node_index, SN0_IOC3_SERIAL_B_CHARDEV));
    Chardev *record_a = serial_hd(sgi_machine_compute_serial_line(
        t, attachment->node_index, SN0_IOC3_RECORD_A_CHARDEV));
    Chardev *record_b = serial_hd(sgi_machine_compute_serial_line(
        t, attachment->node_index, SN0_IOC3_RECORD_B_CHARDEV));
    const char *parallel_id = board_index < MAX_PARALLEL_PORTS &&
        parallel_hds[board_index] ? parallel_hds[board_index]->label : NULL;

    return sn0_node_build_baseio(
        node, OBJECT(s), &s->direct_links[board_index], record->serial,
        ioc3_serial, bus_name, nvram_blk ? blk_name(nvram_blk) : NULL,
        chardev_a ? chardev_a->label : NULL,
        chardev_b ? chardev_b->label : NULL,
        record_a ? record_a->label : NULL,
        record_b ? record_b->label : NULL, parallel_id,
        attachment->widget, errp);
}

/*
 * The Origin 2000/Onyx2 BaseIO card: Crossbow-attached, an sgi-xio-baseio
 * instance (an sgi-xio-board, so it attaches through
 * sgi_xtalk_crossbow_link_attach_widget() like every other XIO card; the
 * Crossbow itself realizes later, once every board and node that
 * attaches to it has -- see sgi_machine_realize's own crossbar-realize
 * pass, after this function's every caller has returned). preset names
 * which sgi-xio-baseio preset to build: "io6" for the plain card,
 * "io6g" for the variant whose MediaIO daughtercard fits a second
 * IOC3.
 */
static bool sgi_machine_instantiate_baseio_card(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelIoBoard *record, unsigned int board_index,
    const SGIModelXtalkAttachment *attachment, const char *preset,
    Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_SGI_XIO_BASEIO);
    g_autofree char *name = g_strdup_printf("io-board%u", board_index);
    /*
     * "pci.baseio<N>": two cards would otherwise share a
     * migration-visible PCI bus name.
     */
    g_autofree char *bus_name = g_strdup_printf("pci.baseio%u", board_index);
    g_autofree char *nvram_id = g_strdup_printf("sgi-nvram%u", board_index);
    DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, board_index);
    BlockBackend *nvram_blk = nvram_dinfo ?
        blk_by_legacy_dinfo(nvram_dinfo) : blk_by_name(nvram_id);
    /*
     * The card's BASEIO PROM flash, by name only: -drive if=none,
     * id=sgi-baseio-flash<N>. The pflash units are the node PROMs.
     */
    g_autofree char *flash_id =
        g_strdup_printf("sgi-baseio-flash%u", board_index);
    BlockBackend *flash_blk = blk_by_name(flash_id);
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
     * A switched fabric carries the destination widget in its own packet
     * header, so the Bridge keeps it in bits 51:48 of the address it
     * composes rather than spending address bits on a node number the
     * switch would not read. This Crossbow's own packet ingress does not
     * exist until it realizes, so the link's own proxy stands in;
     * sgi_xtalk_crossbow_link_finish() aliases the real ingress onto it
     * once the Crossbow is built.
     */
    g_autofree char *proxy_name =
        g_strdup_printf("io-board%u-packet-proxy", board_index);
    SGIXtalkCrossbowLink *crossbow_link =
        &s->crossbow_links[s->num_nodes + board_index];
    SGIXioBoardState *board;
    size_t i;

    object_property_add_child(OBJECT(s), name, OBJECT(dev));
    object_unref(OBJECT(dev));

    qdev_prop_set_uint32(dev, "module", record->module_index);
    qdev_prop_set_uint32(dev, "slot", attachment->widget);
    qdev_prop_set_uint64(dev, "onewire-serial", record->serial);
    if (record->has_ioc3_serial) {
        qdev_prop_set_uint64(dev, "ioc3-onewire-serial", record->ioc3_serial);
    }
    qdev_prop_set_string(dev, "pci-bus-name", bus_name);
    qdev_prop_set_string(dev, "preset", preset);
    if (nvram_blk) {
        qdev_prop_set_string(dev, "nvram-drive", blk_name(nvram_blk));
    }
    if (flash_blk) {
        qdev_prop_set_string(dev, "flash-drive", blk_name(flash_blk));
    }
    for (i = 0; i < ARRAY_SIZE(serial_bindings); i++) {
        Chardev *chr = serial_hd(sgi_machine_compute_serial_line(
            t, attachment->node_index, serial_bindings[i].offset));

        if (chr) {
            qdev_prop_set_string(dev, serial_bindings[i].prop, chr->label);
        }
    }
    /*
     * The MediaIO daughtercard's parallel port, by board index the way
     * sgi_machine_instantiate_ip29_io() binds the Origin 200's. The plain
     * card has no parallel port, and QEMU's default "-parallel" backend
     * exists whether or not one was asked for, so only io6g takes it.
     */
    if (g_str_equal(preset, "io6g") && board_index < MAX_PARALLEL_PORTS &&
        parallel_hds[board_index]) {
        qdev_prop_set_string(dev, "parallel-chardev",
                             parallel_hds[board_index]->label);
    }
    object_property_set_link(
        OBJECT(dev), "fabric",
        OBJECT(sgi_xtalk_crossbow_link_prepare(crossbow_link, OBJECT(s),
                                               proxy_name)),
        &error_fatal);
    qdev_prop_set_uint32(dev, "fabric-widget-shift",
                         SGI_BRIDGE_PACKET_ADDRESS_BITS);

    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    board = SGI_XIO_BOARD(dev);
    s->io_boards[board_index] = board;

    if (!sgi_xtalk_crossbow_link_attach_widget(
            crossbow_link, OBJECT(board), s->crossbars[attachment->crossbar_index],
            attachment->widget, errp)) {
        return false;
    }
    board->attached = true;
    return true;
}

/*
 * The IO-8's fixed devices: an sgi-baseio (the "io8" preset) on the
 * Bedrock node's own XBridge head F, reached through
 * sn1_node_xbridge_pci_bus() (sn1-node.c) with this attachment's own
 * widget number -- the same attachment record every other product's
 * direct-attach board reads its widget from (Origin 200's onboard
 * Bridge in sgi_machine_instantiate_ip29_io(); an attachment with
 * crossbar_index -1, so sgi_machine_index_xtalk_attachments() already
 * buckets it as a board-owning entry rather than a Crossbow one). The
 * XBridge always answers both of its heads -- one carries the IO-8, the
 * other stays inert (docs/datasheets/xbridge.md) -- so an io_boards
 * entry with no fitted IOC3 one-wire serial names the empty head, and
 * building nothing for it is correct, not a fault. Resolves every
 * backend id from this record and the command line, the policy
 * sn1_node_build_io8() applied before this step moved it here.
 */
static bool sgi_machine_instantiate_io8(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelIoBoard *record, unsigned int board_index,
    const SGIModelXtalkAttachment *attachment, Error **errp)
{
    SN1NodeState *node = SN1_NODE(s->bedrock_nodes[attachment->node_index]);
    DeviceState *baseio;
    PCIBus *bus;

    if (!record->has_ioc3_serial) {
        return true;
    }

    bus = sn1_node_xbridge_pci_bus(node, attachment->widget);
    baseio = qdev_new(TYPE_SGI_BASEIO);
    object_property_add_child(OBJECT(node), "baseio", OBJECT(baseio));
    qdev_prop_set_string(baseio, "preset", "io8");
    object_property_set_link(OBJECT(baseio), "pci-bus", OBJECT(bus),
                             &error_abort);
    qdev_prop_set_uint64(baseio, "onewire-serial", record->ioc3_serial);
    /*
     * The onboard IOC3's Timekeeper storage: an indexed `-drive if=mtd`
     * first, else a bare `-drive if=none,id=sgi-nvram0` by name -- one
     * Bedrock node ships today, so always index/id zero, unlike the Hub
     * path's per-board index. Passed as the resolved backend's own name,
     * not the backend itself, for the claim-timing reason sgi-baseio's
     * own header comment gives.
     */
    {
        DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, 0);
        BlockBackend *nvram_blk = nvram_dinfo ?
            blk_by_legacy_dinfo(nvram_dinfo) : blk_by_name("sgi-nvram0");

        if (nvram_blk) {
            qdev_prop_set_string(baseio, "nvram-drive", blk_name(nvram_blk));
        }
    }
    /*
     * The onboard IOC3's serial and parallel backends, named by chardev
     * id for the same claim-timing reason. serial_hd(0) is the L1
     * console -- the node's own controller, not this board's -- so the
     * onboard IOC3 takes indices one through four.
     */
    {
        Chardev *chr;

        chr = serial_hd(1);
        if (chr) {
            qdev_prop_set_string(baseio, "chardev-a", chr->label);
        }
        chr = serial_hd(2);
        if (chr) {
            qdev_prop_set_string(baseio, "chardev-b", chr->label);
        }
        chr = serial_hd(3);
        if (chr) {
            qdev_prop_set_string(baseio, "record-a", chr->label);
        }
        chr = serial_hd(4);
        if (chr) {
            qdev_prop_set_string(baseio, "record-b", chr->label);
        }
        if (parallel_hds[0]) {
            qdev_prop_set_string(baseio, "parallel-chardev",
                                 parallel_hds[0]->label);
        }
    }
    return qdev_realize(DEVICE(baseio), NULL, errp);
}

/*
 * Build one io_boards entry, wire it to its own crosstalk position and
 * realize it: IP29's onboard I/O (direct-attach), the Origin 2000
 * BaseIO card (Crossbow-attach) and the Origin 300 IO-8 (a Bedrock
 * node's own XBridge head) are different enough carriers now (sn0.c
 * owns one outright, sn1-node.c exposes the second as a link property,
 * the third is an sgi-xio-board instance this file builds) that preset
 * alone -- not attachment kind -- decides which function runs;
 * tests/unit/test-sn-model.c checks the compiled descriptor names no
 * other preset once, so this dispatch trusts it and only reports the
 * case it does not recognize.
 */
static bool sgi_machine_build_mgras(SGIMachineState *s, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_SGI_MGRAS);

    object_property_add_child(OBJECT(s), "si-viz", OBJECT(dev));
    s->mgras = SGI_MGRAS(dev);
    object_unref(OBJECT(dev));

    memory_region_init(&s->mgras_packet_proxy, OBJECT(s),
                       "si-viz-packet-proxy",
                       SGI_XBOW_WIDGETS * SGI_XBOW_PACKET_SIZE);
    memory_region_set_enabled(&s->mgras_packet_proxy, false);
    if (!object_property_set_link(OBJECT(dev), "fabric",
                                  OBJECT(&s->mgras_packet_proxy), errp)) {
        return false;
    }
    qdev_prop_set_uint32(dev, "fabric-widget-shift",
                        SGI_BRIDGE_PACKET_ADDRESS_BITS);
    if (!sysbus_realize(SYS_BUS_DEVICE(dev), errp)) {
        return false;
    }

    return sgi_xbow_attach_widget(
        s->crossbars[s->mgras_slot->crossbar_index],
        s->mgras_slot->widget,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0), errp);
}

static const SGIModelXtalkSlot *sgi_machine_find_xtalk_slot(
    const SGIModelTopology *t, const char *id)
{
    const SGIModelXtalkSlotList *entry;

    for (entry = t->xtalk_slots; entry; entry = entry->next) {
        if (g_str_equal(entry->value->id, id)) {
            return entry->value;
        }
    }
    return NULL;
}

static bool sgi_machine_instantiate_io_board(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelIoBoard *record, unsigned int board_index,
    const SGIModelXtalkAttachment *attachment, Error **errp)
{
    switch (record->preset) {
    case SGI_MODEL_IO_BOARD_PRESET_IP29_IO:
        return sgi_machine_instantiate_ip29_io(s, t, record, board_index,
                                               attachment, errp);
    case SGI_MODEL_IO_BOARD_PRESET_IO6:
        return sgi_machine_instantiate_baseio_card(
            s, t, record, board_index, attachment,
            s->baseio_preset ? s->baseio_preset : "io6", errp);
    case SGI_MODEL_IO_BOARD_PRESET_IO6G:
        return sgi_machine_instantiate_baseio_card(s, t, record, board_index,
                                                   attachment, "io6g",
                                                   errp);
    case SGI_MODEL_IO_BOARD_PRESET_IO8:
        return sgi_machine_instantiate_io8(s, t, record, board_index,
                                           attachment, errp);
    default:
        error_setg(errp, "topology '%s' io board %u names an unsupported "
                  "preset for this family", t->name, board_index);
        return false;
    }
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
 * the machine returns cannot become an inbound destination on any of
 * these Crossbows (see the crosstalk port experiment's own report for
 * why), though sgi_machine_done() below still gives such a board an
 * outbound path, through the spare widget this function's own caller
 * held back for it.
 */
static bool sgi_machine_realize_crossbars(
    SGIMachineState *s, const SGIModelTopology *t,
    const unsigned int *node_attachment, const unsigned int *board_attachment,
    Error **errp)
{
    g_autofree const SGIModelXtalkAttachment **attachments = NULL;
    size_t crossbar_count = sgi_machine_topology_crossbar_count(t);
    size_t node_count = sgi_machine_topology_node_count(t);
    size_t io_board_count = sgi_machine_topology_io_board_count(t);
    size_t attachment_count, i;

    attachments = sgi_model_xtalk_attachment_array(t->xtalk_attachments,
                                                    &attachment_count);

    for (i = 0; i < crossbar_count; i++) {
        /*
         * Builder invariant: "links-present" and every endpoint the
         * builders above attached both come from the topology's xtalk
         * attachments plus the graphics slot, so nothing a user supplies
         * can make them differ. An endpoint on a port reported empty would
         * carry traffic to a widget firmware is told is not there.
         */
        uint16_t present = object_property_get_uint(
            OBJECT(s->crossbars[i]), "links-present", &error_abort);

        g_assert(!(sgi_xbow_endpoint_widgets(s->crossbars[i]) & ~present));
        if (!sysbus_realize(SYS_BUS_DEVICE(s->crossbars[i]), errp)) {
            return false;
        }
    }
    for (i = 0; i < node_count; i++) {
        const SGIModelXtalkAttachment *attachment =
            attachments[node_attachment[i]];
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
    for (i = 0; i < io_board_count; i++) {
        const SGIModelXtalkAttachment *attachment =
            attachments[board_attachment[i]];
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
    if (s->mgras) {
        SGIXbowState *crossbow =
            s->crossbars[s->mgras_slot->crossbar_index];
        MemoryRegion *packet =
            sgi_xbow_packet_ingress(crossbow, s->mgras_slot->widget);

        memory_region_init_alias(&s->mgras_packet_alias, OBJECT(s),
                                 "si-viz-packet-ingress", packet, 0,
                                 memory_region_size(packet));
        memory_region_add_subregion(&s->mgras_packet_proxy, 0,
                                    &s->mgras_packet_alias);
        memory_region_set_enabled(&s->mgras_packet_proxy, true);
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
    g_autofree const SGIModelModule **modules = NULL;
    g_autofree const SGIModelNode **nodes = NULL;
    size_t module_count, node_count, module_index;

    modules = sgi_model_module_array(t->modules, &module_count);
    nodes = sgi_model_node_array(t->nodes, &node_count);
    for (module_index = 0; module_index < module_count; module_index++) {
        const SGIModelModule *module = modules[module_index];
        g_autofree unsigned int *module_nodes = NULL;
        size_t module_node_count, i;
        SGIHubModuleIdentity identity;

        if (module->controller_index < 0) {
            continue;
        }
        if (!sgi_sn0_module_management_identity_pages(module->serial,
                                                      identity.page)) {
            error_report("module '%s' has an invalid identity serial",
                        module->id);
            exit(EXIT_FAILURE);
        }
        module_nodes = sgi_model_array_uint32(module->nodes,
                                              &module_node_count);
        for (i = 0; i < module_node_count; i++) {
            unsigned int node_index = module_nodes[i];

            sgi_sn0_module_management_identity_rom(
                nodes[node_index]->slot, module->initial_module_number,
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
    g_autofree const SGIModelController **controllers = NULL;
    g_autofree const SGIModelModule **modules = NULL;
    size_t controller_count, module_count, controller_index;

    controllers = sgi_model_controller_array(t->controllers, &controller_count);
    modules = sgi_model_module_array(t->modules, &module_count);
    for (controller_index = 0; controller_index < controller_count;
         controller_index++) {
        const SGIModelController *controller = controllers[controller_index];
        const SGIModelModule *module = NULL;
        g_autofree unsigned int *module_nodes = NULL;
        size_t module_node_count, i;
        DeviceState *dev;
        Chardev *aux;

        for (i = 0; i < module_count; i++) {
            if (modules[i]->controller_index == (int64_t)controller_index) {
                module = modules[i];
                break;
            }
        }
        if (!module) {
            error_setg(errp, "topology '%s' controller %zu names no module",
                      t->name, controller_index);
            return false;
        }
        if (controller->kind != SGI_MODEL_CONTROLLER_KIND_MSC) {
            error_setg(errp, "topology '%s' controller %zu names an unknown "
                      "controller kind", t->name, controller_index);
            return false;
        }
        module_nodes = sgi_model_array_uint32(module->nodes,
                                              &module_node_count);
        if (module_node_count > SGI_O2000_ADAPTER_COUNT) {
            error_setg(errp, "topology '%s' module '%s' has %zu nodes, "
                      "beyond its controller's %u adapters", t->name,
                      module->id, module_node_count,
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
            t, module_nodes[0], SN0_ELSC_CHARDEV));
        if (aux) {
            qdev_prop_set_chr(dev, "chardev", aux);
        }
        qdev_realize_and_unref(dev, NULL, &error_fatal);

        for (i = 0; i < module_node_count; i++) {
            unsigned int node_index = module_nodes[i];
            SGIHubState *hub = &s->nodes[node_index]->hub;
            SysBusDevice *adapter = SYS_BUS_DEVICE(
                sgi_sn0_module_management_get_adapter(
                    SGI_SN0_MODULE_MANAGEMENT(dev), i));

            memory_region_add_subregion_overlap(
                &hub->regs, SN0_HUB_MODULE_ADAPTER_OFFSET,
                sysbus_mmio_get_region(adapter, 0), 1);
        }
    }
    return true;
}

/*
 * Create every Router in descriptor order, matching the topology's own
 * cable list's router index, and set its own chip properties -- not its
 * port cables, which do not exist until the link-wiring loop below
 * creates them, nor its realize, which waits until every port property
 * is set.
 */
static void sgi_machine_build_routers(SGIMachineState *s,
                                      const SGIModelTopology *t)
{
    g_autofree const SGIModelRouter **routers = NULL;
    size_t router_count, i;

    routers = sgi_model_router_array(t->routers, &router_count);
    s->router_count = router_count;
    s->routers = g_new0(SGIRouterState *, router_count);
    for (i = 0; i < router_count; i++) {
        const SGIModelRouter *router = routers[i];
        g_autofree char *name = g_strdup_printf("router%zu", i);
        DeviceState *dev = qdev_new(TYPE_SGI_ROUTER);

        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        qdev_prop_set_uint8(dev, "chip-inputs", router->chip_inputs);
        qdev_prop_set_uint32(dev, "slot", router->slot);
        qdev_prop_set_uint64(dev, "nic-clock-hz",
                            SN0_HUB_CLOCK_HZ);
        qdev_prop_set_uint64(dev, "nic-serial", router->serial);
        s->routers[i] = SGI_ROUTER(dev);
    }
}

/*
 * Point one endpoint's own cable-reference property at cable: a Hub's
 * one "net-cable", or a Router's numbered "portN-cable".
 */
static void sgi_machine_wire_fabric_vertex(SGIMachineState *s,
                                           const SGIModelLinkEnd *end,
                                           SGINetCableState *cable)
{
    if (end->kind == SGI_MODEL_LINK_END_KIND_ROUTER) {
        g_autofree char *prop =
            g_strdup_printf("port%u-cable", end->port);

        object_property_set_link(OBJECT(s->routers[end->index]), prop,
                                 OBJECT(cable), &error_fatal);
    } else {
        object_property_set_link(OBJECT(&s->nodes[end->index]->hub),
                                 "net-cable", OBJECT(cable), &error_fatal);
    }
}

/*
 * Build one sgi-net-cable per descriptor cable record, named and typed
 * from it, and wire both ends' own cable-reference properties. Routers
 * must already exist (sgi_machine_build_routers()); neither a Router nor
 * a Hub may be realized yet.
 */
static void sgi_machine_build_fabric_links(SGIMachineState *s,
                                           const SGIModelTopology *t)
{
    g_autofree const SGIModelCable **cables = NULL;
    size_t cable_count, i;

    cables = sgi_model_cable_array(t->cables, &cable_count);
    for (i = 0; i < cable_count; i++) {
        const SGIModelCable *cable = cables[i];
        const SGIModelLinkEnd *end0 = cable->ends->value;
        const SGIModelLinkEnd *end1 = cable->ends->next->value;
        DeviceState *dev = qdev_new(TYPE_SGI_NET_CABLE);
        SGINetCableState *net_cable = SGI_NET_CABLE(dev);

        object_property_add_child(OBJECT(s), cable->name, OBJECT(dev));
        qdev_prop_set_uint32(dev, "kind", cable->kind);
        sgi_net_cable_set_end(net_cable, 0,
            end0->kind == SGI_MODEL_LINK_END_KIND_ROUTER ?
                OBJECT(s->routers[end0->index]) :
                OBJECT(&s->nodes[end0->index]->hub),
            end0->port);
        sgi_net_cable_set_end(net_cable, 1,
            end1->kind == SGI_MODEL_LINK_END_KIND_ROUTER ?
                OBJECT(s->routers[end1->index]) :
                OBJECT(&s->nodes[end1->index]->hub),
            end1->port);
        sgi_machine_wire_fabric_vertex(s, end0, net_cable);
        sgi_machine_wire_fabric_vertex(s, end1, net_cable);
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
    const SGIModelProcessor *cpu =
        sgi_machine_topology_node0(t)->cpu_slots->value->assembly;
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
    stq_be_p(config + 0x18, SN0_HUB_CLOCK_HZ);
    stq_be_p(config + 0x20, SN0_HUB_RTC_CLOCK_HZ);
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
 * Bedrock's own PROM policy: -bios names the required stock image and a
 * per-node pflash drive (unit node_index) overrides it, the same two
 * inputs the Hub path below resolves, but with no synthesized default --
 * every Bedrock node this family has built still requires a real image,
 * the contract sn1_node_attach_prom's own removed prefix used to enforce
 * on its own. One image, read once, attaches to every node this
 * topology names (today, at most one: a topology with more is refused
 * before sgi_machine_realize ever reaches this function).
 */
static bool sgi_machine_load_bedrock_flash(MachineState *machine,
                                           SGIMachineState *s, Error **errp)
{
    g_autoptr(GError) local_error = NULL;
    g_autofree char *contents = NULL;
    g_autofree char *path = NULL;
    gsize length;
    unsigned int node_index;

    if (!machine->firmware) {
        error_setg(errp,
                   "origin300 requires -bios with a raw stock PROM payload");
        return false;
    }
    path = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
    if (!path) {
        error_setg(errp, "origin300: cannot find PROM image '%s'",
                  machine->firmware);
        return false;
    }
    if (!g_file_get_contents(path, &contents, &length, &local_error)) {
        error_setg(errp, "origin300: cannot read PROM image '%s': %s",
                  machine->firmware, local_error->message);
        return false;
    }

    for (node_index = 0; node_index < s->num_nodes; node_index++) {
        SN1NodeState *node = SN1_NODE(s->bedrock_nodes[node_index]);

        if (!sn1_node_attach_prom(node, node_index, (const uint8_t *)contents,
                                  length, errp)) {
            return false;
        }
    }
    return true;
}

/*
 * -bios fills every node's boot flash unless a pflash drive already backs
 * it (set on the node before it realized); a short -bios image leaves the
 * default template's own bytes behind it, matching a factory-formatted
 * chip rather than a merely erased one. Bedrock's own policy has no
 * synthesized default and lives in sgi_machine_load_bedrock_flash above;
 * this function's own body stays the Hub's.
 */
static bool sgi_machine_load_flash(MachineState *machine,
                                   const SGIModelTopology *t,
                                   SGIMachineState *s, Error **errp)
{
    const SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(machine);
    uint64_t flash_size;

    if (sgi_machine_topology_node0(t)->asic_kind != SGI_MODEL_ASIC_KIND_HUB) {
        return sgi_machine_load_bedrock_flash(machine, s, errp);
    }

    flash_size = sn0_hub_address_map.flash_reset.size;
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
    g_autofree const SGIModelNode **nodes = NULL;
    unsigned int flash_count = 0;
    unsigned int node_count, node_index, board_index;
    unsigned int cpu_index = 0, cpu_base = 0;
    size_t io_board_count = 0, crossbar_count = 0, node_count_check;
    uint64_t ram_offset = 0;
    SGIModelAsicKind kind;

    if (!t) {
        GString *names = g_string_new(NULL);
        const SGIModelTopologyList *entry;

        for (entry = sgi_model_catalogue()->topologies; entry;
             entry = entry->next) {
            if (sgi_machine_topology_matches_product(entry->value,
                                                      smc->product)) {
                if (names->len) {
                    g_string_append(names, ", ");
                }
                g_string_append(names, entry->value->name);
            }
        }
        error_setg(errp, "%s: unknown topology '%s'; known topologies: %s",
                  mc->name, s->topology_name ? s->topology_name : "(none)",
                  names->str);
        g_string_free(names, true);
        return false;
    }
    if (sgi_machine_has_identity_records(s)) {
        g_autofree const SGIModelController **controllers = NULL;
        size_t controller_count;
        bool has_l1 = false;
        size_t i;

        controllers = sgi_model_controller_array(t->controllers,
                                                  &controller_count);
        for (i = 0; i < controller_count; i++) {
            if (controllers[i]->kind == SGI_MODEL_CONTROLLER_KIND_L1) {
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
    /*
     * Every R10000 and R14000 is a single-core socket: -smp N means N
     * sockets of one core each, and which node each socket sits on
     * comes from the topology's accepted population whose total is N,
     * never from sockets= or cores= on the command line (QEMU's own
     * smp_props.prefer_sockets, set below, completes a bare -smp N to
     * exactly this shape already).
     */
    if (machine->smp.cores != 1 || machine->smp.threads != 1) {
        error_setg(errp, "%s: every processor is its own single-core "
                  "socket; pass -smp N alone, not cores= or threads=",
                  mc->name);
        return false;
    }
    /*
     * With one core and one thread per socket, QEMU derives sockets from
     * maxcpus, so a sockets= that disagrees can only come from an explicit
     * count; -smp N,maxcpus=M (M sockets fitted, N running) stays legal.
     */
    if (machine->smp.sockets != machine->smp.max_cpus) {
        error_setg(errp, "%s: -smp N means N sockets; pass -smp %u, not "
                  "sockets=%u", mc->name, machine->smp.max_cpus,
                  machine->smp.sockets);
        return false;
    }

    node_count = sgi_machine_topology_node_count(t);
    requested = g_new0(unsigned int, node_count);
    {
        const SGIModelCpuPopulation *population;
        const SGIModelCpuPopulation *second;
        g_autofree unsigned int *cpus_per_node = NULL;
        size_t population_node_count;

        population = sgi_machine_find_population_by_total(
            t, machine->smp.cpus, &second);
        if (second) {
            error_setg(errp, "%s: topology '%s' accepts %u CPUs as more "
                      "than one population; a future topology= selector "
                      "will name which one", mc->name, t->name,
                      machine->smp.cpus);
            return false;
        }
        if (!population) {
            g_autofree char *totals = sgi_machine_accepted_totals(t);

            error_setg(errp, "%s: topology '%s' accepts %s CPUs, not %u",
                      mc->name, t->name, totals, machine->smp.cpus);
            return false;
        }
        cpus_per_node = sgi_model_array_uint32(population->cpus_per_node,
                                               &population_node_count);
        for (node_index = 0; node_index < node_count &&
             node_index < population_node_count; node_index++) {
            requested[node_index] = cpus_per_node[node_index];
        }
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
    if (sgi_machine_topology_node0(t)->asic_kind == SGI_MODEL_ASIC_KIND_HUB &&
        s->debug_leds) {
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
    clock_set_hz(s->cpu_refclk,
                sgi_machine_topology_node0(t)->cpu_slots->value->assembly->clock_hz);

    kind = sgi_machine_topology_node0(t)->asic_kind;
    nodes = sgi_model_node_array(t->nodes, &node_count_check);
    g_assert(node_count_check == node_count);

    /*
     * One loop creates every node, the ASIC kind selecting the device
     * type from this two-entry table. The two kinds share no fields, so
     * what each configures below still differs; they no longer reach
     * that configuration through two separate paths, one of them
     * reachable only through an early return.
     */
    s->num_nodes = node_count;
    if (kind == SGI_MODEL_ASIC_KIND_HUB) {
        s->nodes = g_new0(SN0NodeState *, node_count);
    } else {
        s->bedrock_nodes = g_new0(DeviceState *, node_count);
    }
    for (node_index = 0; node_index < node_count; node_index++) {
        g_autofree char *name = g_strdup_printf("node%u", node_index);
        Object *node = object_new(kind == SGI_MODEL_ASIC_KIND_HUB ?
                                  TYPE_SGI_SN0_NODE : TYPE_SGI_SN1_NODE);

        object_property_add_child(OBJECT(machine), name, node);
        object_unref(node);
        if (kind == SGI_MODEL_ASIC_KIND_HUB) {
            s->nodes[node_index] = SN0_NODE(node);
        } else {
            s->bedrock_nodes[node_index] = DEVICE(node);
        }
    }

    if (kind != SGI_MODEL_ASIC_KIND_HUB) {
        g_autofree const SGIModelXtalkAttachment **attachments = NULL;
        size_t attachment_count;

        attachments = sgi_model_xtalk_attachment_array(t->xtalk_attachments,
                                                        &attachment_count);
        for (node_index = 0; node_index < node_count; node_index++) {
            SN1NodeState *node = SN1_NODE(s->bedrock_nodes[node_index]);
            g_autofree unsigned int *widgets = NULL;
            size_t widget_count = 0;
            size_t i;

            /*
             * Widget zero is the crossbar's own local view; the rest come
             * one per this node's own xtalk attachment (a board slot,
             * populated or not -- see sn1-node.c's widget-wiring comment
             * for why an empty slot still needs one). Which of those
             * boards is actually populated is the io_boards loop's own
             * concern below, once every node here has realized.
             */
            widgets = g_new0(unsigned int, attachment_count + 1);
            widgets[widget_count++] = 0;
            for (i = 0; i < attachment_count; i++) {
                if (attachments[i]->node_index != node_index) {
                    continue;
                }
                widgets[widget_count++] = attachments[i]->widget;
            }

            qdev_prop_set_uint32(DEVICE(node), "nasid",
                                 sgi_machine_topology_nasid(t, node_index));
            node->model_node = nodes[node_index];
            node->xbridge_widgets = g_steal_pointer(&widgets);
            node->xbridge_widget_count = widget_count;
            object_property_set_link(OBJECT(node), "ram",
                                     OBJECT(machine->ram), &error_abort);
            qdev_prop_set_uint32(DEVICE(node), "nr-cpus",
                                 requested[node_index]);
            qdev_connect_clock_in(DEVICE(node), "cpu-refclk", s->cpu_refclk);
        }

        /*
         * Every node above is a real device now, its own descriptor
         * record and I/O board resolved, and each already sits at its
         * own nasid-strided address-space window (sn1-node.c), so a
         * second node's realize no longer collides with the first's
         * memory regions. What stops here is the cross-node NUMAlink
         * fabric a real multi-chassis Origin 300 pair needs: nothing
         * sources or models link traffic between Bedrock nodes, so a
         * second node would realize with no way to reach the first one
         * or be reached by it. Refuse explicitly rather than let the
         * topology through to a fabric no code drives.
         */
        if (node_count > 1) {
            error_setg(errp, "%s: topology '%s' names %u Bedrock nodes; "
                      "the cross-node NUMAlink fabric between Bedrock "
                      "nodes is not modeled yet", mc->name, t->name,
                      node_count);
            return false;
        }

        for (node_index = 0; node_index < node_count; node_index++) {
            SN1NodeState *node = SN1_NODE(s->bedrock_nodes[node_index]);
            size_t i;

            sysbus_realize(SYS_BUS_DEVICE(node), &error_fatal);
            for (i = 0; i < requested[node_index]; i++) {
                machine->possible_cpus->cpus[cpu_index++].cpu =
                    CPU(node->cpu[i]);
            }
        }
        g_assert(cpu_index == machine->smp.cpus);

        /*
         * Every node is realized now, so a head's PCI bus link property
         * exists. Each io_boards entry names its own attachment directly
         * (record->attachment, an index into t->xtalk_attachments) rather
         * than through node_attachment/board_attachment's claim-by-
         * crossbar resolution below: a Bedrock head is not behind a real
         * Crossbow, so there is no crossbar to claim through, and both
         * of an XBridge's heads carry their own io_boards entry (one
         * populated, one not) rather than one node-owned link plus a
         * card behind it.
         */
        {
            g_autofree const SGIModelIoBoard **io_boards = NULL;
            size_t bedrock_io_board_count;

            io_boards = sgi_model_io_board_array(t->io_boards,
                                                 &bedrock_io_board_count);
            for (board_index = 0; board_index < bedrock_io_board_count;
                 board_index++) {
                const SGIModelIoBoard *record = io_boards[board_index];

                if (record->attachment >= attachment_count) {
                    error_setg(errp, "topology '%s' io board %u names "
                              "attachment %u, past its %zu crosstalk "
                              "attachment(s)", t->name, board_index,
                              record->attachment, attachment_count);
                    return false;
                }
                if (!sgi_machine_instantiate_io_board(
                        s, t, record, board_index,
                        attachments[record->attachment], errp)) {
                    return false;
                }
            }
        }
        if (!sgi_machine_load_flash(machine, t, s, errp)) {
            return false;
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

    io_board_count = sgi_machine_topology_io_board_count(t);
    crossbar_count = sgi_machine_topology_crossbar_count(t);

    /* Indexed by io_boards position, one entry per board, not by node. */
    s->io_boards = g_new0(SGIXioBoardState *, io_board_count);
    s->direct_links = g_new0(SGIXtalkDirectLink, io_board_count);
    s->crossbars = g_new0(SGIXbowState *, crossbar_count);
    /* One crossbow link per node, then one per io_boards position. */
    s->crossbow_links =
        g_new0(SGIXtalkCrossbowLink, node_count + io_board_count);
    /* One spare fabric proxy per crossbars[] entry; see the header comment. */
    s->spare_fabric_links = g_new0(SGIXtalkCrossbowLink, crossbar_count);
    s->spare_fabric_widget = g_new0(unsigned int, crossbar_count);

    node_attachment = g_new0(unsigned int, node_count);
    board_attachment = g_new0(unsigned int, io_board_count);

    sgi_machine_attach_module_identities(s, t);

    s->rt_refclk = clock_new(OBJECT(machine), "rt-refclk");
    clock_set_hz(s->rt_refclk, SN0_HUB_RTC_CLOCK_HZ);

    sgi_machine_build_routers(s, t);
    sgi_machine_build_fabric_links(s, t);
    for (size_t router_index = 0; router_index < s->router_count;
        router_index++) {
        qdev_realize_and_unref(DEVICE(s->routers[router_index]), NULL,
                               &error_fatal);
    }

    /*
     * Every Hub in the machine, in canonical stride order: each Hub's own
     * "stride" property is its index here, and every Hub gets the same
     * array, itself included, so its own claim-alias walk can size and
     * aim one alias per peer.
     */
    g_autofree SGIHubState **net_peers = g_new(SGIHubState *, node_count);
    for (node_index = 0; node_index < node_count; node_index++) {
        net_peers[node_index] = &s->nodes[node_index]->hub;
    }

    for (node_index = 0; node_index < node_count; node_index++) {
        SN0NodeState *node = s->nodes[node_index];
        const SGIModelNode *model_node = nodes[node_index];
        uint64_t ram_size = machine->ram_size / node_count;
        uint16_t bank_mb[SN0_MAX_MEMORY_BANKS];
        unsigned int populated_banks;
        QList *banks_mb;
        unsigned int bank;
        Chardev *aux_chr;

        if (!sn0_model_memory_layout(&sn0_hub_memory, ram_size,
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

        qdev_prop_set_uint32(DEVICE(node),
                             "nasid",
                             sgi_machine_topology_nasid(t, node_index));
        node->flash_drive = flash_drives[node_index];
        node->model_node = model_node;
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
        qdev_prop_set_uint32(DEVICE(&node->hub), "stride", node_index);
        sgi_hub_set_net_peers(&node->hub, net_peers, node_count);

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

    if (sgi_machine_topology_matches_product(t, "origin2000")) {
        s->mgras_slot = sgi_machine_find_xtalk_slot(t, "xio-slot-3");
        if (!s->mgras_slot) {
            error_setg(errp, "topology '%s' has no xio-slot-3", t->name);
            return false;
        }
    }

    /*
     * Before any node realizes: the Hub's own "xtalk-io" property, once
     * set, cannot change after this node's realize, so a crossbow-attached
     * node's own link claims it here, on a proxy of its own -- the
     * Crossbow itself, and its real packet ingress, do not exist yet.
     */
    if (crossbar_count) {
        g_autofree const SGIModelXtalkAttachment **attachments = NULL;
        size_t attachment_count;

        sgi_machine_build_crossbars(s, t);
        attachments = sgi_model_xtalk_attachment_array(t->xtalk_attachments,
                                                        &attachment_count);
        for (node_index = 0; node_index < node_count; node_index++) {
            if (attachments[node_attachment[node_index]]->crossbar_index >= 0) {
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
            if (nodes[node_index]->slave_role) {
                slave = s->nodes[node_index]->hub.elsc;
            } else {
                master = s->nodes[node_index]->hub.elsc;
            }
        }
        g_assert(master);
        g_assert(slave);
        sgi_elsc_share_arbitration(master, slave, 5);
    }

    /*
     * Every Hub and Router now has its port cables wired and is
     * realized; the machine's own cold reset that follows board
     * construction derives every Hub's initial reachability and link
     * state through the ordinary reset-exit path
     * (sgi_hub_reset_exit(), sgi_router_reset_exit()), the same path a
     * guest-triggered reset uses later. No separate build step is
     * needed.
     */

    /*
     * Every node is realized now, so a direct board's claim view exists
     * and a Crossbow-attached node's physical map can be read.
     */
    {
        g_autofree const SGIModelIoBoard **io_boards = NULL;
        g_autofree const SGIModelXtalkAttachment **attachments = NULL;
        size_t io_board_count_check, attachment_count;

        io_boards = sgi_model_io_board_array(t->io_boards, &io_board_count_check);
        g_assert(io_board_count_check == io_board_count);
        attachments = sgi_model_xtalk_attachment_array(t->xtalk_attachments,
                                                        &attachment_count);
        for (board_index = 0; board_index < io_board_count; board_index++) {
            const SGIModelXtalkAttachment *attachment =
                attachments[board_attachment[board_index]];

            if (!sgi_machine_instantiate_io_board(
                    s, t, io_boards[board_index], board_index, attachment,
                    errp)) {
                return false;
            }
        }
        for (node_index = 0; node_index < node_count; node_index++) {
            const SGIModelXtalkAttachment *attachment =
                attachments[node_attachment[node_index]];

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
    }
    if (crossbar_count &&
        sgi_machine_topology_matches_product(t, "origin2000") &&
        !sgi_machine_build_mgras(s, errp)) {
        return false;
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
     * (see docs/plans/sn-machine-model.md, step 1's own report). A
     * `-device` board realized later still reaches its Crossbow, but only
     * outbound, through the spare widget sgi_machine_build_crossbars()
     * held back for it; sgi_machine_done() below finishes that proxy once
     * every Crossbow realized here is real.
     */
    if (!sgi_machine_realize_crossbars(s, t, node_attachment, board_attachment,
                                       errp)) {
        return false;
    }
    return true;
}

/*
 * Finishes every Crossbow's own spare fabric proxy, once every Crossbow
 * this machine builds has realized (sgi_machine_realize_crossbars() above,
 * called from sgi_machine_realize(), always returns well before this
 * notifier can fire: a cold `-device` board realizes only after
 * machine_run_board_init() has returned, and machine-init-done notifiers
 * do not run until after that). Finishing here, rather than at
 * sgi_machine_build_crossbars() time, is exactly why the proxy is a
 * two-phase prepare()/finish() object in the first place: finish() needs
 * the crossbar's own real packet-ingress region, which does not exist
 * until sgi_machine_realize_crossbars() has run.
 *
 * This wires a `-device` board taking the proxy as its own "fabric"
 * property into the Crossbow both ways. finish() above aliases the spare
 * widget's real packet ingress onto the proxy, but a widget
 * sgi_machine_build_crossbars() held out of startup-links-alive answers
 * as not alive, and the Crossbow disables a not-alive port's whole
 * ingress region (hw/misc/sgi-xbow.c, sgi_xbow_sync_routes()) -- so the
 * alias would still resolve empty without the board also claiming it:
 * build_widget() calls sgi_xtalk_crossbow_link_claim_fabric() once the
 * board's own Bridge has built, and fabric_claimed here is what tells
 * this loop to finish the attachment. For a claimed proxy this loop first
 * registers the board's own faces as the spare widget's inbound
 * destination (sgi_xbow_attach_endpoint_live(), which also turns the
 * port from an empty slot into one reporting a widget present) -- the
 * post-realize counterpart of the sgi_xbow_attach_endpoint() every
 * onboard board uses before its Crossbow realizes, which refuses once the
 * Crossbow is realized and every Crossbow this machine builds is by now --
 * then brings
 * the widget's own link alive (sgi_xbow_mark_widget_alive()) so both
 * directions carry. A processor or peer widget now reaches the board's own
 * Bridge through the Crossbow's real routing, and a packet the board's
 * Bridge sends out lands on whichever populated widget it addresses, the
 * same as any onboard board's traffic in either direction. Onboard and
 * topology-built boards still attach and route before the synchronous
 * Crossbow realize; only the spare widget, which no board may exist to
 * claim until after machine init, takes this later path.
 */
static void sgi_machine_done(Notifier *notifier, void *data)
{
    SGIMachineState *s = container_of(notifier, SGIMachineState,
                                      machine_done);
    const SGIModelTopology *t = sgi_machine_topology(MACHINE(s));
    size_t crossbar_count = sgi_machine_topology_crossbar_count(t);
    size_t i;

    for (i = 0; i < crossbar_count; i++) {
        g_autofree char *name = NULL;

        if (!s->spare_fabric_widget[i]) {
            continue;
        }
        name = g_strdup_printf("crossbow%zu-spare-fabric", i);
        sgi_xtalk_crossbow_link_finish(&s->spare_fabric_links[i], OBJECT(s),
                                       name, s->crossbars[i],
                                       s->spare_fabric_widget[i],
                                       &error_fatal);
        if (s->spare_fabric_links[i].fabric_claimed) {
            sgi_xbow_attach_endpoint_live(
                s->crossbars[i], s->spare_fabric_widget[i],
                s->spare_fabric_links[i].claimant_cpu_face,
                s->spare_fabric_links[i].claimant_packet_face, &error_fatal);
            sgi_xbow_mark_widget_alive(s->crossbars[i],
                                       s->spare_fabric_widget[i],
                                       &error_fatal);
        }
    }
}

void sgi_machine_init(MachineState *machine)
{
    SGIMachineState *s = SGI_MACHINE(machine);

    sgi_machine_realize(machine, &error_fatal);
    s->machine_done.notify = sgi_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
}

static void sgi_machine_reset(MachineState *machine, ResetType type)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    const SGIModelTopology *t = sgi_machine_topology(machine);
    uint32_t mode_bits;
    CPUState *cs;
    unsigned int node, slice;

    /*
     * Every Bedrock node resets itself: "start-powered-off" needs its
     * cpu_reset() to run before qemu_devices_reset(), the reverse of
     * the Hub's own order below, so it cannot share this prefix.
     */
    if (t && sgi_machine_topology_node0(t)->asic_kind != SGI_MODEL_ASIC_KIND_HUB) {
        unsigned int i;

        for (i = 0; i < s->num_nodes; i++) {
            sn1_node_full_reset(SN1_NODE(s->bedrock_nodes[i]), type);
        }
        return;
    }

    mode_bits = t ?
        sgi_machine_topology_node0(t)->cpu_slots->value->assembly->mode_bits : 0;
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

/*
 * origin2000 only: which sgi-xio-baseio preset builds the product's one
 * catalogue-declared I/O board. "io6" (the plain card) by default; "io6g"
 * asks for the variant whose second IOC3 carries a keyboard/mouse pair.
 */
static char *sgi_machine_get_baseio_preset(Object *obj, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    return g_strdup(s->baseio_preset ? s->baseio_preset : "io6");
}

static void sgi_machine_set_baseio_preset(Object *obj, const char *value,
                                          Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    g_free(s->baseio_preset);
    s->baseio_preset = g_strdup(value);
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

/*
 * Asks each Hub for its own currently derived reachability, replacing
 * the fabric's central "routes" property: the same string, from each
 * Hub's own tables instead of one shared object's map.
 */
static char *sgi_machine_get_routes(Object *obj, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);
    GString *result = g_string_new(NULL);
    unsigned int from, to;

    for (from = 0; from < s->num_nodes; from++) {
        for (to = 0; to < s->num_nodes; to++) {
            uint32_t number;
            uint64_t vector;
            bool reachable;

            if (from == to) {
                continue;
            }
            number = sgi_hub_programmed_nasid(&s->nodes[to]->hub);
            reachable = sgi_hub_route(&s->nodes[from]->hub, number, &vector);
            g_string_append_printf(result, "%s%u>%u=", result->len ? " " : "",
                                   from, to);
            if (reachable) {
                g_string_append_printf(result, "0x%" PRIx64, vector);
            } else {
                g_string_append_c(result, '-');
            }
        }
    }
    return g_string_free(result, false);
}

static void sgi_machine_instance_init(Object *obj)
{
    MachineState *machine = MACHINE(obj);
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(obj);
    const SGIModelTopology *t = smc->default_topology ?
        sgi_model_find_topology(smc->default_topology) : NULL;

    SGI_MACHINE(obj)->debug_leds = true;
    /*
     * With no -smp at all QEMU leaves smp.sockets at 1 beside the class's
     * default_cpus; the product's default population is that many
     * single-core sockets, which is what -smp N would have said.
     */
    if (t) {
        machine->smp.sockets = sgi_machine_topology_default_cpus(t);
        machine->smp.cores = 1;
        machine->smp.threads = 1;
    }
}

static void sgi_machine_instance_finalize(Object *obj)
{
    SGIMachineState *s = SGI_MACHINE(obj);
    size_t i;

    g_free(s->nodes);
    g_free(s->bedrock_nodes);
    g_free(s->routers);
    g_free(s->crossbars);
    g_free(s->io_boards);
    g_free(s->direct_links);
    g_free(s->crossbow_links);
    g_free(s->spare_fabric_links);
    g_free(s->spare_fabric_widget);
    g_free(s->topology_name);
    g_free(s->baseio_preset);
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
    /*
     * Every processor is a single-core socket, so a bare -smp N (or no
     * -smp at all, completing to mc->default_cpus) should complete to N
     * sockets of one core each, not one socket of N cores.
     */
    mc->smp_props.prefer_sockets = true;

    /*
     * Both classes are TYPE_SYS_BUS_DEVICE (sgi-xio-board's own parent);
     * qdev's -device path refuses any dynamic sysbus device the machine
     * has not named here, on top of each class's own user_creatable.
     * sgi_machine_build_crossbars()/sgi_machine_done() are what a late
     * instance of either one plugs into.
     */
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_SGI_PCI_EXPANSION);
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_SGI_PCI_SHOEHORN);
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_SGI_XIO_BASEIO);

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

    object_class_property_add_str(oc, "routes", sgi_machine_get_routes, NULL);
    object_class_property_set_description(oc, "routes",
        "The route each ordered pair of Hubs takes, as source>destination="
        "vector in the PROM manual's notation, or - when unreachable, "
        "asked of each Hub's own currently derived tables");

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

    object_class_property_add_str(oc, "baseio-preset",
                                  sgi_machine_get_baseio_preset,
                                  sgi_machine_set_baseio_preset);
    object_class_property_set_description(oc, "baseio-preset",
        "Select the BaseIO card variant this product's I/O board builds: "
        "'io6' (the plain card, the default) or 'io6g' (adds the MediaIO "
        "daughtercard's second IOC3 and both chips' keyboard/mouse pair).");
}

/*
 * A bare -M origin2000, with no -smp at all, never runs smp_parse: the
 * generic machine_initfn default of one socket survives unless this
 * preset corrects it before realize's own validation runs. Every other
 * product's default topology is one node, where one socket is already
 * right.
 */

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
        .class_init    = origin2000_class_init,
    },
};

DEFINE_TYPES(sgi_machine_types)
