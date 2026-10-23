/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The compiled SGI machine catalogue: hw/mips/sgi/sgi-machines.json,
 * embedded into the emulator at build time and parsed once into the
 * QAPI-generated types qapi/sgi-machines.json declares.
 */

#ifndef HW_MIPS_SGI_SGI_MACHINES_H
#define HW_MIPS_SGI_SGI_MACHINES_H

#include "qapi/qapi-types-sgi-machines.h"
#include "qapi/error.h"
#include "qobject/qdict.h"

/*
 * The embedded catalogue, parsed on first use and kept for the process
 * lifetime. Never NULL and never returns after a parse failure: a malformed
 * embedded catalogue is a build defect, not a runtime one.
 */
const SGIMachines *sgi_machines(void);

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

/*
 * Whether @t is one of @product's topologies: topology= is product-scoped,
 * so a name is @product itself or starts with "@product-".
 */
bool sgi_model_topology_matches_product(const SGIModelTopology *t,
                                        const char *product);

/* @machines' topology named @name if it is one of @product's, or NULL. */
const SGIModelTopology *sgi_model_product_topology(
    const SGIMachines *machines, const char *product, const char *name);

/*
 * The topology a machine builds from chassis @chassis with @nodes node
 * boards, or with its most node boards when @nodes is zero: the
 * population view of the matching row. NULL with @errp set when an
 * endpoint is not implemented, no row fits @nodes, or the row rests on
 * unknown evidence. The machine and standalone tools resolve through
 * this one function, so they agree on what a selection builds.
 */
const SGIModelTopology *sgi_model_resolve_population(
    const SGIModelTopology *chassis, unsigned int nodes, Error **errp);

/*
 * Whether @board fits a BASEIO PROM flash (an io<N>-flash store) or a
 * Timekeeper whose NVRAM persists (an nvram<N> store), by the rule the
 * catalogue generator applies when it lists a configuration's storage.
 */
bool sgi_model_io_board_has_io_flash(const SGIModelIoBoard *board);
bool sgi_model_io_board_has_nvram(const SGIModelIoBoard *board);

/*
 * The N of @board's io<N>-flash or nvram<N> store: how many of @t's
 * io_boards before @board @has accepts.
 */
unsigned int sgi_model_io_board_index(
    const SGIModelTopology *t, const SGIModelIoBoard *board,
    bool (*has)(const SGIModelIoBoard *board));

/*
 * A module system controller's configuration NVRAM (a module<N>-nvram
 * store), by the rule the catalogue generator applies: a module whose
 * Origin 2000 or Onyx2 module controller (MSC) the topology lists, or an
 * Origin 200 module one of whose fitted nodes' Hubs embeds the ELSC.
 * The store's N counts the modules before it that have one. On success
 * sgi_model_module_nvram_seed() gives the configuration, module and
 * clock-select (0x70b) bytes a new store starts with: the controller's own
 * power-up defaults, 0x02, 0 and 0x78 for the Origin 200 build, and 0 and
 * the module's initial module number for the Origin 2000 build, whose
 * 0x70b stays 0 (see sgi-sn0-module-management.c). A two-module Origin
 * 200's slave store is seeded like the master's, an approximation: its
 * controller does not initialize it, so its contents are unknown (an
 * erased part usually reads 0xff), and which store the slave Hub reaches
 * is unknown too.
 */
bool sgi_model_module_has_nvram(const SGIModelTopology *t,
                                const SGIModelModule *module);
unsigned int sgi_model_module_nvram_index(const SGIModelTopology *t,
                                          const SGIModelModule *module);
void sgi_model_module_nvram_seed(const SGIModelTopology *t,
                                 const SGIModelModule *module,
                                 uint8_t *config, uint8_t *module_number,
                                 uint8_t *clock);

/*
 * The offering a -M option string selects: "<machine>[,topology=T]
 * [,nodes=N][,processor=P]", resolved as the machine resolves it, with the
 * product's default topology from its offerings when topology= is absent
 * and the nodes' own part when processor= is. Any other option is
 * refused, since the catalogue does not describe it. Offerings of one
 * topology, node count and processor differ only in CPU population, so
 * this returns the first; its storage and init-inputs describe them all.
 */
const SGIModelOffering *sgi_model_resolve_options(const SGIMachines *machines,
                                                  const char *options,
                                                  Error **errp);

/*
 * The processor option @t offers under catalogue id @id, or NULL with
 * @errp naming the ids it does offer.
 */
const SGIModelProcessorOption *sgi_model_find_processor_option(
    const SGIModelTopology *t, const char *id, Error **errp);

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
 * or keeps one that needs a node board it does not fit. I/O peers require
 * the full node, Router and cable population and retain their indices.
 * This structural view does not authorize runtime construction: machine
 * callers must separately check sgi_model_check_runtime_endpoints().
 */
const SGIModelTopology *sgi_model_population_view(
    const SGIModelTopology *t, const SGIModelNodePopulation *row,
    Error **errp);

/*
 * One graphics position of a topology and the option it holds: the one
 * heads.<position>= names, or the position's default.
 */
typedef struct SGIHeadChoice {
    const SGIModelGraphicsPosition *position;
    const SGIModelGraphicsOption *option;
} SGIHeadChoice;

/*
 * Resolve @heads, the heads.<position>=<id> dictionary a machine stored
 * (NULL when the command line gave none), against topology @t: one
 * choice per modelled position, in @t's order, through @count. Refuses
 * an unknown position, an unknown or refused id, a position QEMU does not
 * model and more heads than @t builds, each prefixed with @machine, the
 * -M name. Returns NULL with @errp set on refusal; g_free() the result.
 */
SGIHeadChoice *sgi_model_resolve_heads(const char *machine,
                                       const SGIModelTopology *t,
                                       const QDict *heads, size_t *count,
                                       Error **errp);

/* Whether @choice holds a head rather than an empty position. */
bool sgi_model_head_fitted(const SGIHeadChoice *choice);

#endif /* HW_MIPS_SGI_SGI_MACHINES_H */
