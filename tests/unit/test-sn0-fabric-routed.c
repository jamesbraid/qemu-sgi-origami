/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Synthetic graphs exercise Router forwarding beyond the module topology.
 * A minimal test Hub type stands in for the real Hub (hw/misc/sgi-hub.c),
 * isolating the graph-walking contract in hw/misc/sgi-router.c and
 * hw/mips/sgi/net-port.c, net-cable.c from the real Hub's own register
 * decode -- the same isolation the fabric-based version of this file used.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/resettable.h"
#include "hw/core/qdev-properties.h"
#include "hw/mips/sgi-net-port.h"
#include "hw/mips/sgi/net-cable.h"
#include "hw/misc/sgi-router.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "system/cpu-timers.h"

#define RR_DIAG_PARMS          0x00030
#define RR_STATUS_ERROR(port)  (((uint64_t)(port) << 16) + 0x0008)
#define RR_CHECK_COUNT(value)  extract64(value, 8, 8)
#define RR_RETRY_COUNT(value)  extract64(value, 16, 8)
#define NI_DIAG_SENDERROR      UINT64_C(1)
#define NI_CHECK_COUNT(value)  extract64(value, 8, 8)

/* ---- A minimal Hub stand-in, implementing SGI_NET_PORT directly ---- */

#define TYPE_TEST_HUB "test-net-hub"
OBJECT_DECLARE_SIMPLE_TYPE(TestHubState, TEST_HUB)

struct TestHubState {
    DeviceState parent_obj;
    uint32_t nasid;
    uint64_t ni_diag_parms;
    uint64_t ni_port_error;
    /* This test Hub's one port's cable, wired directly by the test. */
    SGINetCableState *cable;
};

static SGINetPortLinkState test_hub_link_state(Object *port,
                                               unsigned int index)
{
    return (SGINetPortLinkState) { .enabled = true, .eight_bit = false };
}

static SGINetPacketResult test_hub_receive(Object *port, unsigned int index,
                                           SGINetPacket *packet,
                                           Error **errp)
{
    TestHubState *hub = TEST_HUB(port);

    if (packet->op == SGI_NET_PACKET_READ ||
        packet->op == SGI_NET_PACKET_EXCHANGE) {
        packet->value = hub->nasid;
    }
    packet->source = hub->nasid;
    return SGI_NET_PACKET_OK;
}

static void test_hub_record_check_error(Object *port, unsigned int index)
{
    TestHubState *hub = TEST_HUB(port);
    uint64_t count = NI_CHECK_COUNT(hub->ni_port_error);

    if (count != 0xff) {
        hub->ni_port_error = deposit64(hub->ni_port_error, 8, 8, count + 1);
    }
}

static void test_hub_class_init(ObjectClass *oc, const void *data)
{
    SGINetPortClass *pc = SGI_NET_PORT_CLASS(oc);

    pc->link_state = test_hub_link_state;
    pc->receive = test_hub_receive;
    pc->record_check_error = test_hub_record_check_error;
}

static const TypeInfo test_hub_type = {
    .name = TYPE_TEST_HUB,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(TestHubState),
    .class_init = test_hub_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_SGI_NET_PORT },
        { }
    },
};

/* Launch a packet from a test Hub's own port -- the minimum an origin
 * needs to do; the real Hub's own direct-link and write-charge special
 * cases (hw/misc/sgi-hub.c's sgi_hub_vector_launch()) are exercised
 * against the real Hub in tests/qtest/sn0-fabric-test.c instead. */
static SGINetPacketResult test_hub_launch(TestHubState *hub,
                                          SGINetPacket *packet)
{
    Object *peer_port;
    unsigned int peer_index;

    if (!hub->cable ||
        !sgi_net_cable_neighbor(hub->cable, OBJECT(hub), 0, &peer_port,
                                &peer_index)) {
        return SGI_NET_PACKET_NONE;
    }
    /* Mirrors the write-time data-error charge sgi_hub_vector_launch()
     * (hw/misc/sgi-hub.c) places on the first Router's arrival port. */
    if (packet->op == SGI_NET_PACKET_WRITE &&
        (hub->ni_diag_parms & NI_DIAG_SENDERROR) &&
        sgi_net_port_forwards(peer_port, peer_index)) {
        sgi_net_port_record_check_error(peer_port, peer_index);
    }
    return sgi_net_port_receive(peer_port, peer_index, packet, &error_abort);
}

static TestHubState *test_hub_new(Object *parent, const char *name,
                                  uint32_t nasid)
{
    DeviceState *dev = qdev_new(TYPE_TEST_HUB);
    TestHubState *hub = TEST_HUB(dev);

    hub->nasid = nasid;
    object_property_add_child(parent, name, OBJECT(dev));
    qdev_realize(dev, NULL, &error_abort);
    return hub;
}

/* A cable straight between two ports, realized and ready. */
static SGINetCableState *test_cable_new(Object *parent, const char *name,
                                        Object *a, unsigned int a_index,
                                        Object *b, unsigned int b_index)
{
    DeviceState *dev = qdev_new(TYPE_SGI_NET_CABLE);
    SGINetCableState *cable = SGI_NET_CABLE(dev);

    object_property_add_child(parent, name, OBJECT(dev));
    sgi_net_cable_set_end(cable, 0, a, a_index);
    sgi_net_cable_set_end(cable, 1, b, b_index);
    qdev_realize(dev, NULL, &error_abort);
    return cable;
}

static void router_port_cable(SGIRouterState *router, unsigned int port,
                              SGINetCableState *cable)
{
    g_autofree char *prop = g_strdup_printf("port%u-cable", port);

    object_property_set_link(OBJECT(router), prop, OBJECT(cable),
                             &error_abort);
}

static SGIRouterState *test_router_new(Object *parent, const char *name)
{
    DeviceState *dev = qdev_new(TYPE_SGI_ROUTER);

    object_property_add_child(parent, name, OBJECT(dev));
    return SGI_ROUTER(dev);
}

int64_t cpus_get_virtual_clock(void)
{
    return 0;
}

void cpus_set_virtual_clock(int64_t value)
{
}

static void write_router(SGIRouterState *router, uint64_t address,
                         uint64_t value)
{
    SGINetPacket access = {
        .op = SGI_NET_PACKET_WRITE,
        .addr = address,
        .data = value,
    };

    g_assert_true(sgi_router_access(router, 1, &access));
}

static uint64_t read_router(SGIRouterState *router, uint64_t address)
{
    SGINetPacket access = {
        .op = SGI_NET_PACKET_READ,
        .addr = address,
    };

    g_assert_true(sgi_router_access(router, 1, &access));
    return access.value;
}

static TestHubState *test_reach(TestHubState *origin, uint32_t destination);

static void test_diagnostic_error_paths(void)
{
    Object *container = object_property_add_new_container(
        object_get_root(), "diagnostic-error-paths");
    TestHubState *hub0 = test_hub_new(container, "hub0", 3);
    TestHubState *hub1 = test_hub_new(container, "hub1", 7);
    SGIRouterState *router0 = test_router_new(container, "router0");
    SGIRouterState *router1 = test_router_new(container, "router1");
    SGINetCableState *source =
        test_cable_new(container, "source", OBJECT(hub0), 0,
                       OBJECT(router0), 5);
    SGINetCableState *routers =
        test_cable_new(container, "routers", OBJECT(router0), 1,
                       OBJECT(router1), 2);
    SGINetCableState *destination =
        test_cable_new(container, "destination", OBJECT(router1), 4,
                       OBJECT(hub1), 0);
    SGINetPacket access = { .op = SGI_NET_PACKET_WRITE, .addr = 0 };
    uint64_t errors;

    hub0->cable = source;
    hub1->cable = destination;
    router_port_cable(router0, 5, source);
    router_port_cable(router0, 1, routers);
    router_port_cable(router1, 2, routers);
    router_port_cable(router1, 4, destination);
    qdev_realize(DEVICE(router0), NULL, &error_abort);
    device_cold_reset(DEVICE(router0));
    qdev_realize(DEVICE(router1), NULL, &error_abort);
    device_cold_reset(DEVICE(router1));

    /* Hub SENDERROR affects only an outbound vector-write request. */
    hub0->ni_diag_parms = NI_DIAG_SENDERROR;
    access.vector = 0x41;
    g_assert_cmpint(test_hub_launch(hub0, &access), ==, SGI_NET_PACKET_OK);
    errors = read_router(router0, RR_STATUS_ERROR(5));
    g_assert_cmpuint(RR_CHECK_COUNT(errors), ==, 1);
    g_assert_cmpuint(RR_RETRY_COUNT(errors), ==, 0);

    access = (SGINetPacket) { .op = SGI_NET_PACKET_READ, .vector = 0x41 };
    g_assert_cmpint(test_hub_launch(hub0, &access), ==, SGI_NET_PACKET_OK);
    g_assert_cmpuint(RR_CHECK_COUNT(
                         read_router(router0, RR_STATUS_ERROR(5))), ==, 1);

    /* A Router corrupts the reply received by its immediate peer. */
    hub0->ni_diag_parms = 0;
    write_router(router1, RR_DIAG_PARMS, UINT64_C(1) << (2 - 1));
    access = (SGINetPacket) { .op = SGI_NET_PACKET_WRITE, .vector = 0x41 };
    g_assert_cmpint(test_hub_launch(hub0, &access), ==, SGI_NET_PACKET_OK);
    g_assert_cmpuint(NI_CHECK_COUNT(hub0->ni_port_error), ==, 0);
    access = (SGINetPacket) { .op = SGI_NET_PACKET_READ, .vector = 0x41 };
    g_assert_cmpint(test_hub_launch(hub0, &access), ==, SGI_NET_PACKET_OK);
    errors = read_router(router0, RR_STATUS_ERROR(1));
    g_assert_cmpuint(RR_CHECK_COUNT(errors), ==, 1);
    g_assert_cmpuint(RR_RETRY_COUNT(errors), ==, 0);
    g_assert_cmpuint(NI_CHECK_COUNT(hub0->ni_port_error), ==, 0);

    /* The Router directly connected to the Hub charges the Hub. */
    write_router(router1, RR_DIAG_PARMS, 0);
    write_router(router0, RR_DIAG_PARMS, UINT64_C(1) << (5 - 1));
    access = (SGINetPacket) { .op = SGI_NET_PACKET_READ, .vector = 0x41 };
    g_assert_cmpint(test_hub_launch(hub0, &access), ==, SGI_NET_PACKET_OK);
    g_assert_cmpuint(NI_CHECK_COUNT(hub0->ni_port_error), ==, 1);

    object_unparent(container);
}

static void test_route_length(gconstpointer opaque)
{
    unsigned int count = GPOINTER_TO_UINT(opaque);
    Object *container = object_property_add_new_container(
        object_get_root(), "route-length");
    TestHubState *hub0 = test_hub_new(container, "hub0", 3);
    TestHubState *hub1 = test_hub_new(container, "hub1", 7);
    g_autoptr(GPtrArray) routers = g_ptr_array_new();
    Object *previous_port = OBJECT(hub0);
    unsigned int previous_index = 0;
    SGINetCableState *cable;
    unsigned int i;
    SGINetPacket access;

    /*
     * Every port-cable property must be set before its own Router
     * realizes, so every Router in the chain is created first, wired
     * entirely, and only then realized and reset.
     */
    for (i = 0; i < count; i++) {
        g_autofree char *name = g_strdup_printf("router%u", i);

        g_ptr_array_add(routers, test_router_new(container, name));
    }
    for (i = 0; i < count; i++) {
        g_autofree char *link_name = g_strdup_printf("link%u", i);
        SGIRouterState *router = g_ptr_array_index(routers, i);

        /* Every hop enters port 1 and exits port 2, carrying relative 1. */
        cable = test_cable_new(container, link_name, previous_port,
                               previous_index, OBJECT(router), 1);
        if (previous_port == OBJECT(hub0)) {
            hub0->cable = cable;
        } else {
            router_port_cable(SGI_ROUTER(previous_port),
                              previous_index, cable);
        }
        router_port_cable(router, 1, cable);
        previous_port = OBJECT(router);
        previous_index = 2;
    }
    cable = test_cable_new(container, "destination", previous_port,
                           previous_index, OBJECT(hub1), 0);
    if (previous_port == OBJECT(hub0)) {
        hub0->cable = cable;
    } else {
        router_port_cable(SGI_ROUTER(previous_port), previous_index, cable);
    }
    hub1->cable = cable;
    for (i = 0; i < count; i++) {
        SGIRouterState *router = g_ptr_array_index(routers, i);

        qdev_realize(DEVICE(router), NULL, &error_abort);
        device_cold_reset(DEVICE(router));
        write_router(router, 0x10, UINT64_C(1) << 12);
        write_router(router, 0x70100 + 7 * 8, 0xffff1f);
    }

    if (count == 15) {
        g_assert_true(test_reach(hub0, 7) == hub1);
        access = (SGINetPacket) {
            .op = SGI_NET_PACKET_READ, .vector = 0x222222222222222,
        };
        g_assert_cmpint(test_hub_launch(hub0, &access), ==,
                        SGI_NET_PACKET_OK);
        g_assert_cmphex(access.reply_vector, ==, 0x111111111111111);
    } else {
        g_assert_null(test_reach(hub0, 7));
        /* Fifteen forwarded hops plus the answering Router fit 64 bits. */
        access = (SGINetPacket) {
            .op = SGI_NET_PACKET_READ, .vector = 0x222222222222222,
        };
        g_assert_cmpint(test_hub_launch(hub0, &access), ==,
                        SGI_NET_PACKET_OK);
        g_assert_cmphex(access.reply_vector, ==, 0x1111111111111111);
        access = (SGINetPacket) {
            .op = SGI_NET_PACKET_READ, .vector = 0x2222222222222222,
        };
        g_assert_cmpint(test_hub_launch(hub0, &access), ==,
                        SGI_NET_PACKET_NONE);
        g_assert_cmphex(access.reply_vector, ==, 0);
    }
    object_unparent(container);
}

static void test_vector_reply_path(void)
{
    const unsigned int input[] = { 5, 2, 6 };
    const unsigned int output[] = { 1, 4, 3 };
    Object *container = object_property_add_new_container(
        object_get_root(), "vector-reply-path");
    TestHubState *hub0 = test_hub_new(container, "hub0", 3);
    TestHubState *hub1 = test_hub_new(container, "hub1", 7);
    g_autoptr(GPtrArray) routers = g_ptr_array_new();
    Object *previous_port = OBJECT(hub0);
    unsigned int previous_index = 0;
    SGINetCableState *cable;
    SGINetCableState *routers_link = NULL;
    SGINetPacket access;
    const uint64_t requests[] = { 0, 1, 0x41 };
    const uint64_t replies[] = { 5, 0x52, 0x526 };
    uint64_t reply;

    for (unsigned int i = 0; i < G_N_ELEMENTS(input); i++) {
        g_autofree char *name = g_strdup_printf("router%u", i);

        g_ptr_array_add(routers, test_router_new(container, name));
    }
    for (unsigned int i = 0; i < G_N_ELEMENTS(input); i++) {
        g_autofree char *link_name = g_strdup_printf("link%u", i);
        SGIRouterState *router = g_ptr_array_index(routers, i);

        cable = test_cable_new(container, link_name, previous_port,
                               previous_index, OBJECT(router), input[i]);
        if (previous_port == OBJECT(hub0)) {
            hub0->cable = cable;
        } else {
            router_port_cable(SGI_ROUTER(previous_port), previous_index,
                              cable);
        }
        router_port_cable(router, input[i], cable);
        if (i == 1) {
            routers_link = cable;
        }
        previous_port = OBJECT(router);
        previous_index = output[i];
    }
    cable = test_cable_new(container, "destination", previous_port,
                           previous_index, OBJECT(hub1), 0);
    router_port_cable(SGI_ROUTER(previous_port), previous_index, cable);
    hub1->cable = cable;
    for (unsigned int i = 0; i < routers->len; i++) {
        DeviceState *router = g_ptr_array_index(routers, i);

        qdev_realize(router, NULL, &error_abort);
        device_cold_reset(router);
    }

    /* Reverse the traversed ingress ports, not the request's exit ports. */
    access = (SGINetPacket) { .op = SGI_NET_PACKET_READ, .vector = 0x341 };
    g_assert_cmpint(test_hub_launch(hub0, &access), ==, SGI_NET_PACKET_OK);
    g_assert_cmpuint(access.value, ==, 7);
    g_assert_cmphex(access.reply_vector, ==, 0x526);
    reply = access.reply_vector;
    access = (SGINetPacket) { .op = SGI_NET_PACKET_READ, .vector = reply };
    g_assert_cmpint(test_hub_launch(hub1, &access), ==, SGI_NET_PACKET_OK);
    g_assert_cmpuint(access.value, ==, 3);
    g_assert_cmphex(access.reply_vector, ==, 0x341);

    /* A Router responder must include its own ingress port in the reply. */
    for (unsigned int i = 0; i < G_N_ELEMENTS(requests); i++) {
        access = (SGINetPacket) {
            .op = SGI_NET_PACKET_READ, .vector = requests[i],
        };
        g_assert_cmpint(test_hub_launch(hub0, &access), ==,
                        SGI_NET_PACKET_OK);
        g_assert_cmphex(access.reply_vector, ==, replies[i]);
    }
    /* Refused requests must not expose a previous reply or a partial path. */
    access = (SGINetPacket) { .op = SGI_NET_PACKET_READ, .vector = 0x347 };
    g_assert_cmpint(test_hub_launch(hub0, &access), ==, SGI_NET_PACKET_NONE);
    g_assert_cmphex(access.reply_vector, ==, 0);

    object_property_set_bool(OBJECT(routers_link), "down", true,
                             &error_abort);
    access = (SGINetPacket) { .op = SGI_NET_PACKET_READ, .vector = 0x341 };
    g_assert_cmpint(test_hub_launch(hub0, &access), ==, SGI_NET_PACKET_NONE);
    g_assert_cmphex(access.reply_vector, ==, 0);

    object_unparent(container);
}

/*
 * Mirrors the offline reachability walk a real Hub derives its own
 * claim-alias map with (sgi_hub_walk()/sgi_hub_reach(), hw/misc/sgi-
 * hub.c) -- distinct from live vector forwarding above, and, like the
 * fabric-based version of this file, stubbing the first turn at 1: a
 * bare test Hub carries no route tables of its own, so this exercises
 * only what the Router's own tables and the duplicate-number tie-break
 * contribute.
 */
static TestHubState *test_reach(TestHubState *origin, uint32_t destination)
{
    Object *peer_port;
    unsigned int peer_index;
    uint8_t turn = 1;
    unsigned int hops = 0;

    if (destination < 0x100 && destination == origin->nasid) {
        return origin;
    }
    if (!origin->cable ||
        !sgi_net_cable_neighbor(origin->cable, OBJECT(origin), 0, &peer_port,
                                &peer_index)) {
        return NULL;
    }
    while (sgi_net_port_forwards(peer_port, peer_index)) {
        SGIRouterState *router = SGI_ROUTER(peer_port);
        unsigned int exit_port;
        uint8_t next_turn;

        if (hops == SGI_NET_PORT_MAX_HOPS) {
            return NULL;
        }
        if (!sgi_router_route(router, peer_index, turn, destination,
                              &exit_port, &next_turn)) {
            return NULL;
        }
        hops++;
        if (!sgi_router_port_neighbor(router, exit_port, &peer_port,
                                      &peer_index)) {
            return NULL;
        }
        turn = next_turn;
    }
    if (TEST_HUB(peer_port)->nasid != destination) {
        return NULL;
    }
    return TEST_HUB(peer_port);
}

/*
 * Firmware numbers the nodes one at a time, so a Hub that has taken its real
 * number sits beside Hubs still carrying the reset number zero. It has to go
 * on reaching zero: its own firmware text and its exception vectors are
 * node-field-zero addresses.
 */
static void test_duplicate_number_stays_routed(void)
{
    Object *container = object_property_add_new_container(
        object_get_root(), "duplicate-number-stays-routed");
    TestHubState *hubs[3];
    SGIRouterState *router = test_router_new(container, "router");
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(hubs); i++) {
        g_autofree char *name = g_strdup_printf("hub%u", i);

        hubs[i] = test_hub_new(container, name, 0);
    }
    for (i = 0; i < G_N_ELEMENTS(hubs); i++) {
        g_autofree char *name = g_strdup_printf("hub%u-cable", i);
        SGINetCableState *cable = test_cable_new(
            container, name, OBJECT(hubs[i]), 0, OBJECT(router), i + 1);

        hubs[i]->cable = cable;
        router_port_cable(router, i + 1, cable);
    }
    qdev_realize(DEVICE(router), NULL, &error_abort);
    device_cold_reset(DEVICE(router));
    write_router(router, 0x10, UINT64_C(1) << 12);
    /* Destination zero leaves by the port the first turn selects. */
    write_router(router, 0x70100, 0xffff1f);

    /* While every Hub carries zero, each answers for itself. */
    for (i = 0; i < G_N_ELEMENTS(hubs); i++) {
        g_assert_true(test_reach(hubs[i], 0) == hubs[i]);
    }

    /* One Hub takes its real number; the other two have not yet. */
    hubs[0]->nasid = 3;
    g_assert_true(test_reach(hubs[0], 0) == hubs[1]);
    g_assert_true(test_reach(hubs[0], 3) == hubs[0]);
    g_assert_true(test_reach(hubs[1], 0) == hubs[1]);
    g_assert_true(test_reach(hubs[2], 0) == hubs[2]);

    /* A number no Hub holds is nobody's. */
    g_assert_null(test_reach(hubs[0], 5));

    /*
     * The turns decide which Hub is asked, and the asked Hub has to hold the
     * number: take the Hub this route reaches off zero, and zero is refused
     * even though another Hub still holds it and still answers for itself.
     */
    hubs[1]->nasid = 2;
    g_assert_null(test_reach(hubs[0], 0));
    g_assert_true(test_reach(hubs[2], 0) == hubs[2]);

    object_unparent(container);
}

int main(int argc, char **argv)
{
    Object *machine;

    g_test_init(&argc, &argv, NULL);
    qemu_init_main_loop(&error_abort);
    module_call_init(MODULE_INIT_QOM);
    type_register_static(&test_hub_type);
    machine = object_property_add_new_container(object_get_root(), "machine");
    object_property_add_new_container(machine, "unattached");
    g_test_add_func("/sn0-fabric-routed/vector-reply-path",
                    test_vector_reply_path);
    g_test_add_func("/sn0-fabric-routed/diagnostic-error-paths",
                    test_diagnostic_error_paths);
    g_test_add_func("/sn0-fabric-routed/duplicate-number-stays-routed",
                    test_duplicate_number_stays_routed);
    g_test_add_data_func("/sn0-fabric-routed/fifteen-hops",
                        GUINT_TO_POINTER(15), test_route_length);
    g_test_add_data_func("/sn0-fabric-routed/sixteen-hops",
                        GUINT_TO_POINTER(16), test_route_length);
    return g_test_run();
}
