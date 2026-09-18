/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The network port interface; see the header for the contract. */

#include "qemu/osdep.h"
#include "hw/mips/sgi-net-port.h"
#include "qemu/log.h"
#include "qemu/module.h"

const char *sgi_net_port_describe(Object *port, unsigned int index)
{
    SGINetPortClass *pc = SGI_NET_PORT_GET_CLASS(port);

    return pc->describe ? pc->describe(port, index) : "a network port";
}

bool sgi_net_port_forwards(Object *port, unsigned int index)
{
    SGINetPortClass *pc = SGI_NET_PORT_GET_CLASS(port);

    return pc->forwards && pc->forwards(port, index);
}

SGINetPortLinkState sgi_net_port_link_state(Object *port, unsigned int index)
{
    SGINetPortClass *pc = SGI_NET_PORT_GET_CLASS(port);

    if (pc->link_state) {
        return pc->link_state(port, index);
    }
    return (SGINetPortLinkState) { .enabled = true, .eight_bit = false };
}

void sgi_net_port_peer_changed(Object *port, unsigned int index)
{
    SGINetPortClass *pc = SGI_NET_PORT_GET_CLASS(port);

    if (pc->peer_changed) {
        pc->peer_changed(port, index);
    }
}

SGINetPacketResult sgi_net_port_receive(Object *port, unsigned int index,
                                        SGINetPacket *packet, Error **errp)
{
    SGINetPortClass *pc = SGI_NET_PORT_GET_CLASS(port);
    SGINetPacketResult result = SGI_NET_PACKET_NONE;

    if (pc->receive) {
        result = pc->receive(port, index, packet, errp);
    }
    if (result == SGI_NET_PACKET_NONE) {
        /* Refused at any hop: no partial reply path survives, at any
         * level of the recursive forwarding chain this unwinds through. */
        packet->reply_vector = 0;
    }
    return result;
}

void sgi_net_port_record_check_error(Object *port, unsigned int index)
{
    SGINetPortClass *pc = SGI_NET_PORT_GET_CLASS(port);

    if (pc->record_check_error) {
        pc->record_check_error(port, index);
    }
}

bool sgi_net_packet_append_reply(SGINetPacket *packet, unsigned int index)
{
    if (packet->reply_vector > (UINT64_MAX >> 4)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-net-port: refusing overflowing vector reply "
                      "path\n");
        return false;
    }
    packet->reply_vector = (packet->reply_vector << 4) | index;
    return true;
}

static uint64_t sgi_net_port_flood_generation;

uint64_t sgi_net_port_flood_start(void)
{
    return ++sgi_net_port_flood_generation;
}

uint64_t sgi_net_port_flood_current(void)
{
    return sgi_net_port_flood_generation;
}

static const TypeInfo sgi_net_port_types[] = {
    {
        .name = TYPE_SGI_NET_PORT,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(SGINetPortClass),
    },
};

DEFINE_TYPES(sgi_net_port_types)
