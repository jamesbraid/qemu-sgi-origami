/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The XIO link; see the header for the contract. */

#include "qemu/osdep.h"
#include "hw/mips/sgi/xio-link.h"
#include "qapi/error.h"

void sgi_xio_link_init(SGIXioLink *link, Object *owner, const char *name,
                       Object *a, unsigned int a_index,
                       Object *b, unsigned int b_index)
{
    g_assert(!link->owner);
    link->owner = owner;
    link->name = g_strdup(name);
    link->end[0] = (SGIXioLinkEnd) { .port = a, .index = a_index };
    link->end[1] = (SGIXioLinkEnd) { .port = b, .index = b_index };
}

/*
 * A leaf accepts every destination number, so a sender addressing it by
 * (widget << 48) | address reaches the same face whatever the widget.
 */
static MemoryRegion *sgi_xio_link_leaf_fabric(SGIXioLink *link,
                                              unsigned int side,
                                              MemoryRegion *face)
{
    g_autofree char *name = g_strdup_printf("%s.fabric%u", link->name, side);
    uint64_t size = MIN(memory_region_size(face), SGI_XIO_PACKET_SIZE);
    unsigned int widget;

    memory_region_init(&link->fabric[side], link->owner, name,
                       SGI_XIO_WIDGETS * SGI_XIO_PACKET_SIZE);
    for (widget = 0; widget < SGI_XIO_WIDGETS; widget++) {
        g_autofree char *alias =
            g_strdup_printf("%s.fabric%u.widget%u", link->name, side, widget);

        memory_region_init_alias(&link->fabric_widget[side][widget],
                                 link->owner, alias, face, 0, size);
        memory_region_add_subregion(&link->fabric[side],
                                    (hwaddr)widget * SGI_XIO_PACKET_SIZE,
                                    &link->fabric_widget[side][widget]);
    }
    return &link->fabric[side];
}

bool sgi_xio_link_connect(SGIXioLink *link, Error **errp)
{
    unsigned int side;

    for (side = 0; side < 2; side++) {
        SGIXioLinkEnd *end = &link->end[side];
        SGIXioLinkEnd *far = &link->end[!side];
        SGIXioPeer peer = {
            .port = far->port,
            .index = far->index,
            .description = sgi_xio_port_describe(far->port, far->index),
            .cpu_face = sgi_xio_port_cpu_face(far->port, far->index),
            .packet_face = sgi_xio_port_packet_face(far->port, far->index),
            .switched = sgi_xio_port_switched(far->port, far->index),
        };

        if (end->connected || (!peer.cpu_face && !peer.packet_face)) {
            continue;
        }
        if (peer.packet_face) {
            peer.packet_fabric = peer.switched ? peer.packet_face :
                sgi_xio_link_leaf_fabric(link, side, peer.packet_face);
        }
        if (!sgi_xio_port_connect(end->port, end->index, &peer, errp)) {
            error_prepend(errp, "cannot join %s to %s: ",
                          sgi_xio_port_describe(end->port, end->index),
                          peer.description);
            return false;
        }
        end->connected = true;
    }
    return true;
}

bool sgi_xio_link_complete(const SGIXioLink *link)
{
    return link->end[0].connected && link->end[1].connected;
}
