/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SGI_SN1_FABRIC_H
#define HW_MIPS_SGI_SN1_FABRIC_H

#include "qom/object.h"
#include "system/memory.h"
#include "hw/misc/sgi-bedrock.h"

typedef struct SN1FabricClass SN1FabricClass;

#define TYPE_SGI_SN1_FABRIC "sgi-sn1-fabric"
DECLARE_CLASS_CHECKERS(SN1FabricClass, SGI_SN1_FABRIC, TYPE_SGI_SN1_FABRIC)

/* Derived routing state; the owner migrates its Bedrock and mappings. */
typedef struct SN1FabricEndpoint {
    Object *owner;
    SGIBedrockState *bedrock;
    uint32_t placement;
    bool ready;
    bool isolated_route;
    MemTxResult (*processor_rw)(struct SN1FabricEndpoint *endpoint,
        unsigned origin, hwaddr address, void *buffer, size_t size, bool write,
        MemTxAttrs attrs);
    MemTxResult (*destination_rw)(struct SN1FabricEndpoint *endpoint,
        const SGIXioSource *source, hwaddr address, void *buffer, size_t size,
        bool write);
    void (*place_claims)(struct SN1FabricEndpoint *endpoint);
} SN1FabricEndpoint;

typedef struct SN1FabricProcessorView {
    SN1FabricEndpoint *endpoint;
    MemoryRegion region;
    unsigned origin;
    unsigned space;
} SN1FabricProcessorView;

void sn1_fabric_init_processor_view(SN1FabricProcessorView *view,
    SN1FabricEndpoint *endpoint, unsigned origin, unsigned space,
    MemoryRegion *container);

struct SN1FabricClass {
    InterfaceClass parent_class;
    SN1FabricEndpoint *(*endpoint)(Object *owner);
};

#define SN1_FABRIC_SPACES 4
#define SN1_FABRIC_CLAIMS 2
#define SN1_FABRIC_ADDRESS_SHIFT 33
#define SN1_FABRIC_WINDOW_SIZE (UINT64_C(1) << SN1_FABRIC_ADDRESS_SHIFT)
#define SN1_FABRIC_SPACE_SIZE UINT64_C(0x10000000000)

SN1FabricEndpoint *sn1_fabric_resolve_named(SN1FabricEndpoint *endpoint,
                                           unsigned number);
MemTxResult sn1_fabric_processor_rw(SN1FabricEndpoint *endpoint,
    unsigned origin, hwaddr address, void *buffer, size_t size, bool write,
    MemTxAttrs attrs);
MemTxResult sn1_fabric_destination_rw(void *opaque, const SGIXioSource *source,
    hwaddr address, void *buffer, size_t size, bool write);
SN1FabricEndpoint *sn1_fabric_peer(SN1FabricEndpoint *endpoint);
unsigned int sn1_fabric_number(SN1FabricEndpoint *endpoint);
SN1FabricEndpoint *sn1_fabric_claimant(SN1FabricEndpoint *endpoint,
                                      unsigned int number);
void sn1_fabric_update_claims(SN1FabricEndpoint *endpoint);
void sn1_fabric_place_view(SN1FabricEndpoint *endpoint,
                          MemoryRegion claim[SN1_FABRIC_SPACES]
                                            [SN1_FABRIC_CLAIMS]);
void sn1_fabric_init_floor(SN1FabricEndpoint *endpoint,
                          MemoryRegion floor[SN1_FABRIC_SPACES]);
void sn1_fabric_init_view(SN1FabricEndpoint *endpoint, unsigned int view,
                         MemoryRegion *container,
                         MemoryRegion floor[SN1_FABRIC_SPACES],
                         MemoryRegion floor_alias[SN1_FABRIC_SPACES],
                         MemoryRegion claim[SN1_FABRIC_SPACES]
                                           [SN1_FABRIC_CLAIMS]);

#endif
