/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The crosstalk port interface; see the header for the contract. */

#include "qemu/osdep.h"
#include "hw/mips/sgi-xtalk-port.h"
#include "qemu/module.h"

const char *sgi_xtalk_port_describe(Object *port, unsigned int index)
{
    SGIXtalkPortClass *pc = SGI_XTALK_PORT_GET_CLASS(port);

    return pc->describe ? pc->describe(port, index) : "a crosstalk port";
}

MemoryRegion *sgi_xtalk_port_cpu_face(Object *port, unsigned int index)
{
    SGIXtalkPortClass *pc = SGI_XTALK_PORT_GET_CLASS(port);

    return pc->cpu_face ? pc->cpu_face(port, index) : NULL;
}

MemoryRegion *sgi_xtalk_port_packet_face(Object *port, unsigned int index)
{
    SGIXtalkPortClass *pc = SGI_XTALK_PORT_GET_CLASS(port);

    return pc->packet_face ? pc->packet_face(port, index) : NULL;
}

static const TypeInfo sgi_xtalk_port_types[] = {
    {
        .name = TYPE_SGI_XTALK_PORT,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(SGIXtalkPortClass),
    },
};

DEFINE_TYPES(sgi_xtalk_port_types)
