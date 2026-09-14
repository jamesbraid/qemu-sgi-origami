/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Generic consumers for data generated from an SN0 product model. */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/units.h"
#include "sn0-model.h"

const SN0ModelProfile *sn0_model_find_profile(const SN0MachineModel *model,
                                              unsigned int nodes,
                                              const unsigned int *cpus)
{
    size_t profile_index;

    for (profile_index = 0; profile_index < model->profile_count;
         profile_index++) {
        const SN0ModelProfile *profile = &model->profiles[profile_index];
        unsigned int node;

        if (profile->node_count != nodes) {
            continue;
        }
        for (node = 0; node < nodes; node++) {
            if (profile->nodes[node].cpu_count != cpus[node]) {
                break;
            }
        }
        if (node == nodes) {
            return profile;
        }
    }
    return NULL;
}

unsigned int sn0_model_max_nodes(const SN0MachineModel *model)
{
    size_t profile_index;
    unsigned int max_nodes = 0;

    for (profile_index = 0; profile_index < model->profile_count;
         profile_index++) {
        const SN0ModelProfile *profile = &model->profiles[profile_index];

        if (profile->node_count > max_nodes) {
            max_nodes = profile->node_count;
        }
    }
    return max_nodes;
}

unsigned int sn0_model_max_cpus(const SN0MachineModel *model)
{
    size_t profile_index;
    unsigned int max_cpus = 0;

    for (profile_index = 0; profile_index < model->profile_count;
         profile_index++) {
        const SN0ModelProfile *profile = &model->profiles[profile_index];
        unsigned int count = 0;
        size_t node_index;

        for (node_index = 0; node_index < profile->node_count; node_index++) {
            count += profile->nodes[node_index].cpu_count;
        }
        if (count > max_cpus) {
            max_cpus = count;
        }
    }
    return max_cpus;
}

static uint64_t sn0_model_node_offset(const SN0ModelAddressMap *map,
                                      unsigned int nasid)
{
    uint64_t offset;

    g_assert(!umul64_overflow(map->node_stride, nasid, &offset));
    return offset;
}

uint64_t sn0_model_node_address(const SN0ModelAddressMap *map,
                                const SN0ModelAddressSpace *space,
                                unsigned int nasid, uint64_t offset)
{
    uint64_t address;

    g_assert(!uadd64_overflow(space->carrier_base,
                             sn0_model_node_offset(map, nasid), &address));
    g_assert(!uadd64_overflow(address, offset, &address));
    return address;
}

uint64_t sn0_model_region_address(const SN0ModelAddressMap *map,
                                  const SN0ModelAddressSpace *space,
                                  unsigned int nasid,
                                  const SN0ModelRegion *region,
                                  uint64_t offset)
{
    uint64_t region_offset;

    g_assert(offset < region->size);
    g_assert(!uadd64_overflow(region->offset, offset, &region_offset));
    return sn0_model_node_address(map, space, nasid, region_offset);
}

static bool sn0_model_layout_from(const SN0ModelMemory *memory,
                                  uint64_t remaining_mb, unsigned int bank,
                                  uint16_t *bank_mb)
{
    unsigned int size_index;

    if (remaining_mb == 0) {
        return bank > 0;
    }
    if (bank == memory->physical_banks) {
        return false;
    }

    for (size_index = 0; size_index < memory->supported_size_count;
         size_index++) {
        uint64_t size_mb = memory->supported_mib[size_index];

        if ((bank == 0 && size_mb > memory->first_bank_max_mib) ||
            size_mb > remaining_mb) {
            continue;
        }
        bank_mb[bank] = size_mb;
        if (sn0_model_layout_from(memory, remaining_mb - size_mb, bank + 1,
                                  bank_mb)) {
            return true;
        }
        bank_mb[bank] = 0;
    }
    return false;
}

bool sn0_model_memory_layout(const SN0MachineModel *model, uint64_t ram_size,
                             uint16_t *bank_mb,
                             unsigned int *populated_banks)
{
    unsigned int bank;

    memset(bank_mb, 0,
           sizeof(*bank_mb) * model->memory.architectural_banks);
    if (ram_size == 0 || ram_size % MiB != 0 ||
        !sn0_model_layout_from(&model->memory, ram_size / MiB, 0, bank_mb)) {
        return false;
    }

    for (bank = 0; bank < model->memory.physical_banks; bank++) {
        if (bank_mb[bank] == 0) {
            break;
        }
    }
    *populated_banks = bank;
    return true;
}
