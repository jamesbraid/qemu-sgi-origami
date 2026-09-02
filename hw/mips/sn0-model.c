/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Generic consumers for data generated from an SN0 product model. */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "sn0-model.h"

const SN0ModelProfile *sn0_model_find_profile(const SN0MachineModel *model,
                                              unsigned int nodes,
                                              const unsigned int *cpus)
{
    size_t profile_index;

    g_assert(model->profiles || model->profile_count == 0);
    for (profile_index = 0; profile_index < model->profile_count;
         profile_index++) {
        const SN0ModelProfile *profile = &model->profiles[profile_index];
        unsigned int node;

        if (profile->nodes != nodes) {
            continue;
        }
        g_assert(profile->cpus_per_node || profile->nodes == 0);
        for (node = 0; node < nodes; node++) {
            if (profile->cpus_per_node[node] != cpus[node]) {
                break;
            }
        }
        if (node == nodes) {
            return profile;
        }
    }
    return NULL;
}

const SN0ModelDevice *sn0_model_find_device(const SN0MachineModel *model,
                                            SN0ModelDeviceKind kind,
                                            unsigned int instance)
{
    size_t device_index;

    for (device_index = 0; device_index < model->device_count; device_index++) {
        const SN0ModelDevice *device = &model->devices[device_index];

        if (device->kind == kind) {
            if (instance == 0) {
                return device;
            }
            instance--;
        }
    }
    return NULL;
}

unsigned int sn0_model_device_count(const SN0MachineModel *model,
                                    SN0ModelDeviceKind kind)
{
    size_t device_index;
    unsigned int count = 0;

    for (device_index = 0; device_index < model->device_count;
        device_index++) {
        if (model->devices[device_index].kind == kind) {
            count++;
        }
    }
    return count;
}

const SN0ModelRegion *sn0_model_find_region(const SN0MachineModel *model,
                                            SN0ModelRegionRole role)
{
    size_t region_index;

    for (region_index = 0; region_index < model->region_count;
         region_index++) {
        const SN0ModelRegion *region = &model->regions[region_index];

        if (region->role == role) {
            return region;
        }
    }
    return NULL;
}

const SN0ModelRegion *sn0_model_region_alias(const SN0MachineModel *model,
                                             const SN0ModelRegion *region)
{
    if (region->alias_of < 0 ||
        (size_t)region->alias_of >= model->region_count) {
        return NULL;
    }
    return &model->regions[region->alias_of];
}

const SN0ModelAddressSpace *sn0_model_find_space(
    const SN0MachineModel *model, SN0ModelAddressSpaceKind kind)
{
    size_t space_index;

    for (space_index = 0; space_index < model->address_space_count;
         space_index++) {
        if (model->address_spaces[space_index].kind == kind) {
            return &model->address_spaces[space_index];
        }
    }
    return NULL;
}

const SN0ModelAddressSpace *sn0_model_find_space_by_selector(
    const SN0MachineModel *model, unsigned int selector)
{
    size_t space_index;

    for (space_index = 0; space_index < model->address_space_count;
         space_index++) {
        if (model->address_spaces[space_index].selector == selector) {
            return &model->address_spaces[space_index];
        }
    }
    return NULL;
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

        g_assert(size_mb <= UINT16_MAX);
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

    g_assert(model->memory.physical_banks <=
             model->memory.architectural_banks);
    g_assert(model->memory.supported_mib &&
             model->memory.supported_size_count > 0);
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
