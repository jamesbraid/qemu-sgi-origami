/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Byte placement on the DMA path of an SGI XIO-to-PCI bridge.
 *
 * Each target is a callback region taking up to eight bytes in one call, so
 * a doubleword step costs one callback and one onward transfer whatever the
 * placement. The memory core hands it naturally aligned accesses, so the
 * host bytes of one access are always one contiguous run.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "hw/pci/pci-64bit.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci-host/sgi-dma-placement.h"
#include "trace.h"

static const unsigned sgi_dma_placement_unit[SGI_DMA_PLACEMENTS] = {
    [SGI_DMA_PLACEMENT_IDENTITY] = 1,
    [SGI_DMA_PLACEMENT_WORD] = 4,
    [SGI_DMA_PLACEMENT_DOUBLEWORD] = 8,
};

/*
 * pci holds the bytes in PCI address order. A unit wider than the access
 * mirrors the access within the unit; a narrower one reverses each unit of
 * the access in place.
 */
static MemTxResult sgi_dma_placement_access(SGIDMAPlacementTarget *t,
                                            hwaddr addr, uint8_t *pci,
                                            unsigned size, bool is_write,
                                            MemTxAttrs attrs)
{
    SGIDMAPlacementSet *set = t->set;
    unsigned lane = MIN(size, t->unit) - 1;
    hwaddr host = size < t->unit ? addr ^ (t->unit - size) : addr;
    uint8_t bytes[8] = { 0 };
    MemTxResult result;
    unsigned i;

    trace_sgi_dma_placement_access(memory_region_name(&t->mr),
                                   is_write ? "write" : "read", addr, host,
                                   size);
    if (is_write) {
        for (i = 0; i < size; i++) {
            bytes[i ^ lane] = pci[i];
        }
    }
    if (set->send) {
        result = set->send(set->opaque, host, bytes, size, is_write, attrs);
    } else {
        result = address_space_rw(set->onward, host, attrs, bytes, size,
                                  is_write);
    }
    if (!is_write) {
        for (i = 0; i < size; i++) {
            pci[i] = bytes[i ^ lane];
        }
    }
    return result;
}

static MemTxResult sgi_dma_placement_read(void *opaque, hwaddr addr,
                                          uint64_t *value, unsigned size,
                                          MemTxAttrs attrs)
{
    uint8_t pci[8];
    MemTxResult result;

    result = sgi_dma_placement_access(opaque, addr, pci, size, false, attrs);
    *value = ldn_le_p(pci, size);
    return result;
}

static MemTxResult sgi_dma_placement_write(void *opaque, hwaddr addr,
                                           uint64_t value, unsigned size,
                                           MemTxAttrs attrs)
{
    uint8_t pci[8];

    stn_le_p(pci, size, value);
    return sgi_dma_placement_access(opaque, addr, pci, size, true, attrs);
}

/* Little-endian, so a value's bytes are the PCI bytes in address order. */
static const MemoryRegionOps sgi_dma_placement_ops = {
    .read_with_attrs = sgi_dma_placement_read,
    .write_with_attrs = sgi_dma_placement_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void sgi_dma_placement_init_target(SGIDMAPlacementSet *set,
                                          SGIDMAPlacement placement,
                                          Object *owner, const char *name,
                                          uint64_t size)
{
    static const char *const suffix[SGI_DMA_PLACEMENTS] = {
        [SGI_DMA_PLACEMENT_IDENTITY] = "identity",
        [SGI_DMA_PLACEMENT_WORD] = "word",
        [SGI_DMA_PLACEMENT_DOUBLEWORD] = "doubleword",
    };
    SGIDMAPlacementTarget *t = &set->target[placement];
    g_autofree char *target_name = g_strdup_printf("%s.%s", name,
                                                   suffix[placement]);

    t->set = set;
    t->unit = sgi_dma_placement_unit[placement];
    memory_region_init_io(&t->mr, owner, &sgi_dma_placement_ops, t,
                          target_name, size);
    /*
     * The guard is per owning device, and the bridge owns both this region
     * and the requester's route into it, so an ordinary transfer would trip
     * it. Regions reached onward keep their own guards.
     */
    t->mr.disable_reentrancy_guard = true;
    address_space_init(&t->as, &t->mr, target_name);
}

static void sgi_dma_placement_init_targets(SGIDMAPlacementSet *set,
                                           Object *owner, const char *name,
                                           uint64_t size, unsigned needed)
{
    SGIDMAPlacement placement;

    for (placement = 0; placement < SGI_DMA_PLACEMENTS; placement++) {
        if (needed & BIT(placement)) {
            sgi_dma_placement_init_target(set, placement, owner, name, size);
        }
    }
}

void sgi_dma_placement_init(SGIDMAPlacementSet *set, Object *owner,
                            const char *name, AddressSpace *onward,
                            unsigned needed)
{
    memset(set, 0, sizeof(*set));
    set->onward = onward;
    sgi_dma_placement_init_targets(set, owner, name, UINT64_MAX,
                                   needed & ~BIT(SGI_DMA_PLACEMENT_IDENTITY));
}

void sgi_dma_placement_init_send(SGIDMAPlacementSet *set, Object *owner,
                                 const char *name, uint64_t size,
                                 unsigned needed, SGIDMAPlacementSend *send,
                                 void *opaque)
{
    memset(set, 0, sizeof(*set));
    set->send = send;
    set->opaque = opaque;
    sgi_dma_placement_init_targets(set, owner, name, size, needed);
}

void sgi_dma_placement_destroy(SGIDMAPlacementSet *set)
{
    SGIDMAPlacement placement;

    for (placement = 0; placement < SGI_DMA_PLACEMENTS; placement++) {
        if (set->target[placement].set) {
            address_space_destroy(&set->target[placement].as);
        }
    }
}

AddressSpace *sgi_dma_placement_as(SGIDMAPlacementSet *set,
                                   SGIDMAPlacement placement)
{
    assert(placement < SGI_DMA_PLACEMENTS);
    if (!set->target[placement].set) {
        /* Only the address-space identity is unbuilt by design. */
        assert(placement == SGI_DMA_PLACEMENT_IDENTITY && set->onward);
        return set->onward;
    }
    return &set->target[placement].as;
}

uint8_t sgi_dma_placement_reset_widths(PCIBus *bus, uint8_t req64_slots)
{
    uint8_t present = 0, narrow = 0;
    unsigned devfn;

    for (devfn = 0; devfn < PCI_DEVFN(8, 0); devfn++) {
        PCIDevice *dev = bus->devices[devfn];
        unsigned slot = PCI_SLOT(devfn);

        if (!dev) {
            continue;
        }
        present |= BIT(slot);
        if (!pci_64bit_device_reset_done(dev, req64_slots & BIT(slot))) {
            narrow |= BIT(slot);
        }
    }
    return present & ~narrow;
}
