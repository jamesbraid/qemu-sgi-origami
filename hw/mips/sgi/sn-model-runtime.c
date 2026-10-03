/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Validate machine endpoints, resolve a machine selection to the catalogue
 * topology it builds, and project structural population views.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/util.h"
#include "qemu/cutils.h"
#include "qemu/keyval.h"
#include "qobject/qdict.h"
#include "hw/mips/sgi/sgi-machines.h"
#include "hw/nvram/sgi-elsc-nvram.h"

/* The @index'th element of a QAPI list, which the caller has range-checked. */
#define SGI_MODEL_NTH(list, index) ({                                  \
    typeof_strip_qual(list) nth_ = (list);                              \
    for (size_t i_ = 0; i_ < (index); i_++) {                           \
        nth_ = nth_->next;                                              \
    }                                                                   \
    nth_->value;                                                        \
})

bool sgi_model_check_runtime_endpoints(const SGIModelTopology *t, Error **errp)
{
    const SGIModelIoAssemblyList *entry;
    const SGIModelCableList *link;
    size_t peers = QAPI_LIST_LENGTH(t->io_peers);
    size_t nodes = QAPI_LIST_LENGTH(t->nodes);
    size_t modules = QAPI_LIST_LENGTH(t->modules);
    size_t controllers = QAPI_LIST_LENGTH(t->controllers);
    size_t routers = QAPI_LIST_LENGTH(t->routers);
    const SGIModelRouterList *router;
    g_autofree bool *router_ports = g_new0(bool, MAX(routers * 8, 1));
    g_autofree bool *assigned = g_new0(bool, MAX(peers, 1));
    g_autofree bool *connected = g_new0(bool, MAX(peers, 1));

    if (!peers && !t->io_assemblies && !t->l1_management &&
        !(t->generation == SGI_MODEL_GENERATION_SN1 && routers)) {
        const SGIModelModuleList *module;

        for (module = t->modules; module; module = module->next) {
            if (module->value->io_peers) {
                goto unsupported;
            }
        }
        for (link = t->cables; link; link = link->next) {
            const SGIModelLinkEndList *end;

            for (end = link->value->ends; end; end = end->next) {
                if (end->value->kind == SGI_MODEL_LINK_END_KIND_IO_PEER) {
                    goto unsupported;
                }
            }
        }
        return true;
    }
    if (!peers || t->generation != SGI_MODEL_GENERATION_SN1 ||
        (routers && nodes < 2) ||
        QAPI_LIST_LENGTH(t->io_assemblies) != peers || !t->l1_management ||
        QAPI_LIST_LENGTH(t->l1_management->owners) != controllers) {
        goto unsupported;
    }
    for (router = t->routers; router; router = router->next) {
        const SGIModelRouter *r = router->value;
        const SGIModelModule *module;
        const SGIModelController *controller;

        if (r->part != 0x3017 || r->revision != 1 || r->chip_inputs ||
            r->module_index >= modules) {
            goto unsupported;
        }
        module = SGI_MODEL_NTH(t->modules, r->module_index);
        if (module->nodes || module->io_peers ||
            module->controller_index < 0 ||
            module->controller_index >= controllers) {
            goto unsupported;
        }
        controller = SGI_MODEL_NTH(t->controllers, module->controller_index);
        if (controller->kind != SGI_MODEL_CONTROLLER_KIND_L1 ||
            controller->module_index != r->module_index || controller->l1) {
            goto unsupported;
        }
        for (const SGIModelRouterList *other = t->routers; other != router;
             other = other->next) {
            if (other->value->module_index == r->module_index) {
                goto unsupported;
            }
        }
    }
    for (entry = t->io_assemblies; entry; entry = entry->next) {
        const SGIModelIoAssembly *a = entry->value;
        const SGIModelIoPeer *peer;
        const SGIModelModule *module;
        const SGIModelController *controller;
        const uint8List *port, *other_port;
        const SGIModelIoAssemblyList *other;
        const uint8List *pin;

        if (a->version != 1 ||
            a->kind != SGI_MODEL_IO_ASSEMBLY_KIND_ORIGIN300_PCI_EXPANSION ||
            a->peer_index >= peers || assigned[a->peer_index] ||
            a->module_index >= modules || a->controller_index >= controllers ||
            a->placement < nodes || a->placement >= 128 ||
            a->initial_node_number >= 128 || a->bedrock_revision != 2 ||
            a->xbridge_revision != 2 || !a->controller_profile ||
            strcmp(a->controller_profile, "pci-expansion") ||
            a->has_main_power != a->has_initial_main_power ||
            (a->has_main_power &&
             a->main_power != SGI_MODEL_MAIN_POWER_FUNCTIONAL_EXPERIMENT) ||
            (a->has_configuration_storage &&
             (!a->has_main_power ||
              a->configuration_storage !=
                  SGI_MODEL_CONFIGURATION_STORAGE_BEDROCK_FLASH_EXPERIMENT)) ||
            (a->bic_identity & 0xf000) != 0xc000 ||
            QAPI_LIST_LENGTH(a->head_ports) != 4 ||
            (a->has_intx_map && QAPI_LIST_LENGTH(a->intx_map) != 48)) {
            goto unsupported;
        }
        peer = SGI_MODEL_NTH(t->io_peers, a->peer_index);
        module = SGI_MODEL_NTH(t->modules, a->module_index);
        controller = SGI_MODEL_NTH(t->controllers, a->controller_index);
        if (peer->asic_kind != SGI_MODEL_ASIC_KIND_BEDROCK ||
            peer->module_index != a->module_index || module->nodes ||
            QAPI_LIST_LENGTH(module->io_peers) != 1 ||
            module->io_peers->value != a->peer_index ||
            module->controller_index != a->controller_index ||
            controller->kind != SGI_MODEL_CONTROLLER_KIND_L1 ||
            controller->module_index != a->module_index) {
            goto unsupported;
        }
        if (a->has_power_command_issuer) {
            const SGIModelController *issuer;
            const SGIModelModule *issuer_module;

            if (!a->has_main_power || a->power_command_issuer >= controllers) {
                goto unsupported;
            }
            issuer = SGI_MODEL_NTH(t->controllers, a->power_command_issuer);
            if (issuer->kind != SGI_MODEL_CONTROLLER_KIND_L1 ||
                issuer->module_index >= modules) {
                goto unsupported;
            }
            issuer_module = SGI_MODEL_NTH(t->modules, issuer->module_index);
            if (!issuer_module->nodes ||
                issuer_module->controller_index != a->power_command_issuer) {
                goto unsupported;
            }
        }
        for (other = t->io_assemblies; other != entry; other = other->next) {
            if (other->value->placement == a->placement ||
                other->value->module_index == a->module_index ||
                other->value->controller_index == a->controller_index ||
                (a->has_power_command_issuer &&
                 other->value->has_power_command_issuer &&
                 other->value->power_command_issuer ==
                 a->power_command_issuer)) {
                goto unsupported;
            }
        }
        for (port = a->head_ports; port; port = port->next) {
            if (port->value != 8 && port->value != 9 &&
                port->value != 12 && port->value != 13) {
                goto unsupported;
            }
            for (other_port = a->head_ports; other_port != port;
                 other_port = other_port->next) {
                if (other_port->value == port->value) {
                    goto unsupported;
                }
            }
        }
        for (pin = a->intx_map; pin; pin = pin->next) {
            if (pin->value >= 8) {
                goto unsupported;
            }
        }
        assigned[a->peer_index] = true;
    }
    for (link = t->cables; link; link = link->next) {
        const SGIModelLinkEnd *ends[2];

        if (QAPI_LIST_LENGTH(link->value->ends) != 2) {
            goto unsupported;
        }
        ends[0] = link->value->ends->value;
        ends[1] = link->value->ends->next->value;
        for (unsigned side = 0; side < 2; side++) {
            const SGIModelLinkEnd *end = ends[side];
            const SGIModelLinkEnd *other = ends[side ^ 1];

            switch (end->kind) {
            case SGI_MODEL_LINK_END_KIND_NODE:
                if (end->index >= nodes || end->port) {
                    goto unsupported;
                }
                break;
            case SGI_MODEL_LINK_END_KIND_IO_PEER:
                if (end->index >= peers || end->port || connected[end->index] ||
                    (other->kind != SGI_MODEL_LINK_END_KIND_NODE &&
                     other->kind != SGI_MODEL_LINK_END_KIND_ROUTER)) {
                    goto unsupported;
                }
                if (other->kind == SGI_MODEL_LINK_END_KIND_ROUTER &&
                    (other->port < 2 || other->port > 5)) {
                    goto unsupported;
                }
                connected[end->index] = true;
                break;
            case SGI_MODEL_LINK_END_KIND_ROUTER:
                if (end->index >= routers || end->port < 1 || end->port > 8 ||
                    router_ports[end->index * 8 + end->port - 1]) {
                    goto unsupported;
                }
                router_ports[end->index * 8 + end->port - 1] = true;
                break;
            default:
                goto unsupported;
            }
        }
    }
    for (size_t i = 0; i < peers; i++) {
        if (!connected[i]) {
            goto unsupported;
        }
    }
    return true;

unsupported:
    error_setg(errp, "topology '%s' names I/O-only network endpoints; "
               "their complete experimental runtime composition is not supported",
               t->name);
    return false;
}

/*
 * The position each chassis-wide list entry takes among the entries a
 * row keeps, or -1 for one it leaves out; false, with @errp set, for a
 * row naming an entry twice or out of range.
 */
static bool sgi_model_position_map(const uint32List *kept, int *map,
                                   size_t total, const char *what,
                                   const char *topology, Error **errp)
{
    const uint32List *entry;
    int next = 0;
    size_t i;

    for (i = 0; i < total; i++) {
        map[i] = -1;
    }
    for (entry = kept; entry; entry = entry->next) {
        if (entry->value >= total || map[entry->value] >= 0) {
            error_setg(errp, "topology '%s' names %s %u twice or out of "
                       "range in a node-population row", topology, what,
                       entry->value);
            return false;
        }
        map[entry->value] = next++;
    }
    return true;
}

static bool sgi_model_kept(const int *map, size_t total, int64_t index)
{
    return index >= 0 && (uint64_t)index < total && map[index] >= 0;
}

/*
 * Whether each of @t's xio_attachments is its node's own host entry: a
 * Crossbow-attached node's first entry is, and any later one places one
 * of io_boards[] (the rule sgi_machine_index_xio_attachments applies).
 */
static bool *sgi_model_host_attachments(const SGIModelTopology *t,
                                        size_t node_total)
{
    size_t total = QAPI_LIST_LENGTH(t->xio_attachments);
    bool *host = g_new0(bool, total ? total : 1);
    g_autofree bool *seen = g_new0(bool, node_total ? node_total : 1);
    const SGIModelXioAttachmentList *entry;
    size_t i = 0;

    for (entry = t->xio_attachments; entry; entry = entry->next, i++) {
        uint32_t node = entry->value->node_index;

        if (entry->value->crossbar_index >= 0 && node < node_total &&
            !seen[node]) {
            seen[node] = true;
            host[i] = true;
        }
    }
    return host;
}

/*
 * Checks everything sgi_model_build_view relies on before it allocates
 * anything, so the build itself cannot fail half way: every index a kept
 * record holds must survive renumbering.
 */
static bool sgi_model_check_row(const SGIModelTopology *t,
                                const SGIModelNodePopulation *row,
                                const int *node_map, const int *router_map,
                                const int *crossbar_map, const bool *host,
                                Error **errp)
{
    size_t node_total = QAPI_LIST_LENGTH(t->nodes);
    size_t router_total = QAPI_LIST_LENGTH(t->routers);
    size_t peer_total = QAPI_LIST_LENGTH(t->io_peers);
    size_t crossbar_total = QAPI_LIST_LENGTH(t->crossbars);
    size_t cable_total = QAPI_LIST_LENGTH(t->cables);
    size_t slot_total = QAPI_LIST_LENGTH(t->xio_slots);
    size_t attachment_total = QAPI_LIST_LENGTH(t->xio_attachments);
    size_t pci_host_total = QAPI_LIST_LENGTH(t->pci_host_bridges);
    const SGIModelXioAttachmentList *attachment;
    const SGIModelPciHostBridgeList *pci_host;
    const SGIModelIoBoardList *io_board;
    const SGIModelPopulationSlotList *usable;
    const uint32List *cable;
    g_autofree bool *pci_host_attachment =
        g_new0(bool, attachment_total ? attachment_total : 1);
    size_t i;

    for (cable = row->cables; cable; cable = cable->next) {
        const SGIModelLinkEndList *end;

        if (cable->value >= cable_total) {
            error_setg(errp, "topology '%s' names cable %u out of range in a "
                       "node-population row", t->name, cable->value);
            return false;
        }
        for (end = SGI_MODEL_NTH(t->cables, cable->value)->ends; end;
             end = end->next) {
            bool kept;
            const char *kind;

            switch (end->value->kind) {
            case SGI_MODEL_LINK_END_KIND_NODE:
                kept = sgi_model_kept(node_map, node_total, end->value->index);
                kind = "node";
                break;
            case SGI_MODEL_LINK_END_KIND_ROUTER:
                kept = sgi_model_kept(router_map, router_total,
                                      end->value->index);
                kind = "Router";
                break;
            case SGI_MODEL_LINK_END_KIND_IO_PEER:
                kept = end->value->index < peer_total;
                kind = "I/O peer";
                break;
            default:
                kept = false;
                kind = "supported endpoint";
                break;
            }
            if (!kept) {
                error_setg(errp, "topology '%s' fits cable %u without the "
                           "%s at one of its ends", t->name, cable->value,
                           kind);
                return false;
            }
        }
    }
    for (pci_host = t->pci_host_bridges; pci_host;
         pci_host = pci_host->next) {
        uint32_t index = pci_host->value->xio_attachment_index;

        if (index >= attachment_total) {
            error_setg(errp, "topology '%s' gives PCI host '%s' an XIO "
                       "attachment index out of range", t->name,
                       pci_host->value->id);
            return false;
        }
        if (pci_host_attachment[index]) {
            error_setg(errp, "topology '%s' assigns XIO attachment %u to "
                       "more than one PCI host", t->name, index);
            return false;
        }
        pci_host_attachment[index] = true;
    }
    for (attachment = t->xio_attachments, i = 0; attachment;
         attachment = attachment->next, i++) {
        const SGIModelXioAttachment *a = attachment->value;

        if (host[i] || pci_host_attachment[i]) {
            continue;
        }
        if (!sgi_model_kept(node_map, node_total, a->node_index) ||
            (a->crossbar_index >= 0 &&
             !sgi_model_kept(crossbar_map, crossbar_total,
                             a->crossbar_index))) {
            error_setg(errp, "topology '%s': an I/O board needs node %u, "
                       "which a node-population row leaves out", t->name,
                       a->node_index);
            return false;
        }
    }
    for (usable = row->xio_slots; usable; usable = usable->next) {
        const SGIModelXioSlot *slot;

        if (usable->value->slot >= slot_total) {
            error_setg(errp, "topology '%s' names XIO slot %u out of range "
                       "in a node-population row", t->name,
                       usable->value->slot);
            return false;
        }
        slot = SGI_MODEL_NTH(t->xio_slots, usable->value->slot);
        if (!sgi_model_kept(node_map, node_total,
                            usable->value->controlling_node) ||
            !sgi_model_kept(crossbar_map, crossbar_total,
                            slot->crossbar_index)) {
            error_setg(errp, "topology '%s' gives XIO slot %s to a node its "
                       "row does not fit, or on a Crossbow none reaches",
                       t->name, slot->label);
            return false;
        }
    }
    for (io_board = t->io_boards; io_board; io_board = io_board->next) {
        if (io_board->value->has_pci_host_index &&
            io_board->value->pci_host_index >= pci_host_total) {
            error_setg(errp, "topology '%s' gives I/O board '%s' a PCI host "
                       "index out of range", t->name, io_board->value->id);
            return false;
        }
    }
    if (!sgi_model_kept(node_map, node_total, t->console)) {
        error_setg(errp, "topology '%s' leaves out its console node in a "
                   "node-population row", t->name);
        return false;
    }
    return true;
}

/*
 * The view shares every record it keeps unchanged with the catalogue
 * (nodes, Routers, Crossbows, boards, controllers) and copies only those
 * whose indices it renumbers. Views live as long as the catalogue, so
 * nothing here is ever freed.
 */
static SGIModelTopology *sgi_model_build_view(const SGIModelTopology *t,
                                              const SGIModelNodePopulation *row,
                                              Error **errp)
{
    size_t node_total = QAPI_LIST_LENGTH(t->nodes);
    size_t router_total = QAPI_LIST_LENGTH(t->routers);
    size_t crossbar_total = QAPI_LIST_LENGTH(t->crossbars);
    size_t attachment_total = QAPI_LIST_LENGTH(t->xio_attachments);
    size_t pci_host_total = QAPI_LIST_LENGTH(t->pci_host_bridges);
    g_autofree int *node_map = g_new(int, node_total ? node_total : 1);
    g_autofree int *router_map = g_new(int, router_total ? router_total : 1);
    g_autofree int *crossbar_map =
        g_new(int, crossbar_total ? crossbar_total : 1);
    g_autofree int *attachment_map =
        g_new(int, attachment_total ? attachment_total : 1);
    g_autofree int *pci_host_map =
        g_new(int, pci_host_total ? pci_host_total : 1);
    g_autofree bool *host = NULL;
    const SGIModelXioAttachmentList *attachment;
    const SGIModelPciHostBridgeList *pci_host;
    const SGIModelIoBoardList *io_board;
    const SGIModelPopulationSlotList *usable;
    const SGIModelModuleList *module;
    const SGIModelRouterList *router;
    const SGIModelCrossbarList *crossbar;
    const uint32List *entry;
    SGIModelTopology *view;
    SGIModelNodePopulation *own;
    size_t i;
    int next;

    if (t->io_peers) {
        size_t cable_total = QAPI_LIST_LENGTH(t->cables);
        g_autofree int *cable_map = g_new(int, cable_total ? cable_total : 1);

        if (QAPI_LIST_LENGTH(row->nodes) != node_total ||
            QAPI_LIST_LENGTH(row->routers) != router_total ||
            QAPI_LIST_LENGTH(row->cables) != cable_total) {
            error_setg(errp, "topology '%s' with I/O peers requires its "
                       "complete population", t->name);
            return NULL;
        }
        if (!sgi_model_position_map(row->cables, cable_map, cable_total,
                                    "cable", t->name, errp)) {
            return NULL;
        }
    }
    if (!sgi_model_position_map(row->nodes, node_map, node_total, "node",
                                t->name, errp) ||
        !sgi_model_position_map(row->routers, router_map, router_total,
                                "router", t->name, errp)) {
        return NULL;
    }
    host = sgi_model_host_attachments(t, node_total);
    /*
     * A Crossbow stays only while a fitted node board's Hub reaches it:
     * with none, nothing can address it, and its widgets are
     * unreachable anyway.
     */
    for (i = 0; i < crossbar_total; i++) {
        crossbar_map[i] = -1;
    }
    for (i = 0; i < attachment_total; i++) {
        attachment_map[i] = -1;
    }
    for (i = 0; i < pci_host_total; i++) {
        pci_host_map[i] = -1;
    }
    for (attachment = t->xio_attachments, i = 0; attachment;
         attachment = attachment->next, i++) {
        const SGIModelXioAttachment *a = attachment->value;

        if (host[i] && sgi_model_kept(node_map, node_total, a->node_index) &&
            a->crossbar_index < crossbar_total) {
            crossbar_map[a->crossbar_index] = 0;
        }
    }
    for (i = 0, next = 0; i < crossbar_total; i++) {
        if (crossbar_map[i] >= 0) {
            crossbar_map[i] = next++;
        }
    }
    if (!sgi_model_check_row(t, row, node_map, router_map, crossbar_map, host,
                             errp)) {
        return NULL;
    }

    view = g_new0(SGIModelTopology, 1);
    view->name = t->name;
    view->product = t->product;
    view->generation = t->generation;
    view->description = t->description;
    view->has_pci_host_bridges = t->has_pci_host_bridges;
    view->controllers = t->controllers;
    view->has_io_peers = t->has_io_peers;
    view->io_peers = t->io_peers;
    view->has_io_assemblies = t->has_io_assemblies;
    view->io_assemblies = t->io_assemblies;
    view->l1_management = t->l1_management;
    view->ip27_firmware = t->ip27_firmware;
    view->ip35_firmware = t->ip35_firmware;
    view->has_processor_options = t->has_processor_options;
    view->processor_options = t->processor_options;
    view->console = node_map[t->console];
    view->junk_uart = sgi_model_kept(node_map, node_total, t->junk_uart) ?
                      node_map[t->junk_uart] : -1;

    {
        SGIModelNodeList **tail = &view->nodes;

        /* Node boards in the row's fill order, n1 first. */
        for (entry = row->nodes; entry; entry = entry->next) {
            QAPI_LIST_APPEND(tail, SGI_MODEL_NTH(t->nodes, entry->value));
        }
    }
    {
        SGIModelRouterList **tail = &view->routers;

        for (router = t->routers, i = 0; router; router = router->next, i++) {
            if (router_map[i] >= 0) {
                QAPI_LIST_APPEND(tail, router->value);
            }
        }
    }
    {
        SGIModelCrossbarList **tail = &view->crossbars;

        for (crossbar = t->crossbars, i = 0; crossbar;
             crossbar = crossbar->next, i++) {
            if (crossbar_map[i] >= 0) {
                QAPI_LIST_APPEND(tail, crossbar->value);
            }
        }
    }
    {
        SGIModelModuleList **tail = &view->modules;

        for (module = t->modules; module; module = module->next) {
            SGIModelModule *copy = g_memdup2(module->value, sizeof(*copy));
            uint32List **node_tail = &copy->nodes;

            copy->nodes = NULL;
            for (entry = module->value->nodes; entry; entry = entry->next) {
                if (sgi_model_kept(node_map, node_total, entry->value)) {
                    QAPI_LIST_APPEND(node_tail, node_map[entry->value]);
                }
            }
            QAPI_LIST_APPEND(tail, copy);
        }
    }
    {
        SGIModelCableList **tail = &view->cables;

        for (entry = row->cables; entry; entry = entry->next) {
            const SGIModelCable *cable = SGI_MODEL_NTH(t->cables, entry->value);
            SGIModelCable *copy = g_memdup2(cable, sizeof(*copy));
            SGIModelLinkEndList **end_tail = &copy->ends;
            const SGIModelLinkEndList *end;

            copy->ends = NULL;
            for (end = cable->ends; end; end = end->next) {
                SGIModelLinkEnd *renumbered = g_memdup2(end->value,
                                                        sizeof(*renumbered));

                if (renumbered->kind == SGI_MODEL_LINK_END_KIND_NODE) {
                    renumbered->index = node_map[renumbered->index];
                } else if (renumbered->kind == SGI_MODEL_LINK_END_KIND_ROUTER) {
                    renumbered->index = router_map[renumbered->index];
                }
                QAPI_LIST_APPEND(end_tail, renumbered);
            }
            QAPI_LIST_APPEND(tail, copy);
        }
    }
    {
        SGIModelXioAttachmentList **tail = &view->xio_attachments;

        for (attachment = t->xio_attachments, i = 0; attachment;
             attachment = attachment->next, i++) {
            SGIModelXioAttachment *copy;

            if (!sgi_model_kept(node_map, node_total,
                                attachment->value->node_index)) {
                /* An absent node board's own host link: not present. */
                continue;
            }
            copy = g_memdup2(attachment->value, sizeof(*copy));
            copy->node_index = node_map[copy->node_index];
            if (copy->crossbar_index >= 0) {
                copy->crossbar_index = crossbar_map[copy->crossbar_index];
            }
            attachment_map[i] = QAPI_LIST_LENGTH(view->xio_attachments);
            QAPI_LIST_APPEND(tail, copy);
        }
    }
    {
        SGIModelPciHostBridgeList **tail = &view->pci_host_bridges;

        for (pci_host = t->pci_host_bridges, i = 0; pci_host;
             pci_host = pci_host->next, i++) {
            int new_attachment = attachment_map[
                pci_host->value->xio_attachment_index];
            SGIModelPciHostBridge *copy;

            if (new_attachment < 0) {
                continue;
            }
            pci_host_map[i] = QAPI_LIST_LENGTH(view->pci_host_bridges);
            if (new_attachment == pci_host->value->xio_attachment_index) {
                QAPI_LIST_APPEND(tail, pci_host->value);
            } else {
                copy = g_memdup2(pci_host->value, sizeof(*copy));
                copy->xio_attachment_index = new_attachment;
                QAPI_LIST_APPEND(tail, copy);
            }
        }
    }
    {
        SGIModelIoBoardList **tail = &view->io_boards;

        for (io_board = t->io_boards; io_board; io_board = io_board->next) {
            SGIModelIoBoard *copy;

            if (!io_board->value->has_pci_host_index) {
                QAPI_LIST_APPEND(tail, io_board->value);
                continue;
            }
            if (pci_host_map[io_board->value->pci_host_index] < 0) {
                continue;
            }
            if (pci_host_map[io_board->value->pci_host_index] ==
                io_board->value->pci_host_index) {
                QAPI_LIST_APPEND(tail, io_board->value);
            } else {
                copy = g_memdup2(io_board->value, sizeof(*copy));
                copy->pci_host_index =
                    pci_host_map[io_board->value->pci_host_index];
                QAPI_LIST_APPEND(tail, copy);
            }
        }
    }
    {
        SGIModelXioSlotList **tail = &view->xio_slots;

        for (usable = row->xio_slots; usable; usable = usable->next) {
            SGIModelXioSlot *copy =
                g_memdup2(SGI_MODEL_NTH(t->xio_slots, usable->value->slot),
                          sizeof(*copy));

            copy->controlling_node = node_map[usable->value->controlling_node];
            copy->crossbar_index = crossbar_map[copy->crossbar_index];
            QAPI_LIST_APPEND(tail, copy);
        }
    }

    /* The view's own one row, in its own positions. */
    own = g_new0(SGIModelNodePopulation, 1);
    {
        uint32List **tail = &own->nodes;

        for (i = 0; i < QAPI_LIST_LENGTH(row->nodes); i++) {
            QAPI_LIST_APPEND(tail, i);
        }
        tail = &own->routers;
        for (i = 0; i < QAPI_LIST_LENGTH(row->routers); i++) {
            QAPI_LIST_APPEND(tail, i);
        }
        tail = &own->cables;
        for (i = 0; i < QAPI_LIST_LENGTH(row->cables); i++) {
            QAPI_LIST_APPEND(tail, i);
        }
    }
    {
        SGIModelPopulationSlotList **tail = &own->xio_slots;
        const SGIModelXioSlotList *kept;

        for (kept = view->xio_slots, i = 0; kept; kept = kept->next, i++) {
            SGIModelPopulationSlot *place = g_new0(SGIModelPopulationSlot, 1);

            place->slot = i;
            place->controlling_node = kept->value->controlling_node;
            QAPI_LIST_APPEND(tail, place);
        }
    }
    own->supported_cpu_populations = row->supported_cpu_populations;
    own->grade = row->grade;
    QAPI_LIST_PREPEND(view->node_populations, own);
    return view;
}

const SGIModelTopology *sgi_model_population_view(
    const SGIModelTopology *t, const SGIModelNodePopulation *row,
    Error **errp)
{
    static GHashTable *views;
    SGIModelTopology *view;

    if (!views) {
        views = g_hash_table_new(NULL, NULL);
    }
    view = g_hash_table_lookup(views, row);
    if (!view) {
        view = sgi_model_build_view(t, row, errp);
        if (!view) {
            return NULL;
        }
        g_hash_table_insert(views, (gpointer)row, view);
    }
    return view;
}

const SGIModelNodePopulation *sgi_model_find_population(
    const SGIModelTopology *t, unsigned int nodes)
{
    const SGIModelNodePopulationList *entry;

    for (entry = t->node_populations; entry; entry = entry->next) {
        if (QAPI_LIST_LENGTH(entry->value->nodes) == nodes) {
            return entry->value;
        }
    }
    return NULL;
}

const SGIModelNodePopulation *sgi_model_default_population(
    const SGIModelTopology *t)
{
    const SGIModelNodePopulationList *entry;
    const SGIModelNodePopulation *largest = NULL;

    for (entry = t->node_populations; entry; entry = entry->next) {
        if (!largest || QAPI_LIST_LENGTH(entry->value->nodes) >
                        QAPI_LIST_LENGTH(largest->nodes)) {
            largest = entry->value;
        }
    }
    return largest;
}

bool sgi_model_topology_matches_product(const SGIModelTopology *t,
                                        const char *product)
{
    size_t len = strlen(product);
    size_t name_len = strlen(t->name);

    return name_len >= len && !strncmp(t->name, product, len) &&
           (t->name[len] == '\0' || t->name[len] == '-');
}

const SGIModelTopology *sgi_model_product_topology(
    const SGIMachines *machines, const char *product, const char *name)
{
    const SGIModelTopologyList *entry;

    for (entry = machines->topologies; entry; entry = entry->next) {
        if (!strcmp(entry->value->name, name)) {
            return sgi_model_topology_matches_product(entry->value, product) ?
                   entry->value : NULL;
        }
    }
    return NULL;
}

/* "1, 2, 3 or 4": a chassis's accepted node counts, for a refusal. */
static char *sgi_model_accepted_node_counts(const SGIModelTopology *t)
{
    GString *out = g_string_new(NULL);
    const SGIModelNodePopulationList *entry;

    for (entry = t->node_populations; entry; entry = entry->next) {
        if (out->len) {
            g_string_append(out, entry->next ? ", " : " or ");
        }
        g_string_append_printf(out, "%zu",
                               QAPI_LIST_LENGTH(entry->value->nodes));
    }
    return g_string_free(out, false);
}

const SGIModelTopology *sgi_model_resolve_population(
    const SGIModelTopology *chassis, unsigned int nodes, Error **errp)
{
    const SGIModelNodePopulation *row;

    if (!sgi_model_check_runtime_endpoints(chassis, errp)) {
        return NULL;
    }
    row = nodes ? sgi_model_find_population(chassis, nodes) :
        sgi_model_default_population(chassis);
    if (!row) {
        g_autofree char *counts = sgi_model_accepted_node_counts(chassis);

        error_setg(errp, "topology '%s' fits %s node board%s, not %u",
                   chassis->name, counts,
                   g_str_equal(counts, "1") ? "" : "s", nodes);
        return NULL;
    }
    if (row->grade == SGI_MODEL_EVIDENCE_GRADE_UNKNOWN) {
        size_t count = QAPI_LIST_LENGTH(row->nodes);

        error_setg(errp, "topology '%s' with %zu node board%s rests on "
                   "evidence that does not settle its wiring; it is not "
                   "built", chassis->name, count, count == 1 ? "" : "s");
        return NULL;
    }
    return sgi_model_population_view(chassis, row, errp);
}

const SGIModelProcessorOption *sgi_model_find_processor_option(
    const SGIModelTopology *t, const char *id, Error **errp)
{
    const SGIModelProcessorOptionList *entry;
    GString *ids;

    for (entry = t->processor_options; entry; entry = entry->next) {
        if (g_str_equal(entry->value->assembly->id, id)) {
            return entry->value;
        }
    }
    if (!t->processor_options) {
        error_setg(errp, "topology '%s' offers no processor choice", t->name);
        return NULL;
    }
    ids = g_string_new(NULL);
    for (entry = t->processor_options; entry; entry = entry->next) {
        g_string_append_printf(ids, "%s%s", ids->len ? ", " : "",
                               entry->value->assembly->id);
    }
    error_setg(errp, "topology '%s' offers no processor '%s'; it offers %s",
               t->name, id, ids->str);
    g_string_free(ids, true);
    return NULL;
}

/* The topology @product builds with no topology=, as its offerings name. */
static const char *sgi_model_offered_default(const SGIMachines *machines,
                                             const char *product)
{
    const SGIModelOfferingList *entry;

    for (entry = machines->offerings; entry; entry = entry->next) {
        if (g_str_equal(entry->value->product, product)) {
            return entry->value->default_topology;
        }
    }
    return NULL;
}

const SGIModelOffering *sgi_model_resolve_options(const SGIMachines *machines,
                                                  const char *options,
                                                  Error **errp)
{
    g_autoptr(QDict) args = keyval_parse(options, "type", NULL, errp);
    const SGIModelOfferingList *entry;
    const SGIModelTopology *chassis, *view;
    const QDictEntry *option;
    const char *product, *name, *nodes_text, *processor;
    uint64_t nodes = 0;

    if (!args) {
        return NULL;
    }
    for (option = qdict_first(args); option;
         option = qdict_next(args, option)) {
        const char *key = qdict_entry_key(option);

        if (!g_str_equal(key, "type") && !g_str_equal(key, "topology") &&
            !g_str_equal(key, "nodes") && !g_str_equal(key, "processor")) {
            error_setg(errp, "machine option '%s' is not part of a "
                       "catalogue selection; give only the machine, "
                       "topology=, nodes= and processor=", key);
            return NULL;
        }
    }
    product = qdict_get_try_str(args, "type");
    if (!product) {
        error_setg(errp, "no machine named in '%s'", options);
        return NULL;
    }
    name = qdict_get_try_str(args, "topology");
    if (!name) {
        name = sgi_model_offered_default(machines, product);
        if (!name) {
            error_setg(errp, "unknown machine '%s'", product);
            return NULL;
        }
    }
    nodes_text = qdict_get_try_str(args, "nodes");
    if (nodes_text &&
        (qemu_strtou64(nodes_text, NULL, 10, &nodes) < 0 || !nodes ||
         nodes > UINT32_MAX)) {
        error_setg(errp, "nodes must be a node board count, not '%s'",
                   nodes_text);
        return NULL;
    }
    chassis = sgi_model_product_topology(machines, product, name);
    if (!chassis) {
        error_setg(errp, "%s has no topology '%s'", product, name);
        return NULL;
    }
    view = sgi_model_resolve_population(chassis, nodes, errp);
    if (!view) {
        return NULL;
    }
    processor = qdict_get_try_str(args, "processor");
    if (processor) {
        if (!sgi_model_find_processor_option(view, processor, errp)) {
            return NULL;
        }
    } else {
        processor = view->nodes->value->cpu_slots->value->assembly->id;
    }
    for (entry = machines->offerings; entry; entry = entry->next) {
        const SGIModelOffering *offering = entry->value;

        if (g_str_equal(offering->product, product) &&
            g_str_equal(offering->topology, chassis->name) &&
            offering->nodes == QAPI_LIST_LENGTH(view->nodes) &&
            g_str_equal(offering->processor->id, processor)) {
            return offering;
        }
    }
    error_setg(errp, "topology '%s' with %zu node board%s is not offered",
               chassis->name, QAPI_LIST_LENGTH(view->nodes),
               QAPI_LIST_LENGTH(view->nodes) == 1 ? "" : "s");
    return NULL;
}

bool sgi_model_io_board_has_io_flash(const SGIModelIoBoard *board)
{
    switch (board->preset) {
    case SGI_MODEL_IO_BOARD_PRESET_IO6:
    case SGI_MODEL_IO_BOARD_PRESET_IO6G:
    case SGI_MODEL_IO_BOARD_PRESET_GIGACHANNEL:
        return true;
    default:
        return false;
    }
}

/*
 * A selected system Timekeeper, or a base I/O design whose IOC3 is fitted
 * (and so carries the board's Timekeeper); an add-in card's IOC3 has none.
 */
bool sgi_model_io_board_has_nvram(const SGIModelIoBoard *board)
{
    if (board->system_timekeeper) {
        return true;
    }
    switch (board->preset) {
    case SGI_MODEL_IO_BOARD_PRESET_IP29_IO:
    case SGI_MODEL_IO_BOARD_PRESET_IP29_IO_CROSSTOWN:
    case SGI_MODEL_IO_BOARD_PRESET_IO6:
    case SGI_MODEL_IO_BOARD_PRESET_IO6G:
    case SGI_MODEL_IO_BOARD_PRESET_IO8:
    case SGI_MODEL_IO_BOARD_PRESET_IO9:
        return board->has_ioc3_serial;
    default:
        return false;
    }
}

/* Whether @module's controller is a listed Origin 2000-family MSC. */
static bool sgi_model_module_has_msc(const SGIModelTopology *t,
                                     const SGIModelModule *module)
{
    return module->controller_index >= 0 &&
           (size_t)module->controller_index <
           QAPI_LIST_LENGTH(t->controllers) &&
           SGI_MODEL_NTH(t->controllers, module->controller_index)->kind ==
           SGI_MODEL_CONTROLLER_KIND_MSC;
}

bool sgi_model_module_has_nvram(const SGIModelTopology *t,
                                const SGIModelModule *module)
{
    size_t nodes = QAPI_LIST_LENGTH(t->nodes);
    const uint32List *entry;

    if (sgi_model_module_has_msc(t, module)) {
        return true;
    }
    for (entry = module->nodes; entry; entry = entry->next) {
        const SGIModelNode *node;

        if (entry->value >= nodes) {
            continue;
        }
        node = SGI_MODEL_NTH(t->nodes, entry->value);
        if (node->identity && node->identity->system_controller) {
            return true;
        }
    }
    return false;
}

unsigned int sgi_model_module_nvram_index(const SGIModelTopology *t,
                                          const SGIModelModule *module)
{
    const SGIModelModuleList *entry;
    unsigned int index = 0;

    for (entry = t->modules; entry && entry->value != module;
         entry = entry->next) {
        index += sgi_model_module_has_nvram(t, entry->value);
    }
    return index;
}

void sgi_model_module_nvram_seed(const SGIModelTopology *t,
                                 const SGIModelModule *module,
                                 uint8_t *config, uint8_t *module_number,
                                 uint8_t *clock)
{
    if (sgi_model_module_has_msc(t, module)) {
        *config = 0;
        *module_number = module->initial_module_number;
        *clock = 0;
    } else {
        /*
         * The Origin 200 build: automatic power-up, no module number, and
         * the full-clock midplane type it always assumes.
         */
        *config = SGI_ELSC_NVRAM_CONFIG_AUTO_POWER;
        *module_number = 0;
        *clock = SGI_ELSC_NVRAM_CLOCK_FULL;
    }
}

unsigned int sgi_model_io_board_index(
    const SGIModelTopology *t, const SGIModelIoBoard *board,
    bool (*has)(const SGIModelIoBoard *board))
{
    const SGIModelIoBoardList *entry;
    unsigned int index = 0;

    for (entry = t->io_boards; entry && entry->value != board;
         entry = entry->next) {
        index += has(entry->value);
    }
    return index;
}
