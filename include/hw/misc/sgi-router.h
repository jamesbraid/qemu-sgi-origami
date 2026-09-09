/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_ROUTER_H
#define HW_MISC_SGI_ROUTER_H

#include "exec/hwaddr.h"
#include "hw/mips/sn0-fabric.h"
#include "qom/object.h"

#define TYPE_SGI_ROUTER "sgi-router"
OBJECT_DECLARE_SIMPLE_TYPE(SGIRouterState, SGI_ROUTER)

#define SGI_ROUTER_PORTS 6

/* The fabric-facing register target. The arrival port is numbered 1 to 6. */
bool sgi_router_access(SGIRouterState *s, unsigned int arrival_port,
                       SN0FabricVectorAccess *access);

/* Diagnostic controls for the fabric, with ports numbered 1 to 6. */
bool sgi_router_port_eight_bit_forced(SGIRouterState *s, unsigned int port);

bool sgi_router_port_enabled(SGIRouterState *s, unsigned int port);

/*
 * Replace the optional host cache observer without calling it. Notifications
 * follow table, protection and diagnostic writes, reset exit and valid
 * postload.
 * The observer and context are not migrated. It may publish link state using
 * sgi_router_set_port_link(), which does not notify the observer.
 */
void sgi_router_set_changed(SGIRouterState *s, void (*changed)(void *),
                            void *opaque);

/* Link and error state reported by the register target. */
void sgi_router_set_port_link(SGIRouterState *s, unsigned int port,
                              bool working, bool eight_bit);
void sgi_router_latch_local_error(SGIRouterState *s, uint64_t errors);
void sgi_router_latch_port_error(SGIRouterState *s, unsigned int port,
                                 uint64_t errors);

/*
 * Resolve carried turns 0 to 5 and look up the outgoing next-turn nibble.
 * The fabric checks physical links and endpoint identity. It consumes next_turn
 * only at a subsequent Router; a terminal Hub does not use that field.
 */
bool sgi_router_resolve_turn(unsigned int entry_port, uint8_t turn,
                             unsigned int *exit_port);
bool sgi_router_route(SGIRouterState *s, unsigned int entry_port,
                      uint8_t turn, uint16_t destination,
                      unsigned int *exit_port, uint8_t *next_turn);

#endif /* HW_MISC_SGI_ROUTER_H */
