/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The XIO port: a QOM interface for every chip one end of an XIO link
 * attaches to -- the Hub's and Bedrock's I/O interface (II), each
 * Crossbow or XBridge crossbar port, a Bridge, and an XIO board's
 * widget.
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
#include "hw/core/irq.h"

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

/* Access-control inputs, not physical packet-header encoding. */
typedef struct SGIXioSource {
    bool widget_known;
    bool device_known;
    uint8_t widget;
    uint8_t device;
} SGIXioSource;

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

    /* Explicit-source packets never fall back to an unqualified face. */
    MemTxResult (*packet_rw)(Object *port, unsigned int index, hwaddr address,
                             void *buffer, size_t size, bool write,
                             const SGIXioSource *source);

    /* Whether this port's faces carry the destination widget. */
    bool (*switched)(Object *port, unsigned int index);

    /*
     * Take peer's faces as this port's outbound targets. A port that
     * sends nothing over its link may leave this NULL.
     */
    bool (*connect)(Object *port, unsigned int index, const SGIXioPeer *peer,
                    Error **errp);

    /*
     * A link also carries a reset. reset_input() is the line that resets
     * this port's chip when the far end sends one down the link (a
     * Bridge's core, a crossbar's whole core, a Hub's link state);
     * reset_output() wires what this port sends, a crossbar's per-link or
     * propagated reset or a Hub's warm reset, to the far end's input. A
     * port without one leaves it NULL.
     */
    qemu_irq (*reset_input)(Object *port, unsigned int index);
    void (*reset_output)(Object *port, unsigned int index, qemu_irq target);

    /*
     * A crossbar dropped a request this port sent, at XIO address addr
     * (widget in bits 51:48 on the packet path), so no response will
     * come: the sender's own request timer is what reports it. A port
     * with nothing to report leaves it NULL.
     */
    void (*request_dropped)(Object *port, unsigned int index, hwaddr addr,
                            bool write);
};

/*
 * The link layer's guard against reset loops, the same text in the
 * Crossbow (rev 12.0, printed page 133) and XBridge (rev 0.5, printed page
 * 122) specifications: "after detecting the first reset pattern, [it]
 * will ignore all subsequent reset requests for 1024 cycles". True when a
 * reset arriving at now is the first; it then opens the quiet window.
 */
#define SGI_XIO_LLP_RESET_QUIET_CYCLES 1024

static inline bool sgi_xio_llp_reset_accepted(int64_t *quiet_until,
                                              int64_t now,
                                              int64_t cycle_ns)
{
    if (now < *quiet_until) {
        return false;
    }
    *quiet_until = now + SGI_XIO_LLP_RESET_QUIET_CYCLES * cycle_ns;
    return true;
}

MemTxResult sgi_xio_port_packet_rw(Object *port, unsigned int index,
                                   hwaddr address, void *buffer, size_t size,
                                   bool write, const SGIXioSource *source);
MemTxResult sgi_xio_peer_packet_rw(const SGIXioPeer *peer, hwaddr address,
                                   void *buffer, size_t size, bool write,
                                   const SGIXioSource *source);

const char *sgi_xio_port_describe(Object *port, unsigned int index);
MemoryRegion *sgi_xio_port_cpu_face(Object *port, unsigned int index);
MemoryRegion *sgi_xio_port_packet_face(Object *port, unsigned int index);
bool sgi_xio_port_switched(Object *port, unsigned int index);
bool sgi_xio_port_connect(Object *port, unsigned int index,
                          const SGIXioPeer *peer, Error **errp);
qemu_irq sgi_xio_port_reset_input(Object *port, unsigned int index);
bool sgi_xio_port_reset_output(Object *port, unsigned int index,
                               qemu_irq target);
void sgi_xio_port_request_dropped(Object *port, unsigned int index,
                                  hwaddr addr, bool write);

#endif /* HW_MIPS_SGI_XIO_PORT_H */
