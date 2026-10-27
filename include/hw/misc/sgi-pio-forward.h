/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * One forwarding hop of an SGI programmed-I/O access.
 *
 * The Hub hands each processor I/O access on into the processor's physical
 * view, and the Bridge hands each window access on into its PCI space. Both
 * used address_space_read() and address_space_write(), which translate the
 * address through the target space's flat view on every access. A graphics
 * board's framebuffer takes millions of word stores a second this way, and
 * the translation was a large share of their cost.
 *
 * SGIPioForward remembers the region one page of the target space resolved
 * to, and dispatches later accesses in that page straight to the region's
 * handlers: the same handlers, in the same order, as address_space_write()
 * would reach. It forgets the page on every memory topology commit, so a
 * remapped window, base address register or alias is looked up again. An
 * access it cannot serve that way (RAM, an IOMMU, a page the topology
 * splits, an access the region would split, or no BQL) goes through
 * address_space_read() or address_space_write() as before.
 */

#ifndef HW_MISC_SGI_PIO_FORWARD_H
#define HW_MISC_SGI_PIO_FORWARD_H

#include "system/memory.h"

typedef struct SGIPioForward {
    AddressSpace *as;
    MemoryListener listener;
    /* The listener's name: a caller may pass one it frees after init. */
    char *name;
    /* The cached page's region, or NULL when nothing is cached. */
    MemoryRegion *mr;
    hwaddr page;
    hwaddr offset;
} SGIPioForward;

void sgi_pio_forward_init(SGIPioForward *f, AddressSpace *as,
                          const char *name);
void sgi_pio_forward_destroy(SGIPioForward *f);
MemTxResult sgi_pio_forward_read(SGIPioForward *f, hwaddr addr,
                                 MemTxAttrs attrs, void *buf, unsigned len);
MemTxResult sgi_pio_forward_write(SGIPioForward *f, hwaddr addr,
                                  MemTxAttrs attrs, const void *buf,
                                  unsigned len);

#endif
