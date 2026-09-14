/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Validate the physical descriptor independently of device construction. */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "sn0-origin2000-model-consumer.h"
#include "sn0-origin2000-management.h"

static bool origin2000_map_valid(const SN0ModelAddressMap *map)
{
    const SN0ModelAddressSpace *spaces[] = {
        &map->memory, &map->hub_special, &map->xtalk, &map->memory_special,
    };
    const SN0ModelRegion *regions[] = {
        &map->node_memory, &map->hub_registers_local,
        &map->hub_registers_node, &map->hub_local_memory,
        &map->hub_remote_memory, &map->backdoor_ecc,
        &map->backdoor_directory, &map->flash_reset, map->lboot, &map->rboot,
    };
    uint64_t window;
    unsigned int selectors = 0;

    if (!is_power_of_2(map->node_stride) ||
        umul64_overflow(map->node_stride, 256, &window)) {
        return false;
    }
    for (size_t i = 0; i < G_N_ELEMENTS(spaces); i++) {
        const SN0ModelAddressSpace *space = spaces[i];

        if (space->selector >= 4 || (selectors & (1U << space->selector)) ||
            space->carrier_base > UINT64_MAX - window) {
            return false;
        }
        selectors |= 1U << space->selector;
        for (size_t j = 0; j < i; j++) {
            if (space->carrier_base < spaces[j]->carrier_base + window &&
                spaces[j]->carrier_base < space->carrier_base + window) {
                return false;
            }
        }
    }
    for (size_t i = 0; i < G_N_ELEMENTS(regions); i++) {
        const SN0ModelRegion *region = regions[i];

        if (!region || !region->size || region->offset >= map->node_stride ||
            region->size > map->node_stride - region->offset) {
            return false;
        }
    }
    return map->node_memory.offset == 0 &&
           map->node_memory.size == map->node_stride &&
           map->flash_reset.size == MiB;
}

static bool origin2000_memory_valid(const SN0ModelMemory *memory)
{
    uint64_t slots_size;

    if (!memory->physical_banks || memory->architectural_banks > 8 ||
        memory->physical_banks > memory->architectural_banks ||
        !memory->supported_mib || !memory->supported_size_count ||
        !memory->bank_stride_mib || !memory->interleave_slot_stride_mib ||
        !memory->interleave_slots_per_bank ||
        memory->interleave_slots_per_bank > 4 ||
        umul64_overflow(memory->interleave_slot_stride_mib,
                        memory->interleave_slots_per_bank, &slots_size) ||
        slots_size > memory->bank_stride_mib) {
        return false;
    }
    for (unsigned int i = 0; i < memory->supported_size_count; i++) {
        if (!memory->supported_mib[i] ||
            memory->supported_mib[i] > UINT16_MAX ||
            memory->supported_mib[i] > memory->bank_stride_mib) {
            return false;
        }
    }
    return true;
}

static bool origin2000_node_memory_valid(const SN0ModelPhysicalNode *node,
                                          const SN0ModelMemory *memory)
{
    uint64_t total = 0;
    bool empty = false;

    for (unsigned int bank = 0; bank < G_N_ELEMENTS(node->bank_mib); bank++) {
        uint64_t size = node->bank_mib[bank];
        bool supported = false;

        if (!size) {
            empty = true;
            continue;
        }
        if (empty || bank >= memory->physical_banks ||
            (!bank && size > memory->first_bank_max_mib)) {
            return false;
        }
        for (unsigned int i = 0; i < memory->supported_size_count; i++) {
            supported |= size == memory->supported_mib[i];
        }
        if (!supported ||
            (size > memory->interleave_slot_stride_mib &&
             size % memory->interleave_slots_per_bank)) {
            return false;
        }
        total += size;
    }
    return total && total == node->memory_mib;
}

static bool origin2000_fabric_valid(const SN0ModelComposition *c)
{
    bool hubs[4] = { false };
    bool ports[2][7] = { { false } };
    unsigned int attached_hubs[2] = { 0 };
    unsigned int router_links = 0;
    bool slots[2] = { false };

    if (c->router_count != 2 || !c->routers ||
        c->link_count != 5 || !c->links) {
        return false;
    }
    for (size_t i = 0; i < c->router_count; i++) {
        const SN0ModelRouter *router = &c->routers[i];

        if (!router->id || !*router->id || router->module_index != 0 ||
            router->slot < 1 || router->slot > 2 || slots[router->slot - 1] ||
            router->part != 1 || router->revision != 1 ||
            router->chip_inputs > 15 || (router->serial >> 48)) {
            return false;
        }
        slots[router->slot - 1] = true;
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(router->id, c->routers[j].id)) {
                return false;
            }
        }
    }
    for (size_t i = 0; i < c->link_count; i++) {
        const SN0ModelRoutedFabricLink *link = &c->links[i];
        unsigned int hub_count = 0;
        unsigned int router_index = 0;

        if (!link->name || !*link->name ||
            link->kind > SN0_MODEL_LINK_NULL_ROUTER) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(link->name, c->links[j].name)) {
                return false;
            }
        }
        for (unsigned int side = 0; side < 2; side++) {
            const SN0ModelFabricVertex *v = &link->ends[side];

            switch (v->kind) {
            case SN0_MODEL_FABRIC_HUB:
                if (v->index >= 4 || v->port || hubs[v->index]) {
                    return false;
                }
                hubs[v->index] = true;
                hub_count++;
                break;
            case SN0_MODEL_FABRIC_ROUTER:
                if (v->index >= 2 || v->port < 1 || v->port > 6 ||
                    ports[v->index][v->port]) {
                    return false;
                }
                ports[v->index][v->port] = true;
                router_index = v->index;
                break;
            default:
                return false;
            }
        }
        if (hub_count == 1) {
            attached_hubs[router_index]++;
        } else if (hub_count || link->ends[0].index == link->ends[1].index) {
            return false;
        } else {
            router_links++;
        }
    }
    return router_links == 1 && attached_hubs[0] == 2 && attached_hubs[1] == 2;
}

/* Provider-specific Crosstalk behavior is validated by its assembly owner. */
static bool origin2000_io_references_valid(const SN0ModelComposition *c)
{
    bool nodes[4] = { false };
    uint16_t widgets[2] = { 0 };

    if (c->crossbow_count != 2 || !c->crossbows ||
        c->xtalk_attachment_count != 4 || !c->xtalk_attachments ||
        c->io_board_count != 1 || !c->io_boards) {
        return false;
    }
    for (size_t i = 0; i < c->crossbow_count; i++) {
        if (!c->crossbows[i].id || c->crossbows[i].module_index != 0 ||
            c->crossbows[i].widget_revision > 15 ||
            c->crossbows[i].widget_manufacturer > 0x7ff) {
            return false;
        }
    }
    for (size_t i = 0; i < c->xtalk_attachment_count; i++) {
        const SN0ModelXtalkAttachment *a = &c->xtalk_attachments[i];

        if (a->crossbow_index >= c->crossbow_count || a->node_index >= 4 ||
            nodes[a->node_index] || a->widget < 8 || a->widget > 15 ||
            (widgets[a->crossbow_index] & (1U << a->widget))) {
            return false;
        }
        nodes[a->node_index] = true;
        widgets[a->crossbow_index] |= 1U << a->widget;
    }
    for (size_t i = 0; i < c->io_board_count; i++) {
        const SN0ModelIoBoard *board = &c->io_boards[i];

        if (!board->id || board->module_index != 0 || !board->bridge ||
            board->crossbow_index >= c->crossbow_count ||
            board->widget < 8 || board->widget > 15 ||
            (widgets[board->crossbow_index] & (1U << board->widget)) ||
            !board->pci_count || !board->pci) {
            return false;
        }
        widgets[board->crossbow_index] |= 1U << board->widget;
    }
    return true;
}

bool sn0_origin2000_validate(const SN0ModelComposition *c, Error **errp)
{
    unsigned int cpus = 0;

    if (!sn0_origin2000_management_validate(c, errp)) {
        return false;
    }
    if (!c->name || !*c->name || !c->hardware ||
        c->hardware->cpu_kind != SN0_CPU_R10000 ||
        c->hardware->cpu_slots_per_node != 2 ||
        !c->hardware->cpu_clock_hz || !c->hardware->hub_clock_hz ||
        c->hardware->rtc_clock_hz != 1250000 ||
        !origin2000_memory_valid(&c->hardware->memory)) {
        error_setg(errp, "Origin 2000 composition has unsupported hardware");
        return false;
    }
    for (size_t i = 0; i < c->node_count; i++) {
        const SN0ModelPhysicalNode *node = &c->nodes[i];

        if (node->cpu_count != 2 || node->cpu_first_id > 6 ||
            (node->cpu_first_id & 1) ||
            (cpus & (3U << node->cpu_first_id)) ||
            node->flash.kind != SN0_FLASH_AM29F080 ||
            node->hub->ni_revision > 15 ||
            node->hub->widget_revision > 15 ||
            (node->hub->serial >> 48) ||
            node->hub->cpu_irq_bank0 > 5 || node->hub->cpu_irq_bank1 > 5 ||
            node->hub->cpu_irq_rt > 5 ||
            !origin2000_map_valid(&node->map) ||
            !origin2000_node_memory_valid(node, &c->hardware->memory)) {
            error_setg(errp, "Origin 2000 node '%s' has invalid geometry",
                       node->id);
            return false;
        }
        cpus |= 3U << node->cpu_first_id;
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(node->id, c->nodes[j].id)) {
                error_setg(errp, "Origin 2000 repeats node identity '%s'",
                           node->id);
                return false;
            }
        }
    }
    if (!origin2000_fabric_valid(c)) {
        error_setg(errp,
                   "Origin 2000 composition has invalid fabric references");
        return false;
    }
    if (!origin2000_io_references_valid(c)) {
        error_setg(errp, "Origin 2000 composition has invalid I/O references");
        return false;
    }
    return true;
}
