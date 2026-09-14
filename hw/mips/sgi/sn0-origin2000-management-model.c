/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/1wire/crc.h"
#include "sn0-origin2000-management.h"

static void origin2000_module_identity_page_crc(uint8_t page[32])
{
    uint16_t crc = ~onewire_crc16(0, page, 30);

    page[30] = crc;
    page[31] = crc >> 8;
}

static bool origin2000_build_module_identity_pages(
    const char serial[SN0_MODEL_MODULE_SERIAL_LEN + 1], uint8_t page[2][32])
{
    static const char name[] = "MODULEID";

    if (strlen(serial) != SN0_MODEL_MODULE_SERIAL_LEN) {
        return false;
    }
    for (unsigned int i = 0; i < SN0_MODEL_MODULE_SERIAL_LEN; i++) {
        if (!g_ascii_isprint(serial[i]) || serial[i] == ':' ||
            serial[i] == ';') {
            return false;
        }
    }

    memset(page, 0xff, 2 * 32);
    page[0][0] = 0x01;
    memcpy(&page[0][1], serial, SN0_MODEL_MODULE_SERIAL_LEN);
    memcpy(&page[1][16], name, sizeof(name) - 1);
    origin2000_module_identity_page_crc(page[0]);
    origin2000_module_identity_page_crc(page[1]);
    return true;
}

bool sn0_origin2000_management_validate(const SN0ModelComposition *model,
                                       Error **errp)
{
    const SN0ModelModule *module;
    const SN0ModelModuleManagement *management;
    bool members[4] = { false };
    bool adapters[4] = { false };
    bool slots[4] = { false };
    uint8_t expected_pages[2][32];

    if (!model || model->module_count != 1 || !model->modules ||
        model->node_count != 4 || !model->nodes) {
        error_setg(errp,
                   "Origin 2000 management requires one four-node module");
        return false;
    }
    module = &model->modules[0];
    management = module->management;
    if (!module->id || !module->initial_module_number ||
        !origin2000_build_module_identity_pages(module->serial,
                                                expected_pages)) {
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
            !node->module_identity ||
            node->hub->kind != SN0_COMPONENT_HUB ||
            node->slot < 1 || node->slot > 4 || slots[node->slot - 1]) {
            error_setg(errp, "Origin 2000 management node identity is invalid");
            return false;
        }
        if (node->module_identity->rom[0] != 0x09 ||
            onewire_crc8(0, node->module_identity->rom, 8) ||
            memcmp(node->module_identity->page, expected_pages,
                   sizeof(expected_pages))) {
            error_setg(errp,
                       "Origin 2000 node '%s' has invalid module identity",
                       node->id);
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            const SN0ModelModuleIdentity *other =
                model->nodes[module->nodes[j]].module_identity;

            if (!memcmp(node->module_identity->rom + 1, other->rom + 1, 6)) {
                error_setg(errp,
                           "Origin 2000 repeats module responder identity");
                return false;
            }
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
