/* SPDX-License-Identifier: BSD-3-Clause */
/* The XIO port interface; see the header for the contract. */

#include "qemu/osdep.h"
#include "hw/mips/sgi-xio-port.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/log.h"

MemTxResult sgi_xio_port_packet_rw(Object *port, unsigned int index,
                                   hwaddr address, void *buffer, size_t size,
                                   bool write, const SGIXioSource *source)
{
    SGIXioPortClass *pc = SGI_XIO_PORT_GET_CLASS(port);

    if (!source || !source->widget_known || source->widget > 15 ||
        (source->device_known && source->device > 7) || !pc->packet_rw) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xio: explicit packet source or receiver unavailable\n");
        return MEMTX_ACCESS_ERROR;
    }
    uint64_t limit = SGI_XIO_PACKET_SIZE *
        (sgi_xio_port_switched(port, index) ? SGI_XIO_WIDGETS : 1);

    if (address >= limit || size > limit - address) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xio: explicit packet address outside receiver\n");
        return MEMTX_DECODE_ERROR;
    }
    return pc->packet_rw(port, index, address, buffer, size, write, source);
}

MemTxResult sgi_xio_peer_packet_rw(const SGIXioPeer *peer, hwaddr address,
                                   void *buffer, size_t size, bool write,
                                   const SGIXioSource *source)
{
    if (!peer->port) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xio: explicit packet reaches no peer\n");
        return MEMTX_DECODE_ERROR;
    }
    if (address >= SGI_XIO_WIDGETS * SGI_XIO_PACKET_SIZE ||
        size > SGI_XIO_WIDGETS * SGI_XIO_PACKET_SIZE - address) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xio: explicit packet address outside fabric\n");
        return MEMTX_DECODE_ERROR;
    }
    if (!peer->switched) {
        address &= SGI_XIO_PACKET_SIZE - 1;
    }
    return sgi_xio_port_packet_rw(peer->port, peer->index, address,
                                  buffer, size, write, source);
}

const char *sgi_xio_port_describe(Object *port, unsigned int index)
{
    SGIXioPortClass *pc = SGI_XIO_PORT_GET_CLASS(port);

    return pc->describe ? pc->describe(port, index) : "an XIO port";
}

MemoryRegion *sgi_xio_port_cpu_face(Object *port, unsigned int index)
{
    SGIXioPortClass *pc = SGI_XIO_PORT_GET_CLASS(port);

    return pc->cpu_face ? pc->cpu_face(port, index) : NULL;
}

MemoryRegion *sgi_xio_port_packet_face(Object *port, unsigned int index)
{
    SGIXioPortClass *pc = SGI_XIO_PORT_GET_CLASS(port);

    return pc->packet_face ? pc->packet_face(port, index) : NULL;
}

bool sgi_xio_port_switched(Object *port, unsigned int index)
{
    SGIXioPortClass *pc = SGI_XIO_PORT_GET_CLASS(port);

    return pc->switched && pc->switched(port, index);
}

bool sgi_xio_port_connect(Object *port, unsigned int index,
                          const SGIXioPeer *peer, Error **errp)
{
    SGIXioPortClass *pc = SGI_XIO_PORT_GET_CLASS(port);

    return !pc->connect || pc->connect(port, index, peer, errp);
}

qemu_irq sgi_xio_port_reset_input(Object *port, unsigned int index)
{
    SGIXioPortClass *pc = SGI_XIO_PORT_GET_CLASS(port);

    return pc->reset_input ? pc->reset_input(port, index) : NULL;
}

bool sgi_xio_port_reset_output(Object *port, unsigned int index,
                               qemu_irq target)
{
    SGIXioPortClass *pc = SGI_XIO_PORT_GET_CLASS(port);

    if (!pc->reset_output) {
        return false;
    }
    pc->reset_output(port, index, target);
    return true;
}

void sgi_xio_port_request_dropped(Object *port, unsigned int index,
                                  hwaddr addr, bool write)
{
    SGIXioPortClass *pc;

    if (!port) {
        return;
    }
    pc = SGI_XIO_PORT_GET_CLASS(port);
    if (pc->request_dropped) {
        pc->request_dropped(port, index, addr, write);
    }
}

static const TypeInfo sgi_xio_port_types[] = {
    {
        .name = TYPE_SGI_XIO_PORT,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(SGIXioPortClass),
    },
};

DEFINE_TYPES(sgi_xio_port_types)
