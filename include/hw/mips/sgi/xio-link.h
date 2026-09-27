/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The XIO link: one point-to-point connection between two XIO ports.
 * Hardware has one kind of XIO link, whatever sits at either end
 * (Theory of Operations 007-3439-002 p.53: a Hub's single XIO port goes
 * to a Bridge, or to a Crossbow that expands it to eight). What differs
 * between a Hub cabled to a Bridge and a Hub cabled to a Crossbow is
 * what the far end does with the destination widget: a Crossbow routes
 * by it, a Bridge or a Hub accepts any. That decision belongs to the
 * ports (hw/mips/sgi-xio-port.h), so the link carries no chip behaviour:
 * it hands each end the other's faces, and for a leaf peer it presents
 * that peer's packet face at every widget number, the one piece of
 * address shaping every sender would otherwise repeat.
 *
 * Storage is caller-owned, the way the machine's other glue is: the
 * machine keeps an array of these and hands each element in. Every
 * region the link builds is parented to the owner the caller names.
 */

#ifndef HW_MIPS_SGI_XIO_LINK_H
#define HW_MIPS_SGI_XIO_LINK_H

#include "hw/mips/sgi-xio-port.h"
#include "qom/object.h"
#include "system/memory.h"

typedef struct SGIXioLinkEnd {
    Object *port;
    unsigned int index;
    /* Whether this end has taken the other end's faces. */
    bool connected;
} SGIXioLinkEnd;

typedef struct SGIXioLink {
    Object *owner;
    char *name;
    SGIXioLinkEnd end[2];

    /*
     * For each end, the
     * other end's packet face as a widget-addressed space when that other
     * end is a leaf: sixteen aliases of the same face, one per widget number.
     */
    MemoryRegion fabric[2];
    MemoryRegion fabric_widget[2][SGI_XIO_WIDGETS];
} SGIXioLink;

/*
 * Name the two ports a link joins. Nothing is connected until
 * sgi_xio_link_connect() runs.
 */
void sgi_xio_link_init(SGIXioLink *link, Object *owner, const char *name,
                       Object *a, unsigned int a_index,
                       Object *b, unsigned int b_index);

/*
 * Hand each end the other's faces, for every end whose peer has built
 * them and which has not already taken them. A board's faces exist only
 * once it has realized, while it must take its own outbound target
 * before that, so a caller joining a board calls this once before
 * realizing the board and once after; the second call finishes the
 * other direction.
 */
bool sgi_xio_link_connect(SGIXioLink *link,
                                         Error **errp);

/* Whether both ends have taken each other's faces. */
bool sgi_xio_link_complete(const SGIXioLink *link);

#endif /* HW_MIPS_SGI_XIO_LINK_H */
