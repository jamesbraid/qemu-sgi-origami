/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Byte placement on the DMA path of an SGI XIO-to-PCI bridge.
 *
 * Bridge and XBridge place a DMA's bytes by the window's swap control and
 * the width of the PCI data phase: a swap-set transfer is a byte stream; a
 * swap-clear one reverses each aligned 32-bit word on a 32-bit data phase
 * and each aligned doubleword on a 64-bit one (Bridge specification
 * Appendix A, applied to DMA by its Device(x) register description). A
 * bridge's IOMMU translate picks one of the three targets here; each
 * forwards to the bridge's onward address space, or through a send
 * function where the onward leg is an explicit packet.
 */
#ifndef HW_PCI_HOST_SGI_DMA_PLACEMENT_H
#define HW_PCI_HOST_SGI_DMA_PLACEMENT_H

#include "qemu/bitops.h"
#include "system/memory.h"

typedef enum SGIDMAPlacement {
    SGI_DMA_PLACEMENT_IDENTITY,
    SGI_DMA_PLACEMENT_WORD,         /* PCI byte k is host byte k ^ 3 */
    SGI_DMA_PLACEMENT_DOUBLEWORD,   /* PCI byte k is host byte 7 - k */
    SGI_DMA_PLACEMENT_HALF_WRITE,   /* synthetic XBridge direct write */
    SGI_DMA_PLACEMENT_DOUBLEWORD_WORD_RAM,
    SGI_DMA_PLACEMENTS,
} SGIDMAPlacement;

/* Moves host-ordered bytes onward; for a target with no address space. */
typedef MemTxResult SGIDMAPlacementSend(void *opaque, hwaddr addr, void *buf,
                                        unsigned len, bool is_write,
                                        MemTxAttrs attrs,
                                        SGIDMAPlacement placement);

typedef struct SGIDMAPlacementSet SGIDMAPlacementSet;

typedef struct SGIDMAPlacementTarget {
    SGIDMAPlacementSet *set;
    SGIDMAPlacement placement;
    unsigned unit;
    MemoryRegion mr;
    AddressSpace as;
} SGIDMAPlacementTarget;

struct SGIDMAPlacementSet {
    AddressSpace *onward;
    SGIDMAPlacementSend *send;
    void *opaque;
    SGIDMAPlacementTarget target[SGI_DMA_PLACEMENTS];
};

/*
 * Targets over an onward address space. The identity target is that space
 * itself, so a byte-stream transfer reaches RAM with no callback. needed is
 * a mask of BIT(placement): only those reversals are built, since every
 * address space costs each memory topology update.
 */
void sgi_dma_placement_init(SGIDMAPlacementSet *set, Object *owner,
                            const char *name, AddressSpace *onward,
                            unsigned needed);

/* Targets, identity among them if needed, that hand every access to send. */
void sgi_dma_placement_init_send(SGIDMAPlacementSet *set, Object *owner,
                                 const char *name, uint64_t size,
                                 unsigned needed, SGIDMAPlacementSend *send,
                                 void *opaque);

void sgi_dma_placement_destroy(SGIDMAPlacementSet *set);

AddressSpace *sgi_dma_placement_as(SGIDMAPlacementSet *set,
                                   SGIDMAPlacement placement);

/*
 * At the end of a reset of the bridge's PCI bus: tell each device on the
 * first eight slots whether its slot drives REQ64# (req64_slots, one bit
 * per slot) and return the slots whose DMA runs 64-bit data phases. A slot
 * with several functions is 64-bit only if every function is, since the
 * bridge places by slot.
 */
uint8_t sgi_dma_placement_reset_widths(PCIBus *bus, uint8_t req64_slots);

/* The targets a bridge builds for a board whose wired slots are req64. */
static inline unsigned sgi_dma_placement_needed(uint8_t req64_slots)
{
    return BIT(SGI_DMA_PLACEMENT_WORD) |
           (req64_slots ? BIT(SGI_DMA_PLACEMENT_DOUBLEWORD) : 0);
}

/* The rule both bridges share, from the swap bit and the slot's width. */
static inline SGIDMAPlacement sgi_dma_placement_select(bool byte_stream,
                                                       bool wide)
{
    if (byte_stream) {
        return SGI_DMA_PLACEMENT_IDENTITY;
    }
    return wide ? SGI_DMA_PLACEMENT_DOUBLEWORD : SGI_DMA_PLACEMENT_WORD;
}

#endif
