/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The SN0 fabric: the machine-owned object that stands between each Hub's
 * network interface and whatever joins the Hubs -- one CrayLink cable, a
 * Null Router, or a graph of Router devices.
 *
 * A Hub holds one endpoint on the fabric and never sees a peer, a Router,
 * a module, or a cable through it. It reports its programmed node number
 * to the fabric and receives a reachability map back: for each of the 256
 * node numbers, the endpoint currently programmed with that number if the
 * fabric connects the two, else none. The Hub aims its node-qualified
 * aliases by that map and nothing else. Vector operations walk the fabric
 * from the endpoint's port one nybble at a time and end at whatever device
 * the route reaches.
 *
 * Contract: the routed-fabric design in the Origami project tree
 * (docs/designs/2026-09-03-origin2000-routed-fabric-design.md) and the
 * address-map datasheet's programmed-node-number rule.
 */

#ifndef HW_MIPS_SN0_FABRIC_H
#define HW_MIPS_SN0_FABRIC_H

#include "qom/object.h"
#include "hw/core/qdev.h"
#include "exec/hwaddr.h"

#define TYPE_SN0_FABRIC        "sn0-fabric"
#define TYPE_SN0_FABRIC_DIRECT "sn0-fabric-direct"

OBJECT_DECLARE_TYPE(SN0FabricState, SN0FabricClass, SN0_FABRIC)

/* The node numbers an address can carry in M mode: an eight-bit field. */
#define SN0_FABRIC_NUMBERS 256

/* A map entry naming no endpoint. */
#define SN0_FABRIC_NONE (-1)

typedef struct SGIHubState SGIHubState;

typedef struct SN0FabricEndpoint {
    /* The Hub behind this endpoint. */
    SGIHubState *hub;
    /* Its board stride in the canonical map: where its regions sit. */
    uint32_t stride;
    /* The programmed node number it last reported. */
    uint32_t number;
    /* Per node number, the endpoint reachable as that number, or none. */
    int16_t map[SN0_FABRIC_NUMBERS];
} SN0FabricEndpoint;

typedef struct SN0FabricLink {
    /* The model's bus identifier, which the down-links property names. */
    char *name;
    /* The two endpoints the link joins (the direct adapter's only shape). */
    unsigned int a;
    unsigned int b;
} SN0FabricLink;

typedef enum SN0FabricVectorOp {
    SN0_FABRIC_VECTOR_READ,
    SN0_FABRIC_VECTOR_WRITE,
    SN0_FABRIC_VECTOR_EXCHANGE,
    /* A type the protocol does not define: delivered, and refused. */
    SN0_FABRIC_VECTOR_INVALID,
} SN0FabricVectorOp;

typedef enum SN0FabricVectorResult {
    /* Nothing answered: no device at the end of the route, or a link down. */
    SN0_FABRIC_VECTOR_NONE,
    /* A device answered and refused the access. */
    SN0_FABRIC_VECTOR_ERROR,
    /* A device answered and performed the access. */
    SN0_FABRIC_VECTOR_OK,
} SN0FabricVectorResult;

typedef struct SN0FabricVectorAccess {
    SN0FabricVectorOp op;
    /* The register address the request carries (bits 19:3). */
    hwaddr addr;
    /* Write or exchange data in; read or exchange data out. */
    uint64_t data;
    uint64_t value;
    /* The programmed node number of the device that answered. */
    uint32_t source;
} SN0FabricVectorAccess;

struct SN0FabricState {
    DeviceState parent_obj;

    /* Endpoints and links, in the order the machine added them. */
    GArray *endpoints;
    GArray *links;

    /* Bit n set: link n is marked down. Migrated; the maps re-derive. */
    uint64_t down_mask;

    /* Set once the machine has added everything; maps recompute after. */
    bool built;
};

struct SN0FabricClass {
    DeviceClass parent_class;

    /* Recompute every endpoint's map from the reported numbers and links. */
    void (*recompute)(SN0FabricState *f);
    /* Walk a vector from an endpoint's port and perform the access. */
    SN0FabricVectorResult (*vector)(SN0FabricState *f, unsigned int endpoint,
                                    uint64_t vector,
                                    SN0FabricVectorAccess *access);
    /* The route each ordered pair of endpoints takes, for the property. */
    char *(*routes)(SN0FabricState *f);
};

/* The machine's side: build the topology, then declare it built. */
unsigned int sn0_fabric_add_endpoint(SN0FabricState *f, SGIHubState *hub,
                                     uint32_t stride);
void sn0_fabric_add_link(SN0FabricState *f, const char *name,
                         unsigned int a, unsigned int b);
void sn0_fabric_build(SN0FabricState *f);
unsigned int sn0_fabric_endpoint_count(SN0FabricState *f);

/* The Hub's side. */
void sn0_fabric_report_number(SN0FabricState *f, unsigned int endpoint,
                              uint32_t number);
const int16_t *sn0_fabric_map(SN0FabricState *f, unsigned int endpoint);
uint32_t sn0_fabric_endpoint_stride(SN0FabricState *f, unsigned int endpoint);
uint32_t sn0_fabric_endpoint_number(SN0FabricState *f, unsigned int endpoint);
SGIHubState *sn0_fabric_hub_for_number(SN0FabricState *f,
                                       unsigned int endpoint,
                                       uint32_t number);
bool sn0_fabric_link_up(SN0FabricState *f, unsigned int endpoint);
SN0FabricVectorResult sn0_fabric_vector(SN0FabricState *f,
                                        unsigned int endpoint,
                                        uint64_t vector,
                                        SN0FabricVectorAccess *access);

#endif
