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
    void (*place_claims)(struct SN1FabricEndpoint *endpoint);
} SN1FabricEndpoint;

struct SN1FabricClass {
    InterfaceClass parent_class;
    SN1FabricEndpoint *(*endpoint)(Object *owner);
};

#define SN1_FABRIC_SPACES 4
#define SN1_FABRIC_CLAIMS 2
#define SN1_FABRIC_ADDRESS_SHIFT 33
#define SN1_FABRIC_WINDOW_SIZE (UINT64_C(1) << SN1_FABRIC_ADDRESS_SHIFT)
#define SN1_FABRIC_SPACE_SIZE UINT64_C(0x10000000000)

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
