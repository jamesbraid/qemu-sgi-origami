/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SGI Scalable Node family's compiled catalogue: hw/mips/sgi/
 * sn-catalogue.json, embedded into the emulator at build time and parsed once,
 * into the QAPI-generated types qapi/sgi-sn.json declares.
 */

#ifndef HW_MIPS_SGI_SN_CATALOGUE_H
#define HW_MIPS_SGI_SN_CATALOGUE_H

#include "qapi/qapi-types-sgi-sn.h"

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

#endif /* HW_MIPS_SGI_SN_CATALOGUE_H */
