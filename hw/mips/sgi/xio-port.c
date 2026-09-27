/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The XIO port interface; see the header for the contract. */

#include "qemu/osdep.h"
#include "hw/mips/sgi-xio-port.h"
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

static const TypeInfo sgi_xio_port_types[] = {
    {
        .name = TYPE_SGI_XIO_PORT,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(SGIXioPortClass),
    },
};

DEFINE_TYPES(sgi_xio_port_types)
