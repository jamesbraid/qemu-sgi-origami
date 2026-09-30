/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The SGI Scalable Node family's compiled catalogue: hw/mips/sgi/
 * sn-catalogue.json, embedded into the emulator at build time and parsed once,
 * into the QAPI-generated types qapi/sgi-sn.json declares.
 */

#ifndef HW_MIPS_SGI_SN_CATALOGUE_H
#define HW_MIPS_SGI_SN_CATALOGUE_H

#include "qapi/qapi-types-sgi-sn.h"
#include "qapi/error.h"

/*
 * The embedded catalogue, parsed on first use and kept for the process
 * lifetime. Never NULL and never returns after a parse failure: a malformed
 * embedded catalogue is a build defect, not a runtime one.
 */
const SGIModelCatalogue *sgi_model_catalogue(void);

/* The catalogue's topology named @name, or NULL. */
const SGIModelTopology *sgi_model_find_topology(const char *name);

/*
 * The topology name a product boots with when topology= is omitted, or NULL
 * when @product has no unambiguous default: a topology named exactly like
 * @product is its default; a product with only one topology has an unambiguous
 * default regardless of its name.
 */
const char *sgi_model_default_topology(const char *product);

/*
 * Refuse endpoints whose runtime construction is not implemented, before
 * population selection can drop or misclassify their records.
 */
bool sgi_model_check_runtime_endpoints(const SGIModelTopology *t, Error **errp);

/* @t's node-population row fitting exactly @nodes node boards, or NULL. */
const SGIModelNodePopulation *sgi_model_find_population(
    const SGIModelTopology *t, unsigned int nodes);

/* @t's row with the most node boards: what a machine fits by default. */
const SGIModelNodePopulation *sgi_model_default_population(
    const SGIModelTopology *t);

/*
 * The topology a machine builds for @row of chassis @t: only the row's
 * node boards, Routers, cables and usable XIO slots, every index
 * renumbered to those, and a Crossbow no fitted Hub reaches left out.
 * Its node_populations holds that one row in its own numbering. Built
 * once per row and kept for the process lifetime, like the catalogue
 * itself; NULL with @errp set when the row names a record out of range
 * or keeps one that needs a node board it does not fit.
 */
const SGIModelTopology *sgi_model_population_view(
    const SGIModelTopology *t, const SGIModelNodePopulation *row,
    Error **errp);

#endif /* HW_MIPS_SGI_SN_CATALOGUE_H */
