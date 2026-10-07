/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_SN1_ROUTER_H
#define HW_MISC_SGI_SN1_ROUTER_H

#include "hw/core/sysbus.h"
#include "hw/mips/sgi-net-port.h"

#define TYPE_SGI_SN1_ROUTER "sgi-sn1-router"
OBJECT_DECLARE_SIMPLE_TYPE(SGISN1RouterState, SGI_SN1_ROUTER)
#define SGI_SN1_ROUTER_PORTS 8

SGINetPacketResult sgi_sn1_router_access(SGISN1RouterState *s,
    unsigned ingress, SGINetPacket *packet);
bool sgi_sn1_router_route(SGISN1RouterState *s, unsigned ingress,
    uint8_t turn, uint8_t destination, unsigned *output, uint8_t *next_turn);
bool sgi_sn1_router_port_neighbor(SGISN1RouterState *s, unsigned port,
    Object **peer, unsigned *index);
void sgi_sn1_router_set_changed(SGISN1RouterState *s,
    void (*changed)(void *), void *opaque);

void sgi_sn1_router_lock_cables(SGISN1RouterState *s);
void sgi_sn1_router_enable_synthetic_diagnostics(SGISN1RouterState *s);

#endif
