/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "sn0-origin2000-xtalk.h"

#define ORIGIN2000_XTALK_NODES       4
#define ORIGIN2000_XTALK_MODULES     1
#define ORIGIN2000_XTALK_CROSSBOWS   2
#define ORIGIN2000_XTALK_ATTACHMENTS 4
#define ORIGIN2000_XTALK_IO_BOARDS   1
#define ORIGIN2000_XTALK_PCI_DEVICES 3
#define ORIGIN2000_XTALK_FIRST_WIDGET 8
#define ORIGIN2000_XTALK_LAST_WIDGET 15

static bool sn0_origin2000_xtalk_validate_counts(
    const SN0ModelComposition *model, Error **errp)
{
    if (!model) {
        error_setg(errp, "Origin 2000 composition is missing");
        return false;
    }
    if (model->node_count != ORIGIN2000_XTALK_NODES || !model->nodes) {
        error_setg(errp, "Origin 2000 requires four physical nodes");
        return false;
    }
    if (model->module_count != ORIGIN2000_XTALK_MODULES || !model->modules) {
        error_setg(errp, "Origin 2000 requires one module");
        return false;
    }
    if (model->crossbow_count != ORIGIN2000_XTALK_CROSSBOWS ||
        !model->crossbows) {
        error_setg(errp, "Origin 2000 requires two Crossbow descriptors");
        return false;
    }
    if (model->xtalk_attachment_count != ORIGIN2000_XTALK_ATTACHMENTS ||
        !model->xtalk_attachments) {
        error_setg(errp, "Origin 2000 requires four node attachments");
        return false;
    }
    if (model->io_board_count != ORIGIN2000_XTALK_IO_BOARDS ||
        !model->io_boards) {
        error_setg(errp, "Origin 2000 requires one BaseIO board");
        return false;
    }

    return true;
}

static bool sn0_origin2000_xtalk_validate_modules(
    const SN0ModelComposition *model, Error **errp)
{
    bool member[ORIGIN2000_XTALK_NODES] = { false };

    for (size_t module_index = 0; module_index < model->module_count;
         module_index++) {
        const SN0ModelModule *module = &model->modules[module_index];

        if (!module->id || module->node_count != ORIGIN2000_XTALK_NODES ||
            !module->nodes) {
            error_setg(errp, "Origin 2000 module membership is incomplete");
            return false;
        }
        for (size_t i = 0; i < module->node_count; i++) {
            unsigned int node_index = module->nodes[i];

            if (node_index >= model->node_count) {
                error_setg(errp,
                           "Origin 2000 module references missing node %u",
                           node_index);
                return false;
            }
            if (member[node_index]) {
                error_setg(errp,
                           "Origin 2000 module repeats node %u",
                           node_index);
                return false;
            }
            if (model->nodes[node_index].module_index != module_index) {
                error_setg(errp, "Origin 2000 node %u has inconsistent "
                           "module membership",
                           node_index);
                return false;
            }
            member[node_index] = true;
        }
    }

    for (size_t i = 0; i < model->node_count; i++) {
        if (!model->nodes[i].id || !model->nodes[i].hub ||
            model->nodes[i].hub->kind != SN0_COMPONENT_HUB) {
            error_setg(errp, "Origin 2000 node %zu is incomplete", i);
            return false;
        }
        if (!member[i]) {
            error_setg(errp,
                       "Origin 2000 node %zu is absent from module membership",
                       i);
            return false;
        }
    }

    return true;
}

static bool sn0_origin2000_xtalk_validate_pci(const SN0ModelIoBoard *board,
                                              Error **errp)
{
    bool devfn_seen[256] = { false };
    unsigned int isp_devices = 0;
    unsigned int ioc3_devices = 0;

    if (!board->pci || board->pci_count != ORIGIN2000_XTALK_PCI_DEVICES) {
        error_setg(errp, "Origin 2000 BaseIO requires three PCI functions");
        return false;
    }

    for (size_t i = 0; i < board->pci_count; i++) {
        const SN0ModelComponent *function = &board->pci[i];
        unsigned int devfn;

        if (function->device > 31 || function->function > 7) {
            error_setg(errp,
                       "Origin 2000 PCI devfn %u.%u is out of range",
                       function->device, function->function);
            return false;
        }
        devfn = function->device << 3 | function->function;
        if (devfn_seen[devfn]) {
            error_setg(errp, "Origin 2000 PCI devfn %u.%u is duplicated",
                       function->device, function->function);
            return false;
        }
        devfn_seen[devfn] = true;

        switch (function->kind) {
        case SN0_COMPONENT_ISP1040:
            if (function->function != 0 || function->device > 1) {
                error_setg(errp,
                           "Origin 2000 ISP1040 PCI devfn %u.%u is unsupported",
                           function->device, function->function);
                return false;
            }
            isp_devices |= 1U << function->device;
            break;
        case SN0_COMPONENT_IOC3:
            if (function->device != 2 || function->function != 0) {
                error_setg(errp,
                           "Origin 2000 IOC3 PCI devfn %u.%u is unsupported",
                           function->device, function->function);
                return false;
            }
            if (function->irq_input < 0 || function->irq_input > 7) {
                error_setg(errp,
                           "Origin 2000 IOC3 Bridge IRQ input is invalid");
                return false;
            }
            ioc3_devices++;
            break;
        default:
            error_setg(errp, "Origin 2000 BaseIO PCI function is unsupported");
            return false;
        }
    }

    if (isp_devices != 0x3 || ioc3_devices != 1) {
        error_setg(errp, "Origin 2000 BaseIO PCI population is incomplete");
        return false;
    }

    return true;
}

bool sn0_origin2000_xtalk_validate(const SN0ModelComposition *model,
                                  Error **errp)
{
    bool node_seen[ORIGIN2000_XTALK_NODES] = { false };
    uint16_t links_present[ORIGIN2000_XTALK_CROSSBOWS] = { 0 };
    const SN0ModelIoBoard *board;

    if (!sn0_origin2000_xtalk_validate_counts(model, errp) ||
        !sn0_origin2000_xtalk_validate_modules(model, errp)) {
        return false;
    }

    for (size_t i = 0; i < model->crossbow_count; i++) {
        const SN0ModelCrossbow *crossbow = &model->crossbows[i];

        if (!crossbow->id || crossbow->module_index >= model->module_count) {
            error_setg(errp, "Origin 2000 Crossbow %zu has invalid "
                       "module reference",
                       i);
            return false;
        }
    }

    for (size_t i = 0; i < model->xtalk_attachment_count; i++) {
        const SN0ModelXtalkAttachment *attachment =
            &model->xtalk_attachments[i];
        const SN0ModelCrossbow *crossbow;
        const SN0ModelPhysicalNode *node;
        uint16_t link;

        if (attachment->crossbow_index >= model->crossbow_count) {
            error_setg(errp,
                       "Origin 2000 attachment %zu references missing Crossbow %u",
                       i, attachment->crossbow_index);
            return false;
        }
        if (attachment->node_index >= model->node_count) {
            error_setg(errp,
                       "Origin 2000 attachment %zu references missing node %u",
                       i, attachment->node_index);
            return false;
        }
        if (attachment->widget < ORIGIN2000_XTALK_FIRST_WIDGET ||
            attachment->widget > ORIGIN2000_XTALK_LAST_WIDGET) {
            error_setg(errp, "Origin 2000 attachment %zu has invalid widget %u",
                       i, attachment->widget);
            return false;
        }
        if (node_seen[attachment->node_index]) {
            error_setg(errp, "Origin 2000 node %u has duplicate attachment",
                       attachment->node_index);
            return false;
        }

        crossbow = &model->crossbows[attachment->crossbow_index];
        node = &model->nodes[attachment->node_index];
        if (crossbow->module_index != node->module_index) {
            error_setg(errp, "Origin 2000 attachment %zu crosses "
                       "module boundaries", i);
            return false;
        }

        link = 1U << attachment->widget;
        if (links_present[attachment->crossbow_index] & link) {
            error_setg(errp, "Origin 2000 Crossbow %u has duplicate widget %u",
                       attachment->crossbow_index, attachment->widget);
            return false;
        }
        links_present[attachment->crossbow_index] |= link;
        node_seen[attachment->node_index] = true;
    }

    for (size_t i = 0; i < model->node_count; i++) {
        if (!node_seen[i]) {
            error_setg(errp,
                       "Origin 2000 node %zu has no Crossbow attachment", i);
            return false;
        }
    }

    board = &model->io_boards[0];
    if (!board->id || board->module_index >= model->module_count) {
        error_setg(errp, "Origin 2000 BaseIO has invalid module reference");
        return false;
    }
    if (board->crossbow_index >= model->crossbow_count) {
        error_setg(errp, "Origin 2000 BaseIO references missing Crossbow %u",
                   board->crossbow_index);
        return false;
    }
    if (board->module_index !=
        model->crossbows[board->crossbow_index].module_index) {
        error_setg(errp, "Origin 2000 BaseIO crosses module boundaries");
        return false;
    }
    if (board->widget < ORIGIN2000_XTALK_FIRST_WIDGET ||
        board->widget > ORIGIN2000_XTALK_LAST_WIDGET) {
        error_setg(errp, "Origin 2000 BaseIO has invalid widget %u",
                   board->widget);
        return false;
    }
    if (!board->bridge || board->bridge->kind != SN0_COMPONENT_BASE_IO) {
        error_setg(errp, "Origin 2000 BaseIO Bridge descriptor is missing");
        return false;
    }
    if (board->bridge->widget != board->widget) {
        error_setg(errp, "Origin 2000 BaseIO Bridge widget %u does not "
                   "match board widget %u",
                   board->bridge->widget, board->widget);
        return false;
    }
    if (links_present[board->crossbow_index] & (1U << board->widget)) {
        error_setg(errp, "Origin 2000 Crossbow %u has duplicate widget %u",
                   board->crossbow_index, board->widget);
        return false;
    }
    links_present[board->crossbow_index] |= 1U << board->widget;

    if (!sn0_origin2000_xtalk_validate_pci(board, errp)) {
        return false;
    }

    for (size_t i = 0; i < model->crossbow_count; i++) {
        uint16_t startup = model->crossbows[i].initially_operational_links;

        if (startup & ~links_present[i]) {
            error_setg(errp, "Origin 2000 Crossbow %zu startup mask 0x%x "
                       "includes absent links",
                       i, startup);
            return false;
        }
    }

    return true;
}
