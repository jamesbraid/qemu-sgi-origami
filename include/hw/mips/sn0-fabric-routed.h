/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SN0_FABRIC_ROUTED_H
#define HW_MIPS_SN0_FABRIC_ROUTED_H

#include "hw/mips/sn0-fabric.h"

#define TYPE_SN0_FABRIC_ROUTED "sn0-fabric-routed"

typedef struct SGIRouterState SGIRouterState;

typedef enum SN0FabricVertexKind {
    SN0_FABRIC_VERTEX_HUB,
    SN0_FABRIC_VERTEX_ROUTER,
} SN0FabricVertexKind;

typedef struct SN0FabricVertex {
    SN0FabricVertexKind kind;
    unsigned int index;
    unsigned int port;
} SN0FabricVertex;

unsigned int sn0_fabric_routed_add_router(SN0FabricState *f,
                                         SGIRouterState *router);
void sn0_fabric_routed_add_link(SN0FabricState *f, const char *name,
                               SN0FabricVertex a, SN0FabricVertex b);

#endif
