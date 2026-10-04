/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Writable stores an SGI machine binds through machine properties. Each
 * property is named exactly as the catalogue's storage item (node0-flash,
 * nvram0, nvram0-clock, ...) and holds the block node or drive id that
 * backs it. The property stands for the backing device's own "drive"
 * property, as pc_sysfw.c's pflash0 does, but the machine creates those
 * devices itself, so the name is held here until it does.
 */

#ifndef HW_MIPS_SGI_SGI_STORAGE_H
#define HW_MIPS_SGI_SGI_STORAGE_H

#include "hw/mips/sgi/sgi-machines.h"

/*
 * Add the string property @name to machine @obj, recording its value in
 * *@bindings (created on first use). Setting it once the machine is
 * built is refused.
 */
void sgi_storage_add_property(Object *obj, GHashTable **bindings,
                              const char *name);

/* The block node or drive bound to item @name, or NULL. */
const char *sgi_storage_node(GHashTable *bindings, const char *name);

/*
 * Check every binding against @product's offering for @t before anything
 * is built: the offering must list the item, the node must exist, back
 * no other item and be the item's size. A clock record need only hold its
 * record, since the model keeps it in a small file the block layer rounds
 * up to a whole sector. @machine prefixes each refusal.
 */
bool sgi_storage_check(GHashTable *bindings, const char *machine,
                       const char *product, const SGIModelTopology *t,
                       Error **errp);

#endif /* HW_MIPS_SGI_SGI_STORAGE_H */
