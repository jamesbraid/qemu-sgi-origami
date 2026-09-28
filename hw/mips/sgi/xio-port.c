/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The XIO port interface; see the header for the contract. */

#include "qemu/osdep.h"
#include "hw/mips/sgi-xio-port.h"
#include "qapi/error.h"
#include "qemu/module.h"

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

static const TypeInfo sgi_xio_port_types[] = {
    {
        .name = TYPE_SGI_XIO_PORT,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(SGIXioPortClass),
    },
};

DEFINE_TYPES(sgi_xio_port_types)
