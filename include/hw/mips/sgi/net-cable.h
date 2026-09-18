/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-net-cable: joins two SGI_NET_PORT ports -- the Origin 200's direct
 * Hub-to-Hub CrayLink, a module's Hub-to-Router or Router-to-Router
 * midplane traces, an Xpress Link cable between two Router-equipped
 * modules -- so a Hub or a Router never holds a peer, a topology or a
 * central object, only the cable or cables on its own ports.
 *
 * The machine names each cable from the descriptor's link record
 * (SGIModelFabricLink.name), so a test can address it at
 * "/machine/cable[name]" or by its "down" property.
 */

#ifndef HW_MIPS_SGI_NET_CABLE_H
#define HW_MIPS_SGI_NET_CABLE_H

#include "qom/object.h"
#include "hw/core/qdev.h"

#define TYPE_SGI_NET_CABLE "sgi-net-cable"
OBJECT_DECLARE_SIMPLE_TYPE(SGINetCableState, SGI_NET_CABLE)

typedef enum SGINetCableKind {
    SGI_NET_CABLE_KIND_CABLE,
    SGI_NET_CABLE_KIND_MIDPLANE,
    SGI_NET_CABLE_KIND_EXPRESS,
    SGI_NET_CABLE_KIND_DIRECT,
} SGINetCableKind;

/*
 * Connect one end before realize. Both ends must be set before the cable
 * realizes; a cable with only one end set refuses to realize.
 */
void sgi_net_cable_set_end(SGINetCableState *cable, unsigned int side,
                           Object *port, unsigned int index);

/*
 * Resolve the other end of this cable relative to (port, index), which
 * must be one of the two ends. Fails (returns false) only on a
 * programming error -- a port asking about a cable it is not on.
 */
bool sgi_net_cable_other_end(SGINetCableState *cable, Object *port,
                             unsigned int index, Object **peer_port,
                             unsigned int *peer_index);

/*
 * Whether this cable currently carries traffic: realized and not marked
 * down. Does not consult either end's own administrative enable -- a
 * caller combines this with both ends' link_state().
 */
bool sgi_net_cable_up(SGINetCableState *cable);

/*
 * The neighbor visible from (port, index) if the link is genuinely up:
 * a cable exists, is not marked down, and both this port's and the
 * peer's own SGI_NET_PORT link_state() report themselves enabled. This
 * is the one predicate every hop-by-hop walk and every live vector
 * forward uses, so a disabled port or a down cable refuses identically
 * whether it is discovered while deriving a Hub's reachability map or
 * while a packet is actually in flight.
 */
bool sgi_net_cable_neighbor(SGINetCableState *cable, Object *port,
                            unsigned int index, Object **peer_port,
                            unsigned int *peer_index);

#endif /* HW_MIPS_SGI_NET_CABLE_H */
