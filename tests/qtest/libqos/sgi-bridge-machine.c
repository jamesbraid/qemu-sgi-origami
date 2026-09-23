/*
 * libqos driver framework: the SGI Bridge head
 *
 * The qos graph reaches PCI cards on mips64 through a Bridge built out of
 * plain devices on the "none" machine, so a card's register contract is
 * tested on the bus it ships on rather than on a PC.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "../libqtest.h"
#include "qgraph.h"
#include "pci-sgi-bridge.h"
#include "qemu/module.h"

typedef struct QSGIBridgeHost QSGIBridgeHost;
typedef struct QSGIBridgeMachine QSGIBridgeMachine;

struct QSGIBridgeHost {
    QOSGraphObject obj;
    QPCIBusSGIBridge *pci;
};

struct QSGIBridgeMachine {
    QOSGraphObject obj;
    QGuestAllocator alloc;
    QSGIBridgeHost host;
};

static QOSGraphObject *sgi_bridge_host_get_device(void *obj, const char *device)
{
    QSGIBridgeHost *host = obj;

    if (!g_strcmp0(device, "pci-bus-sgi-bridge")) {
        return &host->pci->obj;
    }
    fprintf(stderr, "%s not present in sgi-bridge\n", device);
    g_assert_not_reached();
}

static void qos_create_sgi_bridge_host(QSGIBridgeHost *host, QTestState *qts,
                                       QGuestAllocator *alloc)
{
    QPCIBus *bus = qpci_init_sgi_bridge(qts, alloc);

    host->obj.get_device = sgi_bridge_host_get_device;
    host->pci = container_of(bus, QPCIBusSGIBridge, bus);
}

static void sgi_bridge_destructor(QOSGraphObject *obj)
{
    QSGIBridgeMachine *machine = (QSGIBridgeMachine *)obj;

    qpci_free_sgi_bridge(&machine->host.pci->bus);
    alloc_destroy(&machine->alloc);
}

static void *sgi_bridge_get_driver(void *object, const char *interface)
{
    QSGIBridgeMachine *machine = object;

    if (!g_strcmp0(interface, "memory")) {
        return &machine->alloc;
    }
    fprintf(stderr, "%s not present in mips64/none\n", interface);
    g_assert_not_reached();
}

static QOSGraphObject *sgi_bridge_get_device(void *obj, const char *device)
{
    QSGIBridgeMachine *machine = obj;

    if (!g_strcmp0(device, "sgi-bridge")) {
        return &machine->host.obj;
    }
    fprintf(stderr, "%s not present in mips64/none\n", device);
    g_assert_not_reached();
}

static void *qos_create_machine_sgi_bridge(QTestState *qts)
{
    QSGIBridgeMachine *machine = g_new0(QSGIBridgeMachine, 1);

    machine->obj.get_device = sgi_bridge_get_device;
    machine->obj.get_driver = sgi_bridge_get_driver;
    machine->obj.destructor = sgi_bridge_destructor;
    qpci_sgi_bridge_alloc_init(&machine->alloc, ALLOC_NO_FLAGS);
    qos_create_sgi_bridge_host(&machine->host, qts, &machine->alloc);

    return &machine->obj;
}

static void sgi_bridge_machine_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .before_cmd_line = QPCI_SGI_BRIDGE_HEAD,
    };

    qos_node_create_machine("mips64/none", qos_create_machine_sgi_bridge);
    qos_node_contains("mips64/none", "sgi-bridge", &opts, NULL);

    qos_node_create_driver("sgi-bridge", NULL);
    qos_node_contains("sgi-bridge", "pci-bus-sgi-bridge", NULL);
}

libqos_init(sgi_bridge_machine_register_nodes);
