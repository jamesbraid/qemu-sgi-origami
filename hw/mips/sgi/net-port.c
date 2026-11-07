/* SPDX-License-Identifier: BSD-3-Clause */
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

void sgi_net_port_link_reset(Object *port, unsigned int index)
{
    SGINetPortClass *pc = SGI_NET_PORT_GET_CLASS(port);

    if (pc->link_reset) {
        pc->link_reset(port, index);
    }
}

bool sgi_net_port_negotiate_eight_bit(Object *port, unsigned int index,
                                      Object *peer_port,
                                      unsigned int peer_index)
{
    return sgi_net_port_link_state(port, index).eight_bit ||
           sgi_net_port_link_state(peer_port, peer_index).eight_bit;
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
        /*
         * Refused at any hop: no partial reply path survives, at any
         * * level of the recursive forwarding chain this unwinds through.
         */
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

struct SGINetWarmReset {
    GPtrArray *visited;
    GPtrArray *taken;
};

SGINetWarmReset *sgi_net_warm_reset_begin(Object *origin)
{
    SGINetWarmReset *event = g_new0(SGINetWarmReset, 1);

    event->visited = g_ptr_array_new();
    event->taken = g_ptr_array_new();
    sgi_net_warm_reset_visit(event, origin);
    return event;
}

void sgi_net_warm_reset_end(SGINetWarmReset *event)
{
    if (event) {
        g_ptr_array_free(event->visited, true);
        g_ptr_array_free(event->taken, true);
        g_free(event);
    }
}

bool sgi_net_warm_reset_visit(SGINetWarmReset *event, Object *chip)
{
    if (g_ptr_array_find(event->visited, chip, NULL)) {
        return false;
    }
    g_ptr_array_add(event->visited, chip);
    return true;
}

void sgi_net_warm_reset_take(SGINetWarmReset *event, Object *chip)
{
    g_ptr_array_add(event->taken, chip);
}

GPtrArray *sgi_net_warm_reset_taken(SGINetWarmReset *event)
{
    return event->taken;
}

bool sgi_net_port_warm_reset(Object *port, unsigned int index,
                             SGINetWarmReset *event)
{
    SGINetPortClass *pc = SGI_NET_PORT_GET_CLASS(port);

    return pc->warm_reset && pc->warm_reset(port, index, event);
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
