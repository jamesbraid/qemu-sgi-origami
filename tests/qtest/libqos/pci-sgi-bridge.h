/*
 * libqos PCI bindings for the SGI Bridge head
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LIBQOS_PCI_SGI_BRIDGE_H
#define LIBQOS_PCI_SGI_BRIDGE_H

#include "pci.h"
#include "libqos-malloc.h"
#include "qgraph.h"

/*
 * The head: a Bridge with nothing above it but the "none" machine, and the
 * memory a card transfers into. Neither is placed by a board, so the bus
 * maps both itself and is the only thing that decides where they sit.
 */
#define QPCI_SGI_BRIDGE_HEAD \
    "-cpu R10000 -S " \
    "-device sgi-xtalk-test-target,id=transfer-ram,size=16M " \
    "-device sgi-bridge,id=bridge,board-part=030-1244-001," \
    "fabric=/machine/unattached/system[0] "

typedef struct QPCIBusSGIBridge {
    QOSGraphObject obj;
    QPCIBus bus;
    /* Whose device window this bus points at PCI I/O space. */
    unsigned int io_slot;
} QPCIBusSGIBridge;

QPCIBus *qpci_init_sgi_bridge(QTestState *qts, QGuestAllocator *alloc);
void qpci_free_sgi_bridge(QPCIBus *bus);
void qpci_sgi_bridge_alloc_init(QGuestAllocator *alloc, QAllocOpts flags);

#endif
