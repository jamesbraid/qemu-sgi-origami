/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "sn0-origin2000-management.h"

bool sn0_origin2000_management_validate(const SN0ModelComposition *model,
                                       Error **errp)
{
    const SN0ModelModule *module;
    const SN0ModelModuleManagement *management;
    bool members[4] = { false };
    bool adapters[4] = { false };
    bool slots[4] = { false };

    if (!model || model->module_count != 1 || !model->modules ||
        model->node_count != 4 || !model->nodes) {
        error_setg(errp,
                   "Origin 2000 management requires one four-node module");
        return false;
    }
    module = &model->modules[0];
    management = module->management;
    if (!module->id || !module->initial_module_number) {
        error_setg(errp, "Origin 2000 module identity is missing");
        return false;
    }
    if (module->node_count != 4 || !module->nodes || !management ||
        management->adapter_count != 4 || !management->adapter_nodes ||
        !management->adapter_offsets) {
        error_setg(errp, "Origin 2000 management attachment is incomplete");
        return false;
    }
    if (management->msc_address != 0x08 ||
        management->nvram_address != 0x50 ||
        management->nvram_address_count != 8 ||
        management->nvram_bytes != 2048) {
        error_setg(errp,
                   "Origin 2000 management resource shape is unsupported");
        return false;
    }
    for (size_t i = 0; i < module->node_count; i++) {
        unsigned int index = module->nodes[i];
        const SN0ModelPhysicalNode *node;

        if (index >= model->node_count || members[index]) {
            error_setg(errp, "Origin 2000 module node reference is invalid");
            return false;
        }
        members[index] = true;
        node = &model->nodes[index];
        if (node->module_index != 0 || !node->id || !node->hub ||
            node->hub->kind != SN0_COMPONENT_HUB ||
            node->slot < 1 || node->slot > 4 || slots[node->slot - 1]) {
            error_setg(errp, "Origin 2000 management node identity is invalid");
            return false;
        }
        slots[node->slot - 1] = true;
        if (node->hub->i2c_controller || node->hub->system_controller ||
            node->hub->i2c_nvram_pages) {
            error_setg(errp,
                       "Origin 2000 requires shared management resources");
            return false;
        }
    }
    for (size_t i = 0; i < management->adapter_count; i++) {
        unsigned int index = management->adapter_nodes[i];
        uint64_t offset = management->adapter_offsets[i];
        const SN0ModelAddressMap *map;

        if (index >= model->node_count || adapters[index] || !members[index]) {
            error_setg(errp,
                       "Origin 2000 management adapter reference is invalid");
            return false;
        }
        adapters[index] = true;
        map = &model->nodes[index].map;
        if ((offset & 7) || map->hub_registers_local.size < 16 ||
            map->hub_registers_node.size < 16 ||
            offset > map->hub_registers_local.size - 16 ||
            offset > map->hub_registers_node.size - 16) {
            error_setg(errp,
                       "Origin 2000 management adapter window is invalid");
            return false;
        }
    }
    return true;
}
