/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The XIO port: a QOM interface for every chip one end of an XIO link
 * attaches to -- the Hub's and Bedrock's I/O interface (II), each
 * Crossbow or XBridge crossbar port, a Bridge, and an XIO board's
 * widget. See docs/designs/sn-machine-model-architecture.md, "Ports and
 * links", in the Origami tree.
 *
 * A port receives through two faces, because the model keeps two
 * address forms apart:
 *
 *   - cpu_face() takes a processor's programmed I/O forwarded through a
 *     Hub's small or big memory window: an offset within this widget's
 *     space;
 *   - packet_face() takes a widget-originated request -- DMA, an
 *     interrupt write, or an I/O-space request -- as a complete 48-bit
 *     XIO address.
 *
 * A switched port (a crossbar port) receives with the destination widget
 * still in the address: cpu_face() at widget times the crossbar's span,
 * packet_face() in bits 51:48. A leaf port's faces carry no widget. A
 * face is NULL until the chip that owns it has built it (a board's, for
 * one, exists only once the board has realized), and a port that never
 * receives one of the two kinds (the Bedrock takes no programmed I/O
 * from a peer) answers NULL for it.
 *
 * connect() hands a port its link peer's faces, as the targets of this
 * port's own outbound traffic. The link (hw/mips/sgi/xio-link.c) calls
 * it once each way and owns nothing else.
 */

#ifndef HW_MIPS_SGI_XIO_PORT_H
#define HW_MIPS_SGI_XIO_PORT_H

#include "qom/object.h"
#include "system/memory.h"

#define TYPE_SGI_XIO_PORT "sgi-xio-port"
typedef struct SGIXioPortClass SGIXioPortClass;
DECLARE_CLASS_CHECKERS(SGIXioPortClass, SGI_XIO_PORT, TYPE_SGI_XIO_PORT)

/* The destination widget sits above a 48-bit XIO address in a packet. */
#define SGI_XIO_PACKET_ADDRESS_BITS 48
#define SGI_XIO_PACKET_SIZE         (1ULL << SGI_XIO_PACKET_ADDRESS_BITS)
#define SGI_XIO_WIDGETS             16

/*
 * What a port is told about the other end of its link. packet_fabric is
 * the peer's packet face as a widget-addressed space, whatever the
 * peer: the face itself for a switched peer, and for a leaf peer the
 * same face at every one of the sixteen widget numbers, since a leaf
 * accepts any destination. A sender composes (widget << 48) | address
 * into it and never needs to know which kind of peer it has.
 */
typedef struct SGIXioPeer {
    Object *port;
    unsigned int index;
    const char *description;
    MemoryRegion *cpu_face;
    MemoryRegion *packet_face;
    MemoryRegion *packet_fabric;
    bool switched;
} SGIXioPeer;

struct SGIXioPortClass {
    InterfaceClass parent_class;

    /*
     * Text naming this port for error messages, e.g. "the Hub's I/O
     * interface (II)" or "a Crossbow port". index selects among the
     * several ports one device may answer for (a Crossbow's eight); a
     * port that is its device's only one ignores it.
     */
    const char *(*describe)(Object *port, unsigned int index);

    MemoryRegion *(*cpu_face)(Object *port, unsigned int index);
    MemoryRegion *(*packet_face)(Object *port, unsigned int index);

    /* Whether this port's faces carry the destination widget. */
    bool (*switched)(Object *port, unsigned int index);

    /*
     * Take peer's faces as this port's outbound targets. A port that
     * sends nothing over its link may leave this NULL.
     */
    bool (*connect)(Object *port, unsigned int index, const SGIXioPeer *peer,
                    Error **errp);
};

const char *sgi_xio_port_describe(Object *port, unsigned int index);
MemoryRegion *sgi_xio_port_cpu_face(Object *port, unsigned int index);
MemoryRegion *sgi_xio_port_packet_face(Object *port, unsigned int index);
bool sgi_xio_port_switched(Object *port, unsigned int index);
bool sgi_xio_port_connect(Object *port, unsigned int index,
                          const SGIXioPeer *peer, Error **errp);

#endif /* HW_MIPS_SGI_XIO_PORT_H */
