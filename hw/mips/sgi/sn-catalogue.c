/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SGI Scalable Node family's compiled catalogue: parses the embedded
 * hw/mips/sgi/sn-catalogue.json once, into the QAPI-generated
 * SGIModelCatalogue types qapi/sgi-sn.json declares, and keeps the
 * result for the process lifetime. hw/uefi/var-service-json.c is this
 * tree's other example of parsing JSON into QAPI types with
 * qobject_from_json() and qobject_input_visitor_new().
 */

#include "qemu/osdep.h"
#include "qapi/clone-visitor.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-sgi-sn.h"
#include "qapi/qapi-types-sgi-sn.h"
#include "qapi/qobject-input-visitor.h"
#include "qobject/qjson.h"
#include "qobject/qobject.h"
#include "hw/mips/sgi/sn-catalogue.h"

/* sn_catalogue_json: hw/mips/sgi/sn-catalogue.json, embedded by the
 * custom_target hw/mips/sgi/meson.build defines.
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

SGIModelCatalogue *qmp_query_sgi_sn_catalogue(Error **errp)
{
    return QAPI_CLONE(SGIModelCatalogue, sgi_model_catalogue());
}
