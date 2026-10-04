/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Parse embedded hw/mips/sgi/sgi-machines.json into qapi/sgi-machines.json's
 * SGIMachines once. Retain it for the process lifetime;
 * hw/uefi/var-service-json.c also uses qobject_from_json() and
 * qobject_input_visitor_new() to parse JSON into QAPI types.
 */

#include "qemu/osdep.h"
#include "qapi/clone-visitor.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-sgi-machines.h"
#include "qapi/qapi-types-sgi-machines.h"
#include "qapi/qapi-visit-sgi-machines.h"
#include "qapi/qobject-input-visitor.h"
#include "qapi/util.h"
#include "qobject/qjson.h"
#include "qobject/qobject.h"
#include "hw/mips/sgi/sgi-machines.h"

/*
 * sgi_machines_json embeds hw/mips/sgi/sgi-machines.json via
 * hw/mips/sgi/meson.build.
 */
#include "sgi-machines-embed.c.inc"

static SGIMachines *machines;

static void sgi_machines_load(void)
{
    QObject *qobj;
    Visitor *v;

    if (machines) {
        return;
    }

    qobj = qobject_from_json(sgi_machines_json, &error_abort);
    v = qobject_input_visitor_new(qobj);
    visit_type_SGIMachines(v, NULL, &machines, &error_fatal);
    visit_free(v);
    qobject_unref(qobj);
}

const SGIMachines *sgi_machines(void)
{
    sgi_machines_load();
    return machines;
}

const SGIModelTopology *sgi_model_find_topology(const char *name)
{
    SGIModelTopologyList *entry;

    sgi_machines_load();
    for (entry = machines->topologies; entry; entry = entry->next) {
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

    sgi_machines_load();
    for (entry = machines->topologies; entry; entry = entry->next) {
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

SGIMachines *qmp_query_sgi_machines(Error **errp)
{
    return QAPI_CLONE(SGIMachines, sgi_machines());
}
