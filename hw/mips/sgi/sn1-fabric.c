/* SPDX-License-Identifier: BSD-3-Clause */
/* Cabled Bedrock endpoints, independent of processor population. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "hw/mips/sgi/net-cable.h"
#include "hw/misc/sgi-sn1-router.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "qemu/main-loop.h"
#include "exec/cputlb.h"
#include "sn1-node.h"
#include "sn1-fabric.h"

static const uint64_t sn1_node_space_base[SN1_FABRIC_SPACES] = {
    0, UINT64_C(0x10000000000), UINT64_C(0x20000000000),
    UINT64_C(0x30000000000),
};

static MemTxResult sn1_claim_floor_read(void *opaque, hwaddr offset,
                                        uint64_t *value, unsigned size,
                                        MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR, "sn1: no reachable node carries number "
                  "%u (space offset 0x%" HWADDR_PRIx ")\n",
                  (unsigned)(offset >> SN1_FABRIC_ADDRESS_SHIFT), offset);
    *value = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult sn1_claim_floor_write(void *opaque, hwaddr offset,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR, "sn1: write to number %u reaches no "
                  "node (space offset 0x%" HWADDR_PRIx ")\n",
                  (unsigned)(offset >> SN1_FABRIC_ADDRESS_SHIFT), offset);
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps sn1_claim_floor_ops = {
    .read_with_attrs = sn1_claim_floor_read,
    .write_with_attrs = sn1_claim_floor_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};


unsigned int sn1_fabric_number(SN1FabricEndpoint *endpoint)
{
    /* Seven node bits in M mode, the only mode Origin 300 implements. */
    return sgi_bedrock_node_number(endpoint->bedrock) & 0x7f;
}

SN1FabricEndpoint *sn1_fabric_peer(SN1FabricEndpoint *endpoint)
{
    Object *peer;
    unsigned int index;
    SN1FabricClass *fc;

    if (!endpoint->bedrock->net_cable ||
        !sgi_net_cable_neighbor(endpoint->bedrock->net_cable,
                                OBJECT(endpoint->bedrock), 0, &peer, &index) ||
        index != 0 || !object_dynamic_cast(peer, TYPE_SGI_BEDROCK) ||
        !peer->parent ||
        !object_dynamic_cast(peer->parent, TYPE_SGI_SN1_FABRIC)) {
        return NULL;
    }
    fc = SGI_SN1_FABRIC_GET_CLASS(peer->parent);
    SN1FabricEndpoint *target = fc->endpoint(peer->parent);

    return target && target->bedrock == SGI_BEDROCK(peer) ? target : NULL;
}

bool sn1_fabric_routed(SN1FabricEndpoint *endpoint)
{
    Object *peer;
    unsigned index;

    /* Keep transaction dispatch selected even while the cable is down. */
    return endpoint->bedrock->net_cable &&
        sgi_net_cable_other_end(endpoint->bedrock->net_cable,
            OBJECT(endpoint->bedrock), 0, &peer, &index) &&
        object_dynamic_cast(peer, TYPE_SGI_SN1_ROUTER);
}

static SN1FabricEndpoint *sn1_fabric_route(SN1FabricEndpoint *endpoint,
                                          unsigned number)
{
    struct {
        Object *router;
        unsigned ingress;
        uint8_t turn;
    } visited[SGI_NET_PORT_MAX_HOPS];
    Object *peer;
    unsigned ingress;
    uint8_t turn = endpoint->bedrock->ni.regs.local_route[number] & 15;
    unsigned hops = 0;

    if (!sgi_net_cable_neighbor(endpoint->bedrock->net_cable,
            OBJECT(endpoint->bedrock), 0, &peer, &ingress)) {
        return NULL;
    }
    while (object_dynamic_cast(peer, TYPE_SGI_SN1_ROUTER)) {
        unsigned output;
        uint8_t next_turn;

        if (hops == ARRAY_SIZE(visited)) {
            return NULL;
        }
        for (unsigned i = 0; i < hops; i++) {
            if (visited[i].router == peer && visited[i].ingress == ingress &&
                visited[i].turn == turn) {
                return NULL;
            }
        }
        visited[hops].router = peer;
        visited[hops].ingress = ingress;
        visited[hops++].turn = turn;
        if (!sgi_sn1_router_route(SGI_SN1_ROUTER(peer), ingress, turn,
                number, &output, &next_turn) ||
            !sgi_sn1_router_port_neighbor(SGI_SN1_ROUTER(peer), output,
                &peer, &ingress)) {
            return NULL;
        }
        turn = next_turn;
    }
    if (ingress != 0 || !object_dynamic_cast(peer, TYPE_SGI_BEDROCK) ||
        !peer->parent ||
        !object_dynamic_cast(peer->parent, TYPE_SGI_SN1_FABRIC)) {
        return NULL;
    }
    SN1FabricEndpoint *target =
        SGI_SN1_FABRIC_GET_CLASS(peer->parent)->endpoint(peer->parent);

    return target && target->bedrock == SGI_BEDROCK(peer) && target->ready &&
        sgi_bedrock_node_number(target->bedrock) == number ? target : NULL;
}

SN1FabricEndpoint *sn1_fabric_claimant(SN1FabricEndpoint *endpoint,
                                      unsigned int number)
{
    SN1FabricEndpoint *peer = sn1_fabric_peer(endpoint);

    if (sn1_fabric_routed(endpoint)) {
        return sn1_fabric_resolve_named(endpoint, number);
    }
    if (number == sn1_fabric_number(endpoint)) {
        return endpoint;
    }
    if (peer && peer->ready && sn1_fabric_number(peer) == number) {
        return peer;
    }
    return NULL;
}

SN1FabricEndpoint *sn1_fabric_resolve_named(SN1FabricEndpoint *endpoint,
                                           unsigned number)
{
    SN1FabricEndpoint *peer = sn1_fabric_peer(endpoint);
    unsigned own = sn1_fabric_number(endpoint);

    if (number > 0x7f ||
        sgi_bedrock_node_number(endpoint->bedrock) > 0x7f ||
        (peer && sgi_bedrock_node_number(peer->bedrock) > 0x7f)) {
        return NULL;
    }
    if (!endpoint->ready || (peer && peer->ready &&
                            sn1_fabric_number(peer) == own)) {
        return NULL;
    }
    if (own == number) {
        return endpoint;
    }
    if (sn1_fabric_routed(endpoint)) {
        return sn1_fabric_route(endpoint, number);
    }
    return peer && peer->ready && sn1_fabric_number(peer) == number ?
           peer : NULL;
}

MemTxResult sn1_fabric_processor_rw(SN1FabricEndpoint *endpoint,
    unsigned origin, hwaddr address, void *buffer, size_t size, bool write,
    MemTxAttrs attrs)
{
    SN1FabricEndpoint *target;
    unsigned number = (address >> SN1_FABRIC_ADDRESS_SHIFT) & 0x7f;
    hwaddr canonical;

    if (address >= (UINT64_C(1) << 42) ||
        size > SN1_FABRIC_WINDOW_SIZE -
               (address & (SN1_FABRIC_WINDOW_SIZE - 1))) {
        return MEMTX_DECODE_ERROR;
    }
    target = sn1_fabric_resolve_named(endpoint, number);
    if (!target || !target->processor_rw) {
        return MEMTX_DECODE_ERROR;
    }
    canonical = (address & ~(UINT64_C(0x7f) << SN1_FABRIC_ADDRESS_SHIFT)) |
        ((uint64_t)target->placement << SN1_FABRIC_ADDRESS_SHIFT);
    return target->processor_rw(target, origin, canonical, buffer, size,
                                 write, attrs);
}

MemTxResult sn1_fabric_destination_rw(void *opaque, const SGIXioSource *source,
    hwaddr address, void *buffer, size_t size, bool write)
{
    SN1FabricEndpoint *endpoint = opaque;
    SN1FabricEndpoint *target;
    unsigned number;

    if (!source || !source->widget_known || source->widget > 15 ||
        (source->device_known && source->device > 7)) {
        return MEMTX_ACCESS_ERROR;
    }
    if (address >= (UINT64_C(1) << 48) ||
        size > (UINT64_C(1) << 48) - address) {
        return MEMTX_DECODE_ERROR;
    }
    if (address & (UINT64_C(1) << 47)) {
        number = (address >> 39) & 0xff;
    } else {
        if (address >= (UINT64_C(1) << 40)) {
            return MEMTX_DECODE_ERROR;
        }
        number = (address >> SN1_FABRIC_ADDRESS_SHIFT) & 0x7f;
    }
    target = sn1_fabric_resolve_named(endpoint, number);
    if (!target || !target->destination_rw) {
        qemu_log_mask(LOG_GUEST_ERROR,
            "sn1: NI packet at 0x%" HWADDR_PRIx " has no destination for "
            "node %u (source widget %u)\n", address, number, source->widget);
        return MEMTX_DECODE_ERROR;
    }
    if (!sgi_bedrock_available(endpoint->bedrock) ||
        !sgi_bedrock_available(target->bedrock)) {
        return MEMTX_ACCESS_ERROR;
    }
    /* II source_rw has checked both register endpoints. Memory needs it too. */
    for (unsigned device = source->device_known ? source->device : 0;
         device <= (source->device_known ? source->device : 7); device++) {
        SGIBedrockIOState *ii = &target->bedrock->io;

        if (!(ii->inbound_access & (UINT64_C(1) << source->widget)) ||
            (source->widget >= 8 &&
             !(ii->device_mask & (UINT64_C(1) <<
                 ((source->widget - 8) * 8 + device))))) {
            return MEMTX_ACCESS_ERROR;
        }
    }
    MemTxResult result = target->destination_rw(target, source, address,
                                               buffer, size, write);

    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
            "sn1: node %u refused NI packet at 0x%" HWADDR_PRIx
            " from widget %u (transaction result %u)\n",
            number, address, source->widget, result);
    }
    return result;
}

static MemTxResult sn1_processor_read(void *opaque, hwaddr offset,
    uint64_t *value, unsigned size, MemTxAttrs attrs)
{
    SN1FabricProcessorView *view = opaque;
    uint8_t buffer[8] = { 0 };
    MemTxResult result = sn1_fabric_processor_rw(view->endpoint, view->origin,
        sn1_node_space_base[view->space] + offset, buffer, size, false, attrs);

    switch (size) {
    case 1:
        *value = buffer[0];
        break;
    case 2:
        *value = lduw_be_p(buffer);
        break;
    case 4:
        *value = ldl_be_p(buffer);
        break;
    case 8:
        *value = ldq_be_p(buffer);
        break;
    }
    return result;
}

static MemTxResult sn1_processor_write(void *opaque, hwaddr offset,
    uint64_t value, unsigned size, MemTxAttrs attrs)
{
    SN1FabricProcessorView *view = opaque;
    uint8_t buffer[8];

    switch (size) {
    case 1:
        buffer[0] = value;
        break;
    case 2:
        stw_be_p(buffer, value);
        break;
    case 4:
        stl_be_p(buffer, value);
        break;
    case 8:
        stq_be_p(buffer, value);
        break;
    default: return MEMTX_DECODE_ERROR;
    }
    return sn1_fabric_processor_rw(view->endpoint, view->origin,
        sn1_node_space_base[view->space] + offset, buffer, size, true, attrs);
}

static const MemoryRegionOps sn1_processor_ops = {
    .read_with_attrs = sn1_processor_read,
    .write_with_attrs = sn1_processor_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

void sn1_fabric_init_processor_view(SN1FabricProcessorView *view,
    SN1FabricEndpoint *endpoint, unsigned origin, unsigned space,
    MemoryRegion *container)
{
    g_autofree char *name = g_strdup_printf("sn1.node%u.requester%u.space%u",
                                           endpoint->placement, origin, space);

    view->endpoint = endpoint;
    view->origin = origin;
    view->space = space;
    memory_region_init_io(&view->region, endpoint->owner, &sn1_processor_ops,
                          view, name, SN1_FABRIC_SPACE_SIZE);
    memory_region_set_enabled(&view->region, false);
    memory_region_add_subregion_overlap(container, sn1_node_space_base[space],
                                        &view->region, 0);
}

void sn1_fabric_place_view(SN1FabricEndpoint *endpoint,
                          MemoryRegion claim[SN1_FABRIC_SPACES]
                                            [SN1_FABRIC_CLAIMS])
{
    SN1FabricEndpoint *peer = sn1_fabric_peer(endpoint);
    unsigned int own = sn1_fabric_number(endpoint);
    unsigned int other = peer ? sn1_fabric_number(peer) : 0;
    bool peer_reachable = peer && peer->ready && other != own;
    unsigned int space;

    for (space = 0; space < SN1_FABRIC_SPACES; space++) {
        uint64_t base = sn1_node_space_base[space];
        MemoryRegion *mine = &claim[space][0];
        MemoryRegion *theirs = &claim[space][1];

        memory_region_set_address(
            mine, base + ((uint64_t)own << SN1_FABRIC_ADDRESS_SHIFT));
        memory_region_set_enabled(theirs, peer_reachable);
        if (peer_reachable) {
            memory_region_set_alias_offset(
                theirs,
                base + ((uint64_t)peer->placement << SN1_FABRIC_ADDRESS_SHIFT));
            memory_region_set_address(
                theirs, base + ((uint64_t)other << SN1_FABRIC_ADDRESS_SHIFT));
        }
    }
}

void sn1_fabric_update_claims(SN1FabricEndpoint *endpoint)
{
    SN1FabricEndpoint *peer;

    memory_region_transaction_begin();
    endpoint->place_claims(endpoint);
    peer = sn1_fabric_peer(endpoint);
    if (peer && peer->ready) {
        peer->place_claims(peer);
    }
    memory_region_transaction_commit();
}

void sn1_fabric_init_floor(SN1FabricEndpoint *endpoint,
                          MemoryRegion floor[SN1_FABRIC_SPACES])
{
    unsigned int space;

    for (space = 0; space < SN1_FABRIC_SPACES; space++) {
        g_autofree char *name = g_strdup_printf("sn1.node%u.floor%u",
                                                endpoint->placement, space);

        memory_region_init_io(&floor[space], endpoint->owner,
                              &sn1_claim_floor_ops, endpoint, name,
                              SN1_FABRIC_SPACE_SIZE);
    }
}

void sn1_fabric_init_view(SN1FabricEndpoint *endpoint, unsigned int view,
                         MemoryRegion *container,
                         MemoryRegion floor[SN1_FABRIC_SPACES],
                         MemoryRegion floor_alias[SN1_FABRIC_SPACES],
                         MemoryRegion claim[SN1_FABRIC_SPACES]
                                           [SN1_FABRIC_CLAIMS])
{
    MemoryRegion *system = get_system_memory();
    unsigned int space;

    for (space = 0; space < SN1_FABRIC_SPACES; space++) {
        uint64_t base = sn1_node_space_base[space];
        uint64_t placement = base +
            ((uint64_t)endpoint->placement << SN1_FABRIC_ADDRESS_SHIFT);
        g_autofree char *floor_name =
            g_strdup_printf("sn1.node%u.view%u.floor%u",
                            endpoint->placement, view, space);
        unsigned int slot;

        memory_region_init_alias(&floor_alias[space],
                                 endpoint->owner, floor_name,
                                 &floor[space], 0,
                                 SN1_FABRIC_SPACE_SIZE);
        memory_region_add_subregion_overlap(
            container, base, &floor_alias[space], -1);
        for (slot = 0; slot < SN1_FABRIC_CLAIMS; slot++) {
            g_autofree char *name =
                g_strdup_printf("sn1.node%u.view%u.claim%u.%s",
                                endpoint->placement, view, space,
                                slot ? "peer" : "own");

            memory_region_init_alias(&claim[space][slot],
                                     endpoint->owner, name, system,
                                     placement,
                                     SN1_FABRIC_WINDOW_SIZE);
            memory_region_set_enabled(&claim[space][slot],
                                      slot == 0);
            memory_region_add_subregion_overlap(
                container, placement,
                &claim[space][slot], 0);
        }
    }
}

static const TypeInfo sn1_fabric_types[] = {
    {
        .name = TYPE_SGI_SN1_FABRIC,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(SN1FabricClass),
    },
};

DEFINE_TYPES(sn1_fabric_types)

/* CPU mappings are derived from the fixed machine's live fabric state. */
typedef struct SN1MemoryOrigin {
    SN1MemoryMappings *maps;
    SN1NodeState *node;
    Notifier ni;
} SN1MemoryOrigin;

typedef struct SN1MemoryAlias {
    MemoryRegion region;
    SN1NodeState *origin;
    SN1NodeState *target;
    MemoryRegion *parent;
} SN1MemoryAlias;

typedef struct SN1MemoryCable {
    SN1MemoryMappings *maps;
    SGINetCableState *cable;
    Notifier notifier;
} SN1MemoryCable;

struct SN1MemoryMappings {
    SN1MemoryOrigin *origins;
    unsigned num_nodes;
    SGISN1RouterState **routers;
    unsigned num_routers;
    GPtrArray *aliases;
    GPtrArray *cables;
    VMChangeStateEntry *vm_handler;
    bool stopped;
};

void sn1_memory_mappings_refresh(SN1MemoryMappings *maps, SN1NodeState *origin)
{
    if (!maps || maps->stopped) {
        return;
    }
    assert(bql_locked());
    memory_region_transaction_begin();
    for (unsigned i = 0; i < maps->aliases->len; i++) {
        SN1MemoryAlias *alias = g_ptr_array_index(maps->aliases, i);
        unsigned number;
        uint64_t size;
        bool enabled;

        if (origin && alias->origin != origin) {
            continue;
        }
        number = sgi_bedrock_node_number(
            SGI_BEDROCK(alias->target->bedrock));
        size = MIN(memory_region_size(&alias->target->ni_target[0]),
                   SN1_FABRIC_WINDOW_SIZE);
        enabled = size && alias->origin->endpoint.ready &&
            alias->target->endpoint.ready &&
            sn1_fabric_routed(&alias->origin->endpoint) &&
            sn1_fabric_resolve_named(&alias->origin->endpoint, number) ==
                &alias->target->endpoint;
        memory_region_set_size(&alias->region, size);
        memory_region_set_address(&alias->region,
                                  (uint64_t)number << SN1_FABRIC_ADDRESS_SHIFT);
        memory_region_set_enabled(&alias->region, enabled);
    }
    memory_region_transaction_commit();
}

static void sn1_memory_ni_changed(Notifier *notifier, void *data)
{
    SN1MemoryOrigin *origin = container_of(notifier, SN1MemoryOrigin, ni);

    sn1_memory_mappings_refresh(origin->maps, origin->node);
}

static void sn1_memory_router_changed(void *opaque)
{
    sn1_memory_mappings_refresh(opaque, NULL);
}

static void sn1_memory_cable_changed(Notifier *notifier, void *data)
{
    SN1MemoryCable *cable =
        container_of(notifier, SN1MemoryCable, notifier);

    sn1_memory_mappings_refresh(cable->maps, NULL);
}

static void sn1_memory_vm_changed(void *opaque, bool running, RunState state)
{
    if (running) {
        sn1_memory_mappings_refresh(opaque, NULL);
    }
}

SN1MemoryMappings *sn1_memory_mappings_new(DeviceState **nodes,
    unsigned num_nodes, SGISN1RouterState **routers, unsigned num_routers)
{
    SN1MemoryMappings *maps = g_new0(SN1MemoryMappings, 1);

    assert(bql_locked());
    maps->num_nodes = num_nodes;
    maps->origins = g_new0(SN1MemoryOrigin, num_nodes);
    maps->routers = g_memdup2(routers, sizeof(*routers) * num_routers);
    maps->num_routers = num_routers;
    maps->aliases = g_ptr_array_new_with_free_func(g_free);
    maps->cables = g_ptr_array_new_with_free_func(g_free);
    memory_region_transaction_begin();
    for (unsigned i = 0; i < num_nodes; i++) {
        SN1MemoryOrigin *origin = &maps->origins[i];
        SN1NodeState *node = SN1_NODE(nodes[i]);

        origin->maps = maps;
        origin->node = node;
        object_ref(OBJECT(node));
        origin->ni.notify = sn1_memory_ni_changed;
        node->memory_mappings = maps;
        sgi_bedrock_ni_add_change_notifier(&SGI_BEDROCK(node->bedrock)->ni,
                                          &origin->ni);
        for (unsigned j = 0; j < num_nodes; j++) {
            SN1NodeState *target = SN1_NODE(nodes[j]);

            if (node == target) {
                continue;
            }
            for (unsigned cpu = 0; cpu < node->nr_cpus; cpu++) {
                SN1MemoryAlias *alias = g_new0(SN1MemoryAlias, 1);
                g_autofree char *name = g_strdup_printf(
                    "sn1.node%u.cpu%u.to%u", i, cpu, j);

                alias->origin = node;
                alias->target = target;
                alias->parent = &node->ni_processor[cpu][0].region;
                memory_region_init_alias(&alias->region, OBJECT(node), name,
                    &target->ni_target[0], 0,
                    MIN(memory_region_size(&target->ni_target[0]),
                        SN1_FABRIC_WINDOW_SIZE));
                memory_region_set_enabled(&alias->region, false);
                memory_region_add_subregion_overlap(alias->parent, 0,
                                                      &alias->region, 0);
                g_ptr_array_add(maps->aliases, alias);
            }
        }
    }
    memory_region_transaction_commit();
    for (unsigned i = 0; i < num_routers; i++) {
        object_ref(OBJECT(routers[i]));
        sgi_sn1_router_set_changed(routers[i], sn1_memory_router_changed, maps);
    }
    maps->vm_handler = qemu_add_vm_change_state_handler(sn1_memory_vm_changed,
                                                       maps);
    return maps;
}

void sn1_memory_mappings_add_cable(SN1MemoryMappings *maps,
                                  SGINetCableState *cable)
{
    SN1MemoryCable *entry;

    assert(maps && !maps->stopped);
    for (unsigned i = 0; i < maps->cables->len; i++) {
        entry = g_ptr_array_index(maps->cables, i);
        assert(entry->cable != cable);
    }
    entry = g_new0(SN1MemoryCable, 1);
    entry->maps = maps;
    entry->cable = cable;
    object_ref(OBJECT(cable));
    entry->notifier.notify = sn1_memory_cable_changed;
    sgi_net_cable_add_change_notifier(cable, &entry->notifier);
    g_ptr_array_add(maps->cables, entry);
}

static void sn1_memory_pause_cpu(CPUState *cpu, run_on_cpu_data data)
{
    cpu_pause(cpu);
}

static void sn1_memory_flush_cpu(CPUState *cpu, run_on_cpu_data data)
{
    tlb_flush(cpu);
}

void sn1_memory_mappings_stop(SN1MemoryMappings *maps)
{
    if (!maps || maps->stopped) {
        return;
    }
    assert(bql_locked());
    assert(!qemu_in_vcpu_thread());
    maps->stopped = true;
    qemu_del_vm_change_state_handler(maps->vm_handler);
    for (unsigned i = 0; i < maps->num_nodes; i++) {
        SN1NodeState *node = maps->origins[i].node;

        notifier_remove(&maps->origins[i].ni);
        node->memory_mappings = NULL;
        for (unsigned cpu = 0; cpu < node->nr_cpus; cpu++) {
            CPUState *cs = CPU(node->cpu[cpu]);

            /* Failed realization can leave a CPU without an owning thread. */
            if (cs && cs->created && !cs->stopped) {
                run_on_cpu(cs, sn1_memory_pause_cpu, RUN_ON_CPU_NULL);
            }
        }
    }
    for (unsigned i = 0; i < maps->num_routers; i++) {
        sgi_sn1_router_set_changed(maps->routers[i], NULL, NULL);
    }
    for (unsigned i = 0; i < maps->cables->len; i++) {
        SN1MemoryCable *cable = g_ptr_array_index(maps->cables, i);

        notifier_remove(&cable->notifier);
    }
    memory_region_transaction_begin();
    for (unsigned i = 0; i < maps->aliases->len; i++) {
        SN1MemoryAlias *alias = g_ptr_array_index(maps->aliases, i);

        memory_region_del_subregion(alias->parent, &alias->region);
    }
    memory_region_transaction_commit();
    for (unsigned i = 0; i < maps->num_nodes; i++) {
        SN1NodeState *node = maps->origins[i].node;

        for (unsigned cpu = 0; cpu < node->nr_cpus; cpu++) {
            CPUState *cs = CPU(node->cpu[cpu]);

            /* qtest has no TCG TLB; destroyed threads cannot run work. */
            if (cs && tcg_enabled() && cs->tb_jmp_cache && cs->created) {
                run_on_cpu(cs, sn1_memory_flush_cpu, RUN_ON_CPU_NULL);
            }
        }
    }
    for (unsigned i = 0; i < maps->aliases->len; i++) {
        SN1MemoryAlias *alias = g_ptr_array_index(maps->aliases, i);

        object_unparent(OBJECT(&alias->region));
    }
    /* Child properties can disappear before the machine finalizer runs. */
    for (unsigned i = 0; i < maps->cables->len; i++) {
        SN1MemoryCable *cable = g_ptr_array_index(maps->cables, i);

        object_unref(OBJECT(cable->cable));
    }
    for (unsigned i = 0; i < maps->num_routers; i++) {
        object_unref(OBJECT(maps->routers[i]));
    }
    for (unsigned i = 0; i < maps->num_nodes; i++) {
        object_unref(OBJECT(maps->origins[i].node));
    }
    g_ptr_array_free(maps->aliases, true);
    g_ptr_array_free(maps->cables, true);
    g_free(maps->origins);
    g_free(maps->routers);
}
