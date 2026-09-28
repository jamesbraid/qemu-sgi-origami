/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The network port: a QOM interface for anything a network cable can join to
 * a peer: the Hub's one network interface port, and each of a Router's six
 * numbered ports. This interface carries the vector-routing protocol those
 * two devices use to reach a register over the network.
 *
 * index selects among the several ports one device may answer for: the Hub
 * has one network port and always answers index 0; a Router has six, numbered
 * 1 to 6 as sgi-router.c already does (a Router's arrival-port numbering), so
 * index is that same port number there.
 *
 * A cable object (sgi-net-cable) is what actually joins two ports; this
 * interface carries no "connect" method of its own, matching the crosstalk
 * port interface's reasoning (hw/mips/sgi-xtalk-port.h): the machine wires
 * each device's cable-reference properties directly, and a cable resolves
 * "the other end" for whichever of its two ends calls into it.
 */

#ifndef HW_MIPS_SGI_NET_PORT_H
#define HW_MIPS_SGI_NET_PORT_H

#include "qom/object.h"
#include "exec/hwaddr.h"

#define TYPE_SGI_NET_PORT "sgi-net-port"
typedef struct SGINetPortClass SGINetPortClass;
DECLARE_CLASS_CHECKERS(SGINetPortClass, SGI_NET_PORT, TYPE_SGI_NET_PORT)

/*
 * A vector holds at most fifteen hops (the sixteen-nybble register, one
 * hop consumed per nybble, with the terminating nybble zero). Shared by
 * live packet forwarding and offline route derivation, both bounded the
 * * same way by the same hardware fact.
 */
#define SGI_NET_PORT_MAX_HOPS 15

typedef enum SGINetPacketOp {
    SGI_NET_PACKET_READ,
    SGI_NET_PACKET_WRITE,
    SGI_NET_PACKET_EXCHANGE,
    /* A type the protocol does not define: delivered, and refused. */
    SGI_NET_PACKET_INVALID,
} SGINetPacketOp;

typedef enum SGINetPacketResult {
    /*
     * Nothing answered: no device at the end of the route, an unsupported
     * * or malformed path, or a link down or disabled somewhere along it.
     */
    SGI_NET_PACKET_NONE,
    /* A device answered and refused the access. */
    SGI_NET_PACKET_ERROR,
    /* A device answered and performed the access. */
    SGI_NET_PACKET_OK,
} SGINetPacketResult;

/*
 * A vector packet in flight. The launching Hub fills op/addr/data/vector and
 * zeroes the rest; each forwarding Router hop consumes one nybble of vector
 * and appends its arrival port to reply_vector (a Hub never forwards, so it
 * never appends). The terminal device fills value (a read or exchange) and
 * source.
 *
 * Reply-error charging (the data-error diagnostic bits) does not need a
 * recorded path: a Router that observes its sends_data_error bit set for the
 * port a packet arrived on charges the check error directly against whoever
 * sent it, the peer at the other end of that same arrival port's cable,
 * which the receiving Router already looks up locally at every hop, and the
 * launching Hub's write-time charge covers the first hop the same way. Every
 * intermediate and terminal Router hop is charged this way as the packet
 * unwinds back out through the same call chain it came in on, which visits
 * exactly the Router hops a recorded path would have, in the same order.
 */
typedef struct SGINetPacket {
    SGINetPacketOp op;
    /* The register address the request carries (bits 19:3). */
    hwaddr addr;
    /* Write or exchange data in; read or exchange data out. */
    uint64_t data;
    uint64_t value;
    /* The programmed node number of the device that answered. */
    uint32_t source;
    /* The remaining outbound route, low nybble consumed next. */
    uint64_t vector;
    /*
     * Reply Router output ports, low nibble first; zero with no reply or no
     * Routers. This is transport metadata, not a Hub register latch.
     */
    uint64_t reply_vector;
    /* Hops forwarded so far, capped at SGI_NET_PORT_MAX_HOPS. */
    unsigned int hops;
} SGINetPacket;

typedef struct SGINetPortLinkState {
    /* Whether this port itself is administratively enabled. */
    bool enabled;
    /* Whether this port itself is forcing eight-bit mode. */
    bool eight_bit;
} SGINetPortLinkState;

struct SGINetPortClass {
    InterfaceClass parent_class;

    /*
     * Text naming this port for error messages, e.g. "node 0's Hub network
     * port" or "Router router-1 port 5". index selects among the several
     * ports one device may answer for; a port that is its device's only one
     * ignores it.
     */
    const char *(*describe)(Object *port, unsigned int index);

    /*
     * Whether this port forwards a nonzero vector onward by its routing
     * tables (true for a Router's ports) rather than only ever terminating an
     * access (a Hub's port, always false).
     */
    bool (*forwards)(Object *port, unsigned int index);

    /*
     * What the peer across this port's cable should see: this port's
     * * own administrative enable and forced-width bits.
     */
    SGINetPortLinkState (*link_state)(Object *port, unsigned int index);

    /*
     * The peer's routing table, protection or diagnostic state, programmed
     * number, or enable changed, or the cable went down or up. Re-derive
     * whatever this port's owner caches (a Hub's node-number map and
     * link-up bit; a Router's per-port working and eight-bit status bits)
     * and, if this device itself forwards,
     * tell its other ports' peers in turn.
     */
    void (*peer_changed)(Object *port, unsigned int index);

    /*
     * A vector packet arrived on this port. A Router consumes one nybble of a
     * nonzero packet->vector through its tables and forwards out the exit
     * port's cable; a zero packet->vector, on a Router or a Hub, is answered
     * directly. NONE and ERROR are both a refusal, but distinct guest-visible
     * outcomes (see SGINetPacketResult): a terminal Hub that declines its
     * register decode answers with an address error (ERROR); an unsupported
     * path, a dead end, or a terminal Router's declined access latches
     * nothing, as an unanswered poll would (NONE). errp is for a genuine
     * programming fault, not a protocol-level refusal.
     */
    SGINetPacketResult (*receive)(Object *port, unsigned int index,
                                  SGINetPacket *packet, Error **errp);

    /*
     * This port observed a reply-path data error and is charging it against
     * whoever sent to it (a Router's per-port diagnostic bit, checked by the
     * sender during a read's backward path walk).
     */
    void (*record_check_error)(Object *port, unsigned int index);
};

const char *sgi_net_port_describe(Object *port, unsigned int index);
bool sgi_net_port_forwards(Object *port, unsigned int index);
SGINetPortLinkState sgi_net_port_link_state(Object *port, unsigned int index);
void sgi_net_port_peer_changed(Object *port, unsigned int index);
SGINetPacketResult sgi_net_port_receive(Object *port, unsigned int index,
                                        SGINetPacket *packet, Error **errp);
void sgi_net_port_record_check_error(Object *port, unsigned int index);

/*
 * Append one hop's arrival port to the packet's reply vector, low nybble
 * first. A forwarding Router calls this for itself, once per hop it is
 * visited at, consuming the head vector field and appending the port it
 * entered on. Returns false, unmodified, once the reply vector would overflow
 * sixty-four bits.
 */
bool sgi_net_packet_append_reply(SGINetPacket *packet, unsigned int index);

/*
 * A monotonic generation counter for the peer_changed flood: a device that
 * forwards (a Router) stamps its last-seen generation and skips a
 * peer_changed it has already processed this flood, so a cyclic topology's
 * notification converges instead of recursing forever. Call
 * sgi_net_port_flood_start() once, at the point of a genuine local change,
 * before recomputing and notifying; a peer_changed implementation that
 * propagates further reads the same flood's generation back with
 * sgi_net_port_flood_current().
 */
uint64_t sgi_net_port_flood_start(void);
uint64_t sgi_net_port_flood_current(void);

#endif /* HW_MIPS_SGI_NET_PORT_H */
