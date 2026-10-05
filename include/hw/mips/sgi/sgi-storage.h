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

/*
 * The same as a property of every instance of class @oc, so that
 * "-M <machine>,help" lists it; the bindings table sits @offset bytes into
 * each instance.
 */
void sgi_storage_class_add_property(ObjectClass *oc, size_t offset,
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

/*
 * The boot-PROM rule every SGI machine applies, given whether -bios was
 * passed (@bios) and how many of the machine's @flashes boot flash parts
 * have a store bound (@bound). -bios fills each boot flash with nothing
 * bound. With every one bound it has nothing to fill and is refused, so
 * a launcher passing a newer -bios to a machine created earlier is told
 * so instead of booting the stored PROM. Without -bios every boot flash
 * must be bound: an unbound one would hold no PROM, which only
 * @allow_blank permits.
 */
bool sgi_storage_check_boot_prom(const char *machine, bool bios,
                                 unsigned int bound, unsigned int flashes,
                                 bool allow_blank, Error **errp);

#endif /* HW_MIPS_SGI_SGI_STORAGE_H */
