/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Directly cabled Bedrock endpoints, independent of processor population. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/mips/sgi/net-cable.h"
#include "system/address-spaces.h"
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
        !object_dynamic_cast(peer->parent, TYPE_SGI_SN1_FABRIC)) {
        return NULL;
    }
    fc = SGI_SN1_FABRIC_GET_CLASS(peer->parent);
    return fc->endpoint(peer->parent);
}

SN1FabricEndpoint *sn1_fabric_claimant(SN1FabricEndpoint *endpoint,
                                      unsigned int number)
{
    SN1FabricEndpoint *peer = sn1_fabric_peer(endpoint);

    if (number == sn1_fabric_number(endpoint)) {
        return endpoint;
    }
    if (peer && peer->ready && sn1_fabric_number(peer) == number) {
        return peer;
    }
    return NULL;
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
