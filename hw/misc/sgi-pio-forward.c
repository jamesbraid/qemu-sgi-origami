/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * One forwarding hop of an SGI programmed-I/O access, with the page it last
 * resolved to remembered. See include/hw/misc/sgi-pio-forward.h.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/main-loop.h"
#include "system/memory.h"
#include "hw/misc/sgi-pio-forward.h"

#define SGI_PIO_FORWARD_PAGE    4096

static void sgi_pio_forward_forget(SGIPioForward *f)
{
    if (f->mr) {
        memory_region_unref(f->mr);
        f->mr = NULL;
    }
}

/*
 * Any commit can move what a page resolves to: a window's alias, a base
 * address register, a region enabled or removed. Commits are rare next to
 * accesses, so every one empties the cache rather than working out whether
 * it touched this space. A handler can commit while its own access is
 * being dispatched; that region then stays alive through the superseded
 * flat view, which the caller's RCU read section keeps, not through this
 * cache's reference.
 */
static void sgi_pio_forward_commit(MemoryListener *listener)
{
    sgi_pio_forward_forget(container_of(listener, SGIPioForward, listener));
}

/*
 * The region @addr's page resolves to and @addr's offset in it, or NULL
 * when the page is not one plain region of device handlers.
 */
static MemoryRegion *sgi_pio_forward_lookup(SGIPioForward *f, hwaddr addr,
                                            hwaddr *offset)
{
    hwaddr page = addr & ~(hwaddr)(SGI_PIO_FORWARD_PAGE - 1);
    MemoryRegionSection section;

    if (f->mr && f->page == page) {
        *offset = f->offset + (addr - page);
        return f->mr;
    }
    sgi_pio_forward_forget(f);

    /* memory_region_find() cannot search an address space with no mappings. */
    if (!f->as->root->terminates &&
        QTAILQ_EMPTY(&f->as->root->subregions)) {
        return NULL;
    }
    section = memory_region_find(f->as->root, page, SGI_PIO_FORWARD_PAGE);
    if (!section.mr) {
        return NULL;
    }
    if (section.offset_within_address_space != page ||
        int128_get64(section.size) != SGI_PIO_FORWARD_PAGE ||
        memory_region_is_ram(section.mr) ||
        memory_region_is_romd(section.mr) ||
        memory_region_get_iommu(section.mr)) {
        memory_region_unref(section.mr);
        return NULL;
    }
    f->mr = section.mr;
    f->page = page;
    f->offset = section.offset_within_region;
    *offset = f->offset + (addr - page);
    return f->mr;
}

/*
 * The region and offset to dispatch to, or NULL when the access must take
 * the address space's own path. address_space_write() splits an access the
 * region does not take whole; only an access it would hand over unsplit
 * comes here.
 */
static MemoryRegion *sgi_pio_forward_target(SGIPioForward *f, hwaddr addr,
                                            MemTxAttrs attrs, unsigned len,
                                            hwaddr *offset)
{
    MemoryRegion *mr;

    if (attrs.memory || !bql_locked() || len > 8 ||
        (addr % SGI_PIO_FORWARD_PAGE) + len > SGI_PIO_FORWARD_PAGE) {
        return NULL;
    }
    mr = sgi_pio_forward_lookup(f, addr, offset);
    if (!mr || memory_access_size(mr, len, *offset) != len) {
        return NULL;
    }
    return mr;
}

MemTxResult sgi_pio_forward_read(SGIPioForward *f, hwaddr addr,
                                 MemTxAttrs attrs, void *buf, unsigned len)
{
    MemoryRegion *mr;
    MemTxResult result;
    hwaddr offset;
    uint64_t val;

    mr = sgi_pio_forward_target(f, addr, attrs, len, &offset);
    if (!mr) {
        return address_space_read(f->as, addr, attrs, buf, len);
    }
    result = memory_region_dispatch_read(mr, offset, &val, size_memop(len),
                                         attrs);
    stn_he_p(buf, len, val);
    return result;
}

MemTxResult sgi_pio_forward_write(SGIPioForward *f, hwaddr addr,
                                  MemTxAttrs attrs, const void *buf,
                                  unsigned len)
{
    MemoryRegion *mr;
    hwaddr offset;

    mr = sgi_pio_forward_target(f, addr, attrs, len, &offset);
    if (!mr) {
        return address_space_write(f->as, addr, attrs, buf, len);
    }
    return memory_region_dispatch_write(mr, offset, ldn_he_p(buf, len),
                                        size_memop(len), attrs);
}

void sgi_pio_forward_init(SGIPioForward *f, AddressSpace *as,
                          const char *name)
{
    /* The lookup resolves from as->root, so it must be the outermost root. */
    assert(!as->root->container);
    *f = (SGIPioForward) {
        .as = as,
        .name = g_strdup(name),
        .listener = {
            .commit = sgi_pio_forward_commit,
        },
    };
    f->listener.name = f->name;
    memory_listener_register(&f->listener, as);
}

void sgi_pio_forward_destroy(SGIPioForward *f)
{
    if (f->as) {
        memory_listener_unregister(&f->listener);
        sgi_pio_forward_forget(f);
        f->as = NULL;
        g_free(f->name);
        f->name = NULL;
    }
}
