/*
 * libqos PCI interface for the SGI Bridge.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef LIBQOS_PCI_SGI_BRIDGE_H
#define LIBQOS_PCI_SGI_BRIDGE_H

#include "pci.h"
#include "libqos-malloc.h"
#include "qgraph.h"

/*
 * The bus maps its Bridge and transfer memory on -M none; no board places
 * them. Leave the machine running as on PC: qtest executes no processor, but
 * a stopped machine starves network backends, and a -S migration destination
 * remains stopped after transfer.
 */
#define QPCI_SGI_BRIDGE_HEAD \
    "-cpu R10000 " \
    "-device sgi-xio-test-target,id=transfer-ram,size=16M " \
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
