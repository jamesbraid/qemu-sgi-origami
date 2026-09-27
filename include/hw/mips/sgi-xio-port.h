/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The XIO port: a QOM interface for anything an XIO link can join
 * to a peer: the Hub's I/O port, an XIO board's widget, and (from the
 * Crossbow onward) one of a Crossbow's eight external ports.
 *
 * A port answers with two regions rather than one. cpu_face() is where a
 * peer's processor-addressed traffic (a small/big window access, or a
 * Crossbow's routed XIO access) lands to reach this port.
 * packet_face() is where a peer's packet-addressed traffic (an interrupt, or
 * a transfer whose destination travels in the address rather than in routing
 * state) lands instead. A leaf endpoint that answers both requests the same
 * way (a widget's registers, which nothing packet-addresses separately) may
 * return the same region from both; a container endpoint (the Hub, or a
 * Crossbow port) answers each through its view, because the Hub resolves the
 * two kinds of traffic through different internal claim state.
 *
 * A link object is what actually joins two ports: it reads both faces and
 * installs them, in whatever way that link kind needs. This interface carries
 * no "connect" method; every implementer already has a richer, typed way to
 * accept a peer (sgi_xbow_attach_endpoint() takes two faces and a widget
 * number; an XIO board's "fabric"/"fabric-base"/"fabric-node-shift"/
 * "fabric-widget-shift" properties carry three more values a single region
 * cannot). A generic connect() would have had exactly one meaningful
 * implementer among the three this experiment builds.
 */

#ifndef HW_MIPS_SGI_XIO_PORT_H
#define HW_MIPS_SGI_XIO_PORT_H

#include "qom/object.h"
#include "system/memory.h"

#define TYPE_SGI_XIO_PORT "sgi-xio-port"
typedef struct SGIXioPortClass SGIXioPortClass;
DECLARE_CLASS_CHECKERS(SGIXioPortClass, SGI_XIO_PORT, TYPE_SGI_XIO_PORT)

struct SGIXioPortClass {
    InterfaceClass parent_class;

    /*
     * Text naming this port for error messages, e.g. "node 0's Hub I/O port"
     * or "Crossbow widget 11". index selects among the several ports one
     * device may answer for (a Crossbow's eight); a port that is its device's
     * only one ignores it.
     */
    const char *(*describe)(Object *port, unsigned int index);

    /*
     * NULL if this port offers no such face (unpopulated, or asked before the
     * state the face is built from exists).
     */
    MemoryRegion *(*cpu_face)(Object *port, unsigned int index);
    MemoryRegion *(*packet_face)(Object *port, unsigned int index);
};

const char *sgi_xio_port_describe(Object *port, unsigned int index);
MemoryRegion *sgi_xio_port_cpu_face(Object *port, unsigned int index);
MemoryRegion *sgi_xio_port_packet_face(Object *port, unsigned int index);

#endif /* HW_MIPS_SGI_XIO_PORT_H */
