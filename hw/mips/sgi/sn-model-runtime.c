/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Validate machine endpoints and project structural population views. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/util.h"
#include "hw/mips/sgi/sn-catalogue.h"

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
    g_autofree bool *assigned = g_new0(bool, MAX(peers, 1));
    g_autofree bool *connected = g_new0(bool, MAX(peers, 1));

    if (!peers && !t->io_assemblies && !t->l1_management) {
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
    if (!peers || t->generation != SGI_MODEL_GENERATION_SN1 || t->routers ||
        QAPI_LIST_LENGTH(t->io_assemblies) != peers || !t->l1_management ||
        QAPI_LIST_LENGTH(t->l1_management->owners) != controllers) {
        goto unsupported;
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
        const SGIModelLinkEnd *first, *second;

        if (QAPI_LIST_LENGTH(link->value->ends) != 2) {
            goto unsupported;
        }
        first = link->value->ends->value;
        second = link->value->ends->next->value;
        if (first->kind == SGI_MODEL_LINK_END_KIND_IO_PEER) {
            const SGIModelLinkEnd *swap = first;
            first = second;
            second = swap;
        }
        if (first->kind != SGI_MODEL_LINK_END_KIND_NODE ||
            first->index >= nodes ||
            first->port || second->port) {
            goto unsupported;
        }
        if (second->kind == SGI_MODEL_LINK_END_KIND_IO_PEER) {
            if (second->index >= peers || connected[second->index]) {
                goto unsupported;
            }
            connected[second->index] = true;
        } else if (second->kind != SGI_MODEL_LINK_END_KIND_NODE ||
                   second->index >= nodes) {
            goto unsupported;
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
    const SGIModelXioAttachmentList *attachment;
    const SGIModelPopulationSlotList *usable;
    const uint32List *cable;
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
    for (attachment = t->xio_attachments, i = 0; attachment;
         attachment = attachment->next, i++) {
        const SGIModelXioAttachment *a = attachment->value;

        if (host[i]) {
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
    g_autofree int *node_map = g_new(int, node_total ? node_total : 1);
    g_autofree int *router_map = g_new(int, router_total ? router_total : 1);
    g_autofree int *crossbar_map =
        g_new(int, crossbar_total ? crossbar_total : 1);
    g_autofree bool *host = NULL;
    const SGIModelXioAttachmentList *attachment;
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
    view->io_boards = t->io_boards;
    view->controllers = t->controllers;
    view->has_io_peers = t->has_io_peers;
    view->io_peers = t->io_peers;
    view->has_io_assemblies = t->has_io_assemblies;
    view->io_assemblies = t->io_assemblies;
    view->l1_management = t->l1_management;
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
            QAPI_LIST_APPEND(tail, copy);
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
