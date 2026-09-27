/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Parse embedded hw/mips/sgi/sn-catalogue.json into qapi/sgi-sn.json's
 * SGIModelCatalogue once. Retain it for the process lifetime;
 * hw/uefi/var-service-json.c also uses qobject_from_json() and
 * qobject_input_visitor_new() to parse JSON into QAPI types.
 */

#include "qemu/osdep.h"
#include "qapi/clone-visitor.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-sgi-sn.h"
#include "qapi/qapi-types-sgi-sn.h"
#include "qapi/qapi-visit-sgi-sn.h"
#include "qapi/qobject-input-visitor.h"
#include "qapi/util.h"
#include "qobject/qjson.h"
#include "qobject/qobject.h"
#include "hw/mips/sgi/sn-catalogue.h"

/*
 * sn_catalogue_json embeds hw/mips/sgi/sn-catalogue.json via
 * hw/mips/sgi/meson.build.
 */
#include "sn-catalogue-embed.c.inc"

static SGIModelCatalogue *sn_catalogue;

static void sn_catalogue_load(void)
{
    QObject *qobj;
    Visitor *v;

    if (sn_catalogue) {
        return;
    }

    qobj = qobject_from_json(sn_catalogue_json, &error_abort);
    v = qobject_input_visitor_new(qobj);
    visit_type_SGIModelCatalogue(v, NULL, &sn_catalogue, &error_fatal);
    visit_free(v);
    qobject_unref(qobj);
}

const SGIModelCatalogue *sgi_model_catalogue(void)
{
    sn_catalogue_load();
    return sn_catalogue;
}

const SGIModelTopology *sgi_model_find_topology(const char *name)
{
    SGIModelTopologyList *entry;

    sn_catalogue_load();
    for (entry = sn_catalogue->topologies; entry; entry = entry->next) {
        if (!strcmp(entry->value->name, name)) {
            return entry->value;
        }
    }
    return NULL;
}

const char *sgi_model_default_topology(const char *product)
{
    SGIModelTopologyList *entry;
    const char *single = NULL;
    unsigned int matches = 0;

    sn_catalogue_load();
    for (entry = sn_catalogue->topologies; entry; entry = entry->next) {
        const SGIModelTopology *topology = entry->value;

        if (strcmp(topology->product, product)) {
            continue;
        }
        if (!strcmp(topology->name, product)) {
            return topology->name;
        }
        matches++;
        single = topology->name;
    }
    return matches == 1 ? single : NULL;
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

/* The @index'th element of a QAPI list, which the caller has range-checked. */
#define SGI_MODEL_NTH(list, index) ({                                  \
    typeof_strip_qual(list) nth_ = (list);                              \
    for (size_t i_ = 0; i_ < (index); i_++) {                           \
        nth_ = nth_->next;                                              \
    }                                                                   \
    nth_->value;                                                        \
})

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
 * Whether each of @t's xtalk_attachments is its node's own host entry: a
 * Crossbow-attached node's first entry is, and any later one places one
 * of io_boards[] (the rule sgi_machine_index_xtalk_attachments applies).
 */
static bool *sgi_model_host_attachments(const SGIModelTopology *t,
                                        size_t node_total)
{
    size_t total = QAPI_LIST_LENGTH(t->xtalk_attachments);
    bool *host = g_new0(bool, total ? total : 1);
    g_autofree bool *seen = g_new0(bool, node_total ? node_total : 1);
    const SGIModelXtalkAttachmentList *entry;
    size_t i = 0;

    for (entry = t->xtalk_attachments; entry; entry = entry->next, i++) {
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
    size_t crossbar_total = QAPI_LIST_LENGTH(t->crossbars);
    size_t cable_total = QAPI_LIST_LENGTH(t->cables);
    size_t slot_total = QAPI_LIST_LENGTH(t->xtalk_slots);
    const SGIModelXtalkAttachmentList *attachment;
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
            bool is_node = end->value->kind == SGI_MODEL_LINK_END_KIND_NODE;

            if (!sgi_model_kept(is_node ? node_map : router_map,
                                is_node ? node_total : router_total,
                                end->value->index)) {
                error_setg(errp, "topology '%s' fits cable %u without the "
                           "%s at one of its ends", t->name, cable->value,
                           is_node ? "node" : "Router");
                return false;
            }
        }
    }
    for (attachment = t->xtalk_attachments, i = 0; attachment;
         attachment = attachment->next, i++) {
        const SGIModelXtalkAttachment *a = attachment->value;

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
    for (usable = row->xtalk_slots; usable; usable = usable->next) {
        const SGIModelXtalkSlot *slot;

        if (usable->value->slot >= slot_total) {
            error_setg(errp, "topology '%s' names XIO slot %u out of range "
                       "in a node-population row", t->name,
                       usable->value->slot);
            return false;
        }
        slot = SGI_MODEL_NTH(t->xtalk_slots, usable->value->slot);
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
    const SGIModelXtalkAttachmentList *attachment;
    const SGIModelPopulationSlotList *usable;
    const SGIModelModuleList *module;
    const SGIModelRouterList *router;
    const SGIModelCrossbarList *crossbar;
    const uint32List *entry;
    SGIModelTopology *view;
    SGIModelNodePopulation *own;
    size_t i;
    int next;

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
    for (attachment = t->xtalk_attachments, i = 0; attachment;
         attachment = attachment->next, i++) {
        const SGIModelXtalkAttachment *a = attachment->value;

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

                renumbered->index =
                    renumbered->kind == SGI_MODEL_LINK_END_KIND_NODE ?
                    node_map[renumbered->index] :
                    router_map[renumbered->index];
                QAPI_LIST_APPEND(end_tail, renumbered);
            }
            QAPI_LIST_APPEND(tail, copy);
        }
    }
    {
        SGIModelXtalkAttachmentList **tail = &view->xtalk_attachments;

        for (attachment = t->xtalk_attachments, i = 0; attachment;
             attachment = attachment->next, i++) {
            SGIModelXtalkAttachment *copy;

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
        SGIModelXtalkSlotList **tail = &view->xtalk_slots;

        for (usable = row->xtalk_slots; usable; usable = usable->next) {
            SGIModelXtalkSlot *copy =
                g_memdup2(SGI_MODEL_NTH(t->xtalk_slots, usable->value->slot),
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
        SGIModelPopulationSlotList **tail = &own->xtalk_slots;
        const SGIModelXtalkSlotList *kept;

        for (kept = view->xtalk_slots, i = 0; kept; kept = kept->next, i++) {
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

SGIModelCatalogue *qmp_query_sgi_sn_catalogue(Error **errp)
{
    return QAPI_CLONE(SGIModelCatalogue, sgi_model_catalogue());
}
