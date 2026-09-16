/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The abstract SGI Scalable Node family machine and its builder over a
 * format-2 SGIModelTopology (sn-model.h). Origin 200 is the first
 * product on this base; see docs/plans/sn-machine-model.md, "QEMU" and
 * "One contract for every product".
 *
 * What is fixed here rather than carried in the descriptor: the Hub's
 * physical address map (node stride, window splits and unit offsets),
 * its memory-bank geometry, its three hardwired CPU interrupt inputs,
 * and the PROM PI-error probe's Bridge offset. Every one of these
 * values is unchanged from the reviewed values Origami's generated
 * descriptor already carried per node before this change (they were
 * byte-for-byte identical across every SN0 node measured, because they
 * are Hub hardware facts, not per-configuration data); see the plan's
 * "Descriptor, format 2" section for why the address architecture moved
 * here instead of staying in the descriptor.
 *
 * Origin 200's onboard I/O is direct-attached (no crossbar): this part
 * keeps that construction inline, the way sn0-model-consumer.c did,
 * rather than moving it onto the sgi-xtalk-board classes -- verifying a
 * board-class re-parent under machine-done deferral is the board-devices
 * step's job, not this one's.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/host-utils.h"
#include "qemu/datadir.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qobject/qlist.h"
#include "qemu/error-report.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-hub.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci/pci.h"
#include "hw/scsi/qlogic-isp.h"
#include "net/net.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/system.h"
#include "trace/control.h"
#include "cpu.h"
#include "hw/mips/sgi/sn-machine.h"
#include "hw/mips/sn0-fabric-routed.h"
#include "sn0.h"

/* The Hub's flash controller offers this view whenever the board wires it. */
static const SN0ModelRegion sgi_sn0_hub_lboot = {
    .offset = 0x10000000ULL,
    .size = 0x10000000ULL,
};

/*
 * The Hub's physical address map: identical on every SN0 node measured
 * (Origin 200 and Origin 2000 alike), so it is one constant here rather
 * than descriptor data repeated per node. See sn-model.h's header
 * comment for the rule this follows.
 */
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
#define SGI_SN0_PROM_PI_PROBE_WINDOW_OFFSET 0x208ULL

struct SGIMachineState {
    MachineState parent_obj;

    char *topology_name;

    unsigned int num_nodes;
    SN0NodeState **nodes;
    /* Per-node adapter structs the shared node builder in sn0.c reads. */
    SN0ModelPhysicalNode *physical_nodes;
    SN0ModelComponent *hub_components;
    SGIBridgeState **io_bridge;
    MemoryRegion *widget0;
    MemoryRegion (*widget_alias)[SN0_XTALK_WIDGETS];
    PCIDevice ***pci_devices;
    MemoryRegion *big_window_route;

    SN0FabricState *fabric;
    Clock *cpu_refclk;
    Clock *rt_refclk;

    bool debug_leds;
    bool junk_uart_present;
    uint8_t elsc_dip_switches;
};

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
 * A name alone is not enough: sgi_model_entries[] holds every product's
 * topologies in one table, and topology= is product-scoped (origin200
 * never builds origin2000's wiring). Require the match sgi_model_
 * find_topology only checks by name to also belong to this machine's
 * own product, the way the "unknown topology" listing below already
 * does.
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

static unsigned int sgi_machine_product_max_cpus(const char *product)
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

static uint64_t sgi_machine_topology_default_ram(const SGIModelTopology *t)
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

static unsigned int sgi_machine_topology_default_cpus(
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
 * Validate that the per-node memory size matches one of the accepted
 * populations. Every node in the topology must accept the same per-node
 * size if an accepted list is present.
 */
static bool sgi_machine_validate_memory_population(
    const SGIModelTopology *t, uint64_t per_node_mib)
{
    unsigned int node_index;

    for (node_index = 0; node_index < t->node_count; node_index++) {
        const SGIModelNode *node = &t->nodes[node_index];
        size_t i;

        if (!node->accepted_memory_mib || !node->accepted_memory_mib_count) {
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
    unsigned int index = 0;
    unsigned int node_index;

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

    for (node_index = 0; node_index < node_count && index < max_cpus;
         node_index++) {
        unsigned int slice;

        for (slice = 0; slice < requested[node_index] && index < max_cpus;
             slice++, index++) {
            CPUArchId *slot = &machine->possible_cpus->cpus[index];

            slot->type = machine->cpu_type;
            slot->vcpus_count = 1;
            slot->arch_id = index;
            slot->props.has_socket_id = true;
            slot->props.socket_id = node_index;
            slot->props.has_core_id = true;
            slot->props.core_id = slice;
            slot->props.has_thread_id = true;
            slot->props.thread_id = 0;
        }
    }
    /*
     * A mismatched -smp/topology= combination is reported clearly once
     * sgi_machine_init runs; give NUMA parsing, which runs first, a
     * complete table to reject from meanwhile.
     */
    for (; index < max_cpus; index++) {
        CPUArchId *slot = &machine->possible_cpus->cpus[index];

        slot->type = machine->cpu_type;
        slot->vcpus_count = 1;
        slot->arch_id = index;
        slot->props.has_socket_id = true;
        slot->props.socket_id = 0;
        slot->props.has_core_id = true;
        slot->props.core_id = index;
        slot->props.has_thread_id = true;
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

/* ---- Descriptor validation ---- */

static bool sgi_machine_cpu_slot_shape(SGIModelAsicKind kind,
                                       unsigned int *interfaces,
                                       unsigned int *per_interface)
{
    switch (kind) {
    case SGI_MODEL_ASIC_HUB:
        *interfaces = 1;
        *per_interface = 2;
        return true;
    case SGI_MODEL_ASIC_BEDROCK:
        *interfaces = 2;
        *per_interface = 2;
        return true;
    default:
        return false;
    }
}

static unsigned int sgi_machine_architectural_banks(SGIModelAsicKind kind)
{
    switch (kind) {
    case SGI_MODEL_ASIC_HUB:
        return sgi_sn0_hub_hardware.memory.architectural_banks;
    default:
        return SGI_MODEL_MAX_MEMORY_BANKS;
    }
}

bool sgi_machine_validate_topology(const SGIModelTopology *t, Error **errp)
{
    g_autofree bool *claimed = NULL;
    size_t i, j;

    if (t->node_count == 0) {
        error_setg(errp, "topology '%s' names no nodes", t->name);
        return false;
    }

    claimed = g_new0(bool, t->node_count);
    for (i = 0; i < t->module_count; i++) {
        const SGIModelModule *module = &t->modules[i];

        for (j = 0; j < module->node_count; j++) {
            unsigned int node_index = module->nodes[j];

            if (node_index >= t->node_count) {
                error_setg(errp, "topology '%s' module '%s' names node %u, "
                          "past its %zu nodes", t->name, module->id,
                          node_index, t->node_count);
                return false;
            }
            if (claimed[node_index]) {
                error_setg(errp, "topology '%s' node %u is in two modules",
                          t->name, node_index);
                return false;
            }
            claimed[node_index] = true;
        }
    }
    for (i = 0; i < t->node_count; i++) {
        if (!claimed[i]) {
            error_setg(errp, "topology '%s' node %zu is in no module",
                      t->name, i);
            return false;
        }
    }

    for (i = 0; i < t->node_count; i++) {
        const SGIModelNode *node = &t->nodes[i];
        unsigned int interfaces, per_interface, banks;

        if (!sgi_machine_cpu_slot_shape(node->asic_kind, &interfaces,
                                        &per_interface)) {
            error_setg(errp, "topology '%s' node %zu names an unknown node "
                      "ASIC kind", t->name, i);
            return false;
        }
        for (j = 0; j < node->cpu_slot_count; j++) {
            const SGIModelCpuSlot *slot = &node->cpu_slots[j];

            if (slot->interface >= interfaces || slot->slot >= per_interface) {
                error_setg(errp, "topology '%s' node %zu names a CPU slot "
                          "beyond its ASIC's %u interface(s) of %u each",
                          t->name, i, interfaces, per_interface);
                return false;
            }
        }
        banks = sgi_machine_architectural_banks(node->asic_kind);
        for (j = 0; j < SGI_MODEL_MAX_MEMORY_BANKS; j++) {
            if (node->bank_mib[j] != 0 && j >= banks) {
                error_setg(errp, "topology '%s' node %zu fills bank %zu, "
                          "beyond its ASIC's %u architectural banks",
                          t->name, i, j, banks);
                return false;
            }
        }
    }

    for (i = 0; i < t->link_count; i++) {
        const SGIModelFabricLink *link = &t->links[i];
        unsigned int side;

        for (side = 0; side < 2; side++) {
            const SGIModelFabricVertex *end = &link->ends[side];
            unsigned int count;

            switch (end->kind) {
            case SGI_MODEL_FABRIC_NODE:
                count = t->node_count;
                break;
            case SGI_MODEL_FABRIC_ROUTER:
                count = t->router_count;
                break;
            default:
                error_setg(errp, "topology '%s' link '%s' names an unknown "
                          "fabric vertex kind", t->name, link->name);
                return false;
            }
            if (end->index >= count) {
                error_setg(errp, "topology '%s' link '%s' end %u names "
                          "vertex %u, past %u", t->name, link->name, side,
                          end->index, count);
                return false;
            }
            if (end->kind == SGI_MODEL_FABRIC_ROUTER &&
                end->port >= t->routers[end->index].port_count) {
                error_setg(errp, "topology '%s' link '%s' end %u names port "
                          "%u, past router %u's %u ports", t->name,
                          link->name, side, end->port, end->index,
                          t->routers[end->index].port_count);
                return false;
            }
        }
        for (j = 0; j < i; j++) {
            const SGIModelFabricLink *other = &t->links[j];
            unsigned int a, b;

            for (a = 0; a < 2; a++) {
                for (b = 0; b < 2; b++) {
                    if (link->ends[a].kind == other->ends[b].kind &&
                        link->ends[a].index == other->ends[b].index &&
                        link->ends[a].port == other->ends[b].port) {
                        error_setg(errp, "topology '%s' links '%s' and '%s' "
                                  "share one fabric end", t->name, link->name,
                                  other->name);
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

/* ---- Origin 200's direct-attached onboard I/O (no crossbar) ---- */

static const SGIModelIoBoard *sgi_machine_node_io_board(
    const SGIModelTopology *t, unsigned int node_index)
{
    unsigned int module_index = t->nodes[node_index].module_index;
    size_t i;

    for (i = 0; i < t->io_board_count; i++) {
        if (t->io_boards[i].module_index == module_index) {
            return &t->io_boards[i];
        }
    }
    return NULL;
}

static const SGIModelXtalkAttachment *sgi_machine_node_xtalk(
    const SGIModelTopology *t, unsigned int node_index)
{
    size_t i;

    for (i = 0; i < t->xtalk_attachment_count; i++) {
        if (t->xtalk_attachments[i].node_index == node_index) {
            return &t->xtalk_attachments[i];
        }
    }
    return NULL;
}

#define SN0_PROM_PI_PROBE_MEMOP(size) (size_memop(size) | MO_BE)

static MemTxResult sgi_machine_prom_pi_probe_read(void *opaque, hwaddr addr,
                                                   uint64_t *data,
                                                   unsigned int size,
                                                   MemTxAttrs attrs)
{
    SN0NodeState *node = opaque;

    if (addr == 0 && size == 8) {
        *data = 0;
        sgi_hub_latch_prom_pi_error(&node->hub);
        return MEMTX_ERROR;
    }
    return memory_region_dispatch_read(
        &node->prom_pi_probe_bridge->
            device_window_mem[node->prom_pi_probe_device_window],
        SGI_SN0_PROM_PI_PROBE_WINDOW_OFFSET + addr, data,
        SN0_PROM_PI_PROBE_MEMOP(size), attrs);
}

static MemTxResult sgi_machine_prom_pi_probe_write(void *opaque, hwaddr addr,
                                                    uint64_t data,
                                                    unsigned int size,
                                                    MemTxAttrs attrs)
{
    SN0NodeState *node = opaque;

    return memory_region_dispatch_write(
        &node->prom_pi_probe_bridge->
            device_window_mem[node->prom_pi_probe_device_window],
        SGI_SN0_PROM_PI_PROBE_WINDOW_OFFSET + addr, data,
        SN0_PROM_PI_PROBE_MEMOP(size), attrs);
}

static const MemoryRegionOps sgi_machine_prom_pi_probe_ops = {
    .read_with_attrs = sgi_machine_prom_pi_probe_read,
    .write_with_attrs = sgi_machine_prom_pi_probe_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/*
 * Compute the serial line index for a node component, respecting the
 * topology's console field.
 *
 * The console field selects which node's IOC3 port A is serial line 0.
 * Serial lines are assigned: console node first (lines 0-5), then other
 * nodes in their original index order (starting from line 6).
 */
static unsigned int sgi_machine_compute_serial_line(
    const SGIModelTopology *t, unsigned int node_index, unsigned int offset)
{
    unsigned int position;

    /*
     * Compute the position of this node in console-first ordering.
     * After reordering, the console node is at position 0, and all
     * other nodes shift accordingly.
     */
    if (node_index == t->console) {
        position = 0;
    } else if (node_index < t->console) {
        position = node_index + 1;
    } else {
        position = node_index;
    }

    return position * SN0_SERIAL_LINES_PER_NODE + offset;
}

static uint32_t sgi_machine_ioc3_population(
    const SGIModelIoc3Population *population)
{
    uint32_t mask = 0;

    mask |= population->ethernet ? SGI_IOC3_POPULATION_ETHERNET : 0;
    mask |= population->serial_a ? SGI_IOC3_POPULATION_SERIAL_A : 0;
    mask |= population->serial_b ? SGI_IOC3_POPULATION_SERIAL_B : 0;
    mask |= population->keyboard_mouse ?
        SGI_IOC3_POPULATION_KEYBOARD_MOUSE : 0;
    mask |= population->parallel ? SGI_IOC3_POPULATION_PARALLEL : 0;
    mask |= population->onewire_identity ?
        SGI_IOC3_POPULATION_ONEWIRE_IDENTITY : 0;
    return mask;
}

static PCIDevice *sgi_machine_create_ioc3(SGIMachineState *s,
                                          const SGIModelTopology *t,
                                          unsigned int node_index,
                                          const SGIModelPciFunction *function)
{
    PCIDevice *ioc3 = pci_new(PCI_DEVFN(function->device, function->function),
                              TYPE_SGI_IOC3);
    Chardev *chr;

    object_property_add_child(OBJECT(s->nodes[node_index]), "ioc3",
                              OBJECT(ioc3));

    chr = serial_hd(sgi_machine_compute_serial_line(t, node_index,
                                                    SN0_IOC3_SERIAL_A_CHARDEV));
    if (chr) {
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-a", chr);
    }
    chr = serial_hd(sgi_machine_compute_serial_line(t, node_index,
                                                    SN0_IOC3_SERIAL_B_CHARDEV));
    if (chr) {
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-b", chr);
    }
    chr = serial_hd(sgi_machine_compute_serial_line(t, node_index,
                                                    SN0_IOC3_RECORD_A_CHARDEV));
    if (chr) {
        qdev_prop_set_chr(DEVICE(ioc3), "record-a", chr);
    }
    chr = serial_hd(sgi_machine_compute_serial_line(t, node_index,
                                                    SN0_IOC3_RECORD_B_CHARDEV));
    if (chr) {
        qdev_prop_set_chr(DEVICE(ioc3), "record-b", chr);
    }
    if (node_index < MAX_PARALLEL_PORTS && parallel_hds[node_index]) {
        qdev_prop_set_chr(DEVICE(ioc3), "parallel-chardev",
                          parallel_hds[node_index]);
    }

    qdev_prop_set_uint16(DEVICE(ioc3), "subsystem-id", function->subsystem_id);
    qdev_prop_set_uint32(DEVICE(ioc3), "population",
                         sgi_machine_ioc3_population(
                             &function->ioc3_population));
    qdev_prop_set_uint8(DEVICE(ioc3), "timekeeper-kind",
                        function->ioc3_population.timekeeper ?
                        SGI_IOC3_TIMEKEEPER_M48T35 : SGI_IOC3_TIMEKEEPER_NONE);
    qdev_prop_set_uint64(DEVICE(ioc3), "onewire-serial", function->serial);

    {
        g_autofree char *nvram_id = g_strdup_printf("sgi-nvram%u", node_index);
        DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, node_index);
        BlockBackend *nvram_blk = nvram_dinfo ?
            blk_by_legacy_dinfo(nvram_dinfo) : blk_by_name(nvram_id);

        if (nvram_blk) {
            qdev_prop_set_drive(DEVICE(ioc3), "nvram-drive", nvram_blk);
        }
    }

    pci_realize_and_unref(ioc3, PCI_HOST_BRIDGE(s->io_bridge[node_index])->bus,
                          &error_fatal);
    pci_set_byte(ioc3->config + PCI_REVISION_ID, function->revision);
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in(DEVICE(s->io_bridge[node_index]),
                                           function->irq_input));
    return ioc3;
}

static PCIDevice *sgi_machine_create_isp(SGIMachineState *s,
                                         unsigned int node_index,
                                         const SGIModelPciFunction *function,
                                         unsigned int instance)
{
    g_autofree char *name = g_strdup_printf("isp%u", instance);
    PCIDevice *isp = pci_new(PCI_DEVFN(function->device, function->function),
                             "qlogic-isp1040");

    object_property_add_child(OBJECT(s->nodes[node_index]), name,
                              OBJECT(isp));
    pci_realize_and_unref(isp, PCI_HOST_BRIDGE(s->io_bridge[node_index])->bus,
                          &error_fatal);
    pci_set_byte(isp->config + PCI_REVISION_ID, function->revision);
    qlogic_isp_handle_legacy_cmdline(DEVICE(isp));
    return isp;
}

static void sgi_machine_create_bridge(SGIMachineState *s,
                                      const SGIModelTopology *t,
                                      unsigned int node_index)
{
    SN0NodeState *node = s->nodes[node_index];
    const SGIModelIoBoard *board = sgi_machine_node_io_board(t, node_index);
    const SGIModelXtalkAttachment *xtalk =
        sgi_machine_node_xtalk(t, node_index);
    const SN0ModelAddressMap *map = &sgi_sn0_hub_address_map;
    const SN0ModelRegion *bridge_region = &map->bridge_registers;
    MemoryRegion *bridge_face;
    SGIBridgeState *bridge;
    g_autofree char *bridge_name = node_index == 0 ?
        g_strdup("bridge") : g_strdup_printf("bridge%u", node_index);
    g_autofree char *bus_name = node_index == 0 ?
        g_strdup("pci") : g_strdup_printf("pci.%u", node_index);
    g_autofree char *widget0_name = node_index == 0 ?
        g_strdup("sn0.xtalk.widget0") :
        g_strdup_printf("sn0.node%u.xtalk.widget0", node_index);

    g_assert(board && xtalk);
    bridge = SGI_BRIDGE(object_new(TYPE_SGI_BRIDGE));
    object_property_add_child(OBJECT(s), bridge_name, OBJECT(bridge));
    object_unref(OBJECT(bridge));
    s->io_bridge[node_index] = bridge;
    qdev_prop_set_uint32(DEVICE(bridge), "revision", board->revision);
    qdev_prop_set_uint64(DEVICE(bridge), "onewire-serial", board->serial);
    qdev_prop_set_string(DEVICE(bridge), "board-part", board->part);
    qdev_prop_set_string(DEVICE(bridge), "pci-bus-name", bus_name);
    /*
     * Interrupt packets travel a processor's claim view, which resolves a
     * node-qualified address by programmed number, not by the board's
     * position -- see sn0_model_create_bridge in the prior consumer for
     * the two-chassis case this matters for. View zero speaks for the
     * node; a node always has a processor.
     */
    object_property_set_link(OBJECT(bridge), "fabric",
                             OBJECT(&node->claim_view[0]), &error_fatal);
    qdev_prop_set_uint64(DEVICE(bridge), "fabric-base",
                         map->xtalk.carrier_base);
    qdev_prop_set_uint32(DEVICE(bridge), "fabric-node-shift",
                         ctz64(map->node_stride));

    sysbus_realize(SYS_BUS_DEVICE(bridge), &error_fatal);
    bridge_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    g_assert(memory_region_size(bridge_face) == bridge_region->size);

    memory_region_init_alias(&s->widget0[node_index], OBJECT(s), widget0_name,
                             bridge_face, 0, bridge_region->size);
    memory_region_add_subregion(&node->xtalk,
                                SN0_XTALK_BASE(xtalk->discovery_widget),
                                &s->widget0[node_index]);
}

static void sgi_machine_populate_node(SGIMachineState *s,
                                      const SGIModelTopology *t,
                                      unsigned int node_index)
{
    SN0NodeState *node = s->nodes[node_index];
    const SGIModelIoBoard *board = sgi_machine_node_io_board(t, node_index);
    const SGIModelXtalkAttachment *xtalk =
        sgi_machine_node_xtalk(t, node_index);
    const SN0ModelRegion *bridge_region =
        &sgi_sn0_hub_address_map.bridge_registers;
    MemoryRegion *bridge_face;
    unsigned int scsi_index = 0;
    unsigned int device_window;
    unsigned int widget;
    size_t i;

    g_assert(board && xtalk);
    sgi_machine_create_bridge(s, t, node_index);
    s->pci_devices[node_index] = g_new0(PCIDevice *, board->pci_count);

    for (i = 0; i < board->pci_count; i++) {
        const SGIModelPciFunction *function = &board->pci[i];

        switch (function->kind) {
        case SGI_MODEL_PCI_ISP1040:
            s->pci_devices[node_index][i] =
                sgi_machine_create_isp(s, node_index, function, scsi_index++);
            break;
        case SGI_MODEL_PCI_IOC3:
            s->pci_devices[node_index][i] =
                sgi_machine_create_ioc3(s, t, node_index, function);
            break;
        default:
            g_assert_not_reached();
        }
    }

    bridge_face = sysbus_mmio_get_region(
        SYS_BUS_DEVICE(s->io_bridge[node_index]), 0);
    memory_region_add_subregion_overlap(
        &node->xtalk, SN0_XTALK_BASE(xtalk->widget), bridge_face, -1);

    for (widget = 0; widget < SN0_XTALK_WIDGETS; widget++) {
        g_autofree char *name = NULL;

        if (!(xtalk->decoded_widget_mask & (1u << widget)) ||
            widget == xtalk->discovery_widget || widget == xtalk->widget) {
            continue;
        }
        name = g_strdup_printf("sn0.node%u.xtalk.widget%u", node_index,
                               widget);
        memory_region_init_alias(&s->widget_alias[node_index][widget],
                                 OBJECT(s), name, bridge_face, 0,
                                 bridge_region->size);
        memory_region_add_subregion(&node->xtalk, SN0_XTALK_BASE(widget),
                                    &s->widget_alias[node_index][widget]);
    }

    memory_region_init_io(&node->prom_pi_probe, OBJECT(node),
                          &sgi_machine_prom_pi_probe_ops, node,
                          "sn0.prom-pi-probe", 8);
    node->prom_pi_probe_bridge = s->io_bridge[node_index];
    for (device_window = 0; device_window < SGI_BRIDGE_DEVICES;
         device_window++) {
        MemoryRegion *window = &node->prom_pi_probe_bridge->
                                   device_window_mem[device_window];

        if (SGI_SN0_PROM_PI_PROBE_OFFSET >= window->addr &&
            SGI_SN0_PROM_PI_PROBE_OFFSET - window->addr <
                memory_region_size(window)) {
            break;
        }
    }
    g_assert(device_window < SGI_BRIDGE_DEVICES);
    node->prom_pi_probe_device_window = device_window;
    memory_region_add_subregion_overlap(
        &node->xtalk,
        SN0_XTALK_BASE(SGI_SN0_PROM_PI_PROBE_WIDGET) +
        SGI_SN0_PROM_PI_PROBE_OFFSET,
        &node->prom_pi_probe, 1);
}

/* ---- The builder ---- */

static bool sgi_machine_show_indicator_codes(Error **errp)
{
    TraceEvent *ev = trace_event_name("sgi_hub_led");

    if (!ev) {
        error_setg(errp, "the sgi_hub_led trace event is missing, so the "
                         "firmware's boot-progress indicator cannot be "
                         "shown; build with it or pass debug-leds=off");
        return false;
    }
    trace_event_set_state_dynamic(ev, true);
    return true;
}

static SN0ModelComponent *sgi_machine_build_hub_component(
    const SGIModelAsicIdentity *identity)
{
    SN0ModelComponent *hub = g_new0(SN0ModelComponent, 1);

    hub->kind = SN0_COMPONENT_HUB;
    hub->ni_revision = identity->ni_revision;
    hub->widget_revision = identity->widget_revision;
    hub->serial = identity->serial;
    hub->i2c_controller = identity->i2c_controller;
    hub->system_controller = identity->system_controller;
    hub->i2c_controller_offset = identity->i2c_controller_offset;
    hub->i2c_elsc_addr = identity->i2c_elsc_addr;
    hub->i2c_nvram_first_addr = identity->i2c_nvram_first_addr;
    hub->i2c_nvram_pages = identity->i2c_nvram_pages;
    hub->cpu_irq_bank0 = SGI_SN0_HUB_CPU_IRQ_BANK0;
    hub->cpu_irq_bank1 = SGI_SN0_HUB_CPU_IRQ_BANK1;
    hub->cpu_irq_rt = SGI_SN0_HUB_CPU_IRQ_RT;
    return hub;
}

static void sgi_machine_load_flash(MachineState *machine, SGIMachineState *s)
{
    uint64_t flash_size = sgi_sn0_hub_address_map.flash_reset.size;
    uint8_t *flash;
    unsigned int node_index;

    flash = g_malloc(flash_size);
    memset(flash, 0xff, flash_size);
    if (machine->firmware) {
        char *path = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
        ssize_t size = path ? load_image_size(path, flash, flash_size) : -1;

        if (size < 0) {
            error_report("%s: cannot read the firmware image '%s'",
                        MACHINE_GET_CLASS(machine)->name, machine->firmware);
            exit(EXIT_FAILURE);
        }
        g_free(path);
    }
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
    g_free(flash);
}

void sgi_machine_init(MachineState *machine)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    const SGIModelTopology *t = sgi_machine_topology(machine);
    Error *err = NULL;
    g_autofree unsigned int *requested = NULL;
    g_autofree BlockBackend **flash_drives = NULL;
    unsigned int flash_count = 0;
    unsigned int node_count, node_index, cpu_index = 0, cpu_base = 0;
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
        error_report("%s: unknown topology '%s'; known topologies: %s",
                    mc->name,
                    s->topology_name ? s->topology_name : "(none)",
                    names->str);
        g_string_free(names, true);
        exit(EXIT_FAILURE);
    }
    if (!sgi_machine_validate_topology(t, &err)) {
        error_report_err(err);
        exit(EXIT_FAILURE);
    }
    if (machine->kernel_filename) {
        error_report("%s: stock-PROM boot only; boot a kernel through the "
                    "PROM's own TFTP netboot or a disk image, not -kernel",
                    mc->name);
        exit(EXIT_FAILURE);
    }
    if (machine->numa_state && machine->numa_state->num_nodes) {
        error_report("%s: -numa is refused; topology= selects the wiring, "
                    "-smp and -m select the population within it", mc->name);
        exit(EXIT_FAILURE);
    }
    if (machine->smp.threads != 1) {
        error_report("%s: one thread per core", mc->name);
        exit(EXIT_FAILURE);
    }

    node_count = t->node_count;
    requested = g_new0(unsigned int, node_count);
    if (node_count == 1) {
        if (machine->smp.sockets != 1) {
            error_report("%s: topology '%s' is one node, one socket",
                        mc->name, t->name);
            exit(EXIT_FAILURE);
        }
        requested[0] = machine->smp.cpus;
    } else {
        if (machine->smp.sockets != node_count ||
            machine->smp.cpus != node_count * machine->smp.cores) {
            error_report("%s: topology '%s' is %u nodes as %u sockets with "
                        "one core count each", mc->name, t->name, node_count,
                        node_count);
            exit(EXIT_FAILURE);
        }
        for (node_index = 0; node_index < node_count; node_index++) {
            requested[node_index] = machine->smp.cores;
        }
    }
    if (!sgi_machine_find_population(t, requested)) {
        error_report("%s: topology '%s' does not accept %u CPUs as %u "
                    "socket(s)", mc->name, t->name, machine->smp.cpus,
                    machine->smp.sockets);
        exit(EXIT_FAILURE);
    }

    if (node_count > 1 && machine->ram_size % ((uint64_t)node_count * MiB)) {
        error_report("%s: topology '%s' splits -m evenly across %u nodes",
                    mc->name, t->name, node_count);
        exit(EXIT_FAILURE);
    }

    if (!sgi_machine_validate_memory_population(t,
                                                machine->ram_size / node_count / MiB)) {
        error_report("%s: topology '%s' does not accept %" PRIu64 " MiB per node",
                    mc->name, t->name, machine->ram_size / node_count / MiB);
        exit(EXIT_FAILURE);
    }

    if (s->debug_leds && !sgi_machine_show_indicator_codes(&err)) {
        error_report_err(err);
        exit(EXIT_FAILURE);
    }

    /*
     * Populate and cache machine->possible_cpus before the node loop
     * below fills in each CPU's pointer.
     */
    sgi_machine_possible_cpu_arch_ids(machine);

    flash_drives = g_new0(BlockBackend *, node_count);
    for (node_index = 0; node_index < node_count; node_index++) {
        DriveInfo *drive = drive_get_by_index(IF_PFLASH, node_index);

        if (drive) {
            flash_drives[node_index] = blk_by_legacy_dinfo(drive);
            flash_count++;
        }
    }
    if (!machine->firmware && !flash_count && !qtest_enabled()) {
        error_report("%s: supports stock-PROM boot only: pass -bios or a "
                    "pflash drive per node", mc->name);
        exit(EXIT_FAILURE);
    }

    s->num_nodes = node_count;
    s->nodes = g_new0(SN0NodeState *, node_count);
    s->physical_nodes = g_new0(SN0ModelPhysicalNode, node_count);
    s->hub_components = g_new0(SN0ModelComponent, node_count);
    s->io_bridge = g_new0(SGIBridgeState *, node_count);
    s->widget0 = g_new0(MemoryRegion, node_count);
    s->widget_alias = g_malloc0(sizeof(*s->widget_alias) * node_count);
    s->pci_devices = g_new0(PCIDevice **, node_count);

    for (node_index = 0; node_index < node_count; node_index++) {
        g_autofree char *name = g_strdup_printf("node%u", node_index);
        Object *node = object_new(TYPE_SGI_SN0_NODE);

        object_property_add_child(OBJECT(machine), name, node);
        object_unref(node);
        s->nodes[node_index] = SN0_NODE(node);
    }

    /* One clock per processor entry; the Hub shares its rate. */
    s->cpu_refclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(s->cpu_refclk, t->nodes[0].cpu_slots[0].assembly->clock_hz);
    s->rt_refclk = clock_new(OBJECT(machine), "rt-refclk");
    clock_set_hz(s->rt_refclk, sgi_sn0_hub_hardware.rtc_clock_hz);

    s->fabric = SN0_FABRIC(qdev_new(TYPE_SN0_FABRIC_ROUTED));
    object_property_add_child(OBJECT(machine), "fabric", OBJECT(s->fabric));
    for (node_index = 0; node_index < node_count; node_index++) {
        unsigned int endpoint = sn0_fabric_add_endpoint(
            s->fabric, &s->nodes[node_index]->hub, node_index);

        g_assert(endpoint == node_index);
    }
    for (link_index = 0; link_index < t->link_count; link_index++) {
        const SGIModelFabricLink *link = &t->links[link_index];
        SN0FabricVertex a = {
            .kind = SN0_FABRIC_VERTEX_HUB,
            .index = link->ends[0].index,
            .port = link->ends[0].port,
        };
        SN0FabricVertex b = {
            .kind = SN0_FABRIC_VERTEX_HUB,
            .index = link->ends[1].index,
            .port = link->ends[1].port,
        };

        sn0_fabric_routed_add_link(s->fabric, link->name, a, b);
    }
    qdev_realize_and_unref(DEVICE(s->fabric), NULL, &error_fatal);

    for (node_index = 0; node_index < node_count; node_index++) {
        SN0NodeState *node = s->nodes[node_index];
        const SGIModelNode *model_node = &t->nodes[node_index];
        uint64_t ram_size = machine->ram_size / node_count;
        uint16_t bank_mb[SN0_MAX_MEMORY_BANKS];
        unsigned int populated_banks;
        QList *banks_mb;
        unsigned int bank;
        Chardev *aux_chr;

        if (!sn0_model_memory_layout(&sgi_sn0_hub_hardware.memory, ram_size,
                                     bank_mb, &populated_banks)) {
            error_report("%s: node %u's %" PRIu64 " MiB cannot be populated "
                        "from IP29 memory banks", mc->name, node_index,
                        ram_size / MiB);
            exit(EXIT_FAILURE);
        }
        banks_mb = qlist_new();
        for (bank = 0; bank < populated_banks; bank++) {
            qlist_append_int(banks_mb, bank_mb[bank]);
        }

        s->hub_components[node_index] =
            *sgi_machine_build_hub_component(&model_node->identity);
        s->hub_components[node_index].i2c_controller_offset =
            model_node->management_adapter_offset;
        s->physical_nodes[node_index] = (SN0ModelPhysicalNode) {
            .id = model_node->id,
            .slot = model_node->slot,
            .hub = &s->hub_components[node_index],
            .map = sgi_sn0_hub_address_map,
            .flash = { .kind = (SN0ModelFlashKind)model_node->flash.kind },
        };

        qdev_prop_set_uint32(DEVICE(node), "nasid", node_index);
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

        sysbus_realize(SYS_BUS_DEVICE(node), &error_fatal);

        {
            unsigned int slice;

            for (slice = 0; slice < requested[node_index]; slice++) {
                machine->possible_cpus->cpus[cpu_index++].cpu =
                    CPU(node->cpu[slice]);
            }
        }
        cpu_base += requested[node_index];
        ram_offset += ram_size;
    }
    g_assert(cpu_index == machine->smp.cpus);
    g_assert(ram_offset == machine->ram_size);

    sn0_map_cpu_big_windows(OBJECT(s), s->nodes, s->num_nodes,
                            &s->big_window_route);

    /* The two-chassis Origin 200 has four CPU turns and one slave turn. */
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

    for (node_index = 0; node_index < node_count; node_index++) {
        sgi_machine_populate_node(s, t, node_index);
    }

    sgi_machine_load_flash(machine, s);
}

static void sgi_machine_reset(MachineState *machine, ResetType type)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    const SGIModelTopology *t = sgi_machine_topology(machine);
    uint32_t mode_bits = t ? t->nodes[0].cpu_slots[0].assembly->mode_bits : 0;
    CPUState *cs;
    unsigned int node, slice;

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
    unsigned int node_index;

    for (node_index = 0; node_index < s->num_nodes; node_index++) {
        if (s->pci_devices) {
            g_free(s->pci_devices[node_index]);
        }
    }
    g_free(s->nodes);
    g_free(s->physical_nodes);
    g_free(s->hub_components);
    g_free(s->io_bridge);
    g_free(s->pci_devices);
    g_free(s->topology_name);
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
    mc->desc = t->description;
    mc->valid_cpu_types = sgi_sn0_r10000_cpu_types;
    mc->default_cpus = sgi_machine_topology_default_cpus(t);
    mc->max_cpus = sgi_machine_product_max_cpus("origin200");
    mc->default_ram_size = sgi_machine_topology_default_ram(t);
    mc->default_ram_id = "sn0.node0.ram";
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
};

DEFINE_TYPES(sgi_machine_types)
