/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic graphs exercise the routed adapter beyond the module topology. */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/resettable.h"
#include "hw/mips/sn0-fabric-routed.h"
#include "hw/misc/sgi-hub.h"
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

/* Only the Hub endpoint is a fixture; routing uses the real Router devices. */
bool sgi_hub_first_turn(const SGIHubState *hub, uint32_t destination,
                        uint8_t *turn)
{
    *turn = 1;
    return true;
}

uint32_t sgi_hub_programmed_nasid(const SGIHubState *hub)
{
    return hub->nasid;
}

void sgi_hub_fabric_changed(SGIHubState *hub)
{
}

bool sgi_hub_vector_access(SGIHubState *hub, SN0FabricVectorAccess *access)
{
    if (access->op == SN0_FABRIC_VECTOR_READ) {
        access->value = hub->nasid;
    }
    return true;
}

bool sgi_hub_sends_data_error(const SGIHubState *hub)
{
    return hub->ni_diag_parms & NI_DIAG_SENDERROR;
}

void sgi_hub_record_port_check_error(SGIHubState *hub)
{
    uint64_t count = NI_CHECK_COUNT(hub->ni_port_error);

    if (count != 0xff) {
        hub->ni_port_error = deposit64(hub->ni_port_error, 8, 8, count + 1);
    }
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
    SN0FabricVectorAccess access = {
        .op = SN0_FABRIC_VECTOR_WRITE,
        .addr = address,
        .data = value,
    };

    g_assert_true(sgi_router_access(router, 1, &access));
}

static uint64_t read_router(SGIRouterState *router, uint64_t address)
{
    SN0FabricVectorAccess access = {
        .op = SN0_FABRIC_VECTOR_READ,
        .addr = address,
    };

    g_assert_true(sgi_router_access(router, 1, &access));
    return access.value;
}

static void test_diagnostic_error_paths(void)
{
    SGIHubState hubs[2] = { { .nasid = 3 }, { .nasid = 7 } };
    SN0FabricState *fabric = SN0_FABRIC(qdev_new(TYPE_SN0_FABRIC_ROUTED));
    DeviceState *devices[] = {
        qdev_new(TYPE_SGI_ROUTER), qdev_new(TYPE_SGI_ROUTER),
    };
    SGIRouterState *routers[] = {
        SGI_ROUTER(devices[0]), SGI_ROUTER(devices[1]),
    };
    SN0FabricVectorAccess access = {
        .op = SN0_FABRIC_VECTOR_WRITE,
    };
    uint64_t errors;

    for (unsigned int i = 0; i < G_N_ELEMENTS(hubs); i++) {
        sn0_fabric_add_endpoint(fabric, &hubs[i], i);
        sn0_fabric_report_number(fabric, i, hubs[i].nasid);
    }
    for (unsigned int i = 0; i < G_N_ELEMENTS(devices); i++) {
        qdev_realize(devices[i], NULL, &error_abort);
        device_cold_reset(devices[i]);
        sn0_fabric_routed_add_router(fabric, routers[i]);
    }
    sn0_fabric_routed_add_link(fabric, "source",
        (SN0FabricVertex) { SN0_FABRIC_VERTEX_HUB, 0, 0 },
        (SN0FabricVertex) { SN0_FABRIC_VERTEX_ROUTER, 0, 5 });
    sn0_fabric_routed_add_link(fabric, "routers",
        (SN0FabricVertex) { SN0_FABRIC_VERTEX_ROUTER, 0, 1 },
        (SN0FabricVertex) { SN0_FABRIC_VERTEX_ROUTER, 1, 2 });
    sn0_fabric_routed_add_link(fabric, "destination",
        (SN0FabricVertex) { SN0_FABRIC_VERTEX_ROUTER, 1, 4 },
        (SN0FabricVertex) { SN0_FABRIC_VERTEX_HUB, 1, 0 });
    qdev_realize(DEVICE(fabric), NULL, &error_abort);
    sn0_fabric_build(fabric);

    /* Hub SENDERROR affects only an outbound vector-write request. */
    hubs[0].ni_diag_parms = NI_DIAG_SENDERROR;
    g_assert_cmpint(sn0_fabric_vector(fabric, 0, 0x41, &access), ==,
                    SN0_FABRIC_VECTOR_OK);
    errors = read_router(routers[0], RR_STATUS_ERROR(5));
    g_assert_cmpuint(RR_CHECK_COUNT(errors), ==, 1);
    g_assert_cmpuint(RR_RETRY_COUNT(errors), ==, 0);

    access.op = SN0_FABRIC_VECTOR_READ;
    g_assert_cmpint(sn0_fabric_vector(fabric, 0, 0x41, &access), ==,
                    SN0_FABRIC_VECTOR_OK);
    g_assert_cmpuint(RR_CHECK_COUNT(
                         read_router(routers[0], RR_STATUS_ERROR(5))), ==, 1);

    /* A Router corrupts the reply received by its immediate peer. */
    hubs[0].ni_diag_parms = 0;
    write_router(routers[1], RR_DIAG_PARMS, UINT64_C(1) << (2 - 1));
    access.op = SN0_FABRIC_VECTOR_WRITE;
    g_assert_cmpint(sn0_fabric_vector(fabric, 0, 0x41, &access), ==,
                    SN0_FABRIC_VECTOR_OK);
    g_assert_cmpuint(NI_CHECK_COUNT(hubs[0].ni_port_error), ==, 0);
    access.op = SN0_FABRIC_VECTOR_READ;
    g_assert_cmpint(sn0_fabric_vector(fabric, 0, 0x41, &access), ==,
                    SN0_FABRIC_VECTOR_OK);
    errors = read_router(routers[0], RR_STATUS_ERROR(1));
    g_assert_cmpuint(RR_CHECK_COUNT(errors), ==, 1);
    g_assert_cmpuint(RR_RETRY_COUNT(errors), ==, 0);
    g_assert_cmpuint(NI_CHECK_COUNT(hubs[0].ni_port_error), ==, 0);

    /* The Router directly connected to the Hub charges the Hub. */
    write_router(routers[1], RR_DIAG_PARMS, 0);
    write_router(routers[0], RR_DIAG_PARMS, UINT64_C(1) << (5 - 1));
    g_assert_cmpint(sn0_fabric_vector(fabric, 0, 0x41, &access), ==,
                    SN0_FABRIC_VECTOR_OK);
    g_assert_cmpuint(NI_CHECK_COUNT(hubs[0].ni_port_error), ==, 1);

    object_unparent(OBJECT(fabric));
    object_unref(OBJECT(fabric));
    for (unsigned int i = 0; i < G_N_ELEMENTS(devices); i++) {
        object_unparent(OBJECT(devices[i]));
        object_unref(OBJECT(devices[i]));
    }
}

static void test_route_length(gconstpointer opaque)
{
    unsigned int count = GPOINTER_TO_UINT(opaque);
    SGIHubState hubs[2] = { { .nasid = 3 }, { .nasid = 7 } };
    SN0FabricState *fabric = SN0_FABRIC(qdev_new(TYPE_SN0_FABRIC_ROUTED));
    g_autoptr(GPtrArray) routers = g_ptr_array_new_with_free_func(object_unref);
    SN0FabricVertex previous = { SN0_FABRIC_VERTEX_HUB, 0, 0 };
    g_autofree char *routes = NULL;
    unsigned int i;
    SN0FabricVectorAccess access = { .op = SN0_FABRIC_VECTOR_READ };

    for (i = 0; i < G_N_ELEMENTS(hubs); i++) {
        sn0_fabric_add_endpoint(fabric, &hubs[i], i);
        sn0_fabric_report_number(fabric, i, hubs[i].nasid);
    }
    for (i = 0; i < count; i++) {
        DeviceState *device = qdev_new(TYPE_SGI_ROUTER);
        SGIRouterState *router = SGI_ROUTER(device);
        SN0FabricVertex input = { SN0_FABRIC_VERTEX_ROUTER, i, 1 };
        g_autofree char *name = g_strdup_printf("link%u", i);

        qdev_realize(device, NULL, &error_abort);
        device_cold_reset(device);
        g_ptr_array_add(routers, device);
        /* Every hop enters port 1 and exits port 2, carrying relative 1. */
        write_router(router, 0x10, UINT64_C(1) << 12);
        write_router(router, 0x70100 + 7 * 8, 0xffff1f);
        sn0_fabric_routed_add_router(fabric, router);
        sn0_fabric_routed_add_link(fabric, name, previous, input);
        previous = (SN0FabricVertex) { SN0_FABRIC_VERTEX_ROUTER, i, 2 };
    }
    sn0_fabric_routed_add_link(fabric, "destination", previous,
        (SN0FabricVertex) { SN0_FABRIC_VERTEX_HUB, 1, 0 });
    qdev_realize(DEVICE(fabric), NULL, &error_abort);
    sn0_fabric_build(fabric);
    routes = object_property_get_str(OBJECT(fabric), "routes", &error_abort);
    if (count == 15) {
        g_assert_cmpint(sn0_fabric_map(fabric, 0)[7], ==, 1);
        g_assert_cmpstr(routes, ==, "0>1=0x222222222222222 1>0=-");
        g_assert_cmpint(sn0_fabric_vector(fabric, 0, 0x222222222222222,
                                          &access), ==, SN0_FABRIC_VECTOR_OK);
        g_assert_cmphex(access.reply_vector, ==, 0x111111111111111);
    } else {
        g_assert_cmpint(sn0_fabric_map(fabric, 0)[7], ==, SN0_FABRIC_NONE);
        g_assert_cmpstr(routes, ==, "0>1=- 1>0=-");
        /* Fifteen forwarded hops plus the answering Router fit 64 bits. */
        g_assert_cmpint(sn0_fabric_vector(fabric, 0, 0x222222222222222,
                                          &access), ==, SN0_FABRIC_VECTOR_OK);
        g_assert_cmphex(access.reply_vector, ==, 0x1111111111111111);
        g_assert_cmpint(sn0_fabric_vector(fabric, 0, 0x2222222222222222,
                                          &access), ==, SN0_FABRIC_VECTOR_NONE);
        g_assert_cmphex(access.reply_vector, ==, 0);
    }
    object_unparent(OBJECT(fabric));
    object_unref(OBJECT(fabric));
    for (i = 0; i < routers->len; i++) {
        object_unparent(g_ptr_array_index(routers, i));
    }
}

static void test_vector_reply_path(void)
{
    const unsigned int input[] = { 5, 2, 6 };
    const unsigned int output[] = { 1, 4, 3 };
    SGIHubState hubs[2] = { { .nasid = 3 }, { .nasid = 7 } };
    SN0FabricState *fabric = SN0_FABRIC(qdev_new(TYPE_SN0_FABRIC_ROUTED));
    g_autoptr(GPtrArray) routers = g_ptr_array_new_with_free_func(object_unref);
    SN0FabricVertex previous = { SN0_FABRIC_VERTEX_HUB, 0, 0 };
    SN0FabricVectorAccess access = { .op = SN0_FABRIC_VECTOR_READ };
    const uint64_t requests[] = { 0, 1, 0x41 };
    const uint64_t replies[] = { 5, 0x52, 0x526 };
    uint64_t reply;

    for (unsigned int i = 0; i < G_N_ELEMENTS(hubs); i++) {
        sn0_fabric_add_endpoint(fabric, &hubs[i], i);
        sn0_fabric_report_number(fabric, i, hubs[i].nasid);
    }
    for (unsigned int i = 0; i < G_N_ELEMENTS(input); i++) {
        DeviceState *device = qdev_new(TYPE_SGI_ROUTER);
        SGIRouterState *router = SGI_ROUTER(device);
        g_autofree char *name = g_strdup_printf("link%u", i);

        qdev_realize(device, NULL, &error_abort);
        device_cold_reset(device);
        g_ptr_array_add(routers, device);
        sn0_fabric_routed_add_router(fabric, router);
        sn0_fabric_routed_add_link(fabric, name, previous,
            (SN0FabricVertex) { SN0_FABRIC_VERTEX_ROUTER, i, input[i] });
        previous = (SN0FabricVertex) {
            SN0_FABRIC_VERTEX_ROUTER, i, output[i],
        };
    }
    sn0_fabric_routed_add_link(fabric, "destination", previous,
        (SN0FabricVertex) { SN0_FABRIC_VERTEX_HUB, 1, 0 });
    qdev_realize(DEVICE(fabric), NULL, &error_abort);
    sn0_fabric_build(fabric);

    /* Reverse the traversed ingress ports, not the request's exit ports. */
    g_assert_cmpint(sn0_fabric_vector(fabric, 0, 0x341, &access), ==,
                    SN0_FABRIC_VECTOR_OK);
    g_assert_cmpuint(access.value, ==, 7);
    g_assert_cmphex(access.reply_vector, ==, 0x526);
    reply = access.reply_vector;
    g_assert_cmpint(sn0_fabric_vector(fabric, 1, reply, &access), ==,
                    SN0_FABRIC_VECTOR_OK);
    g_assert_cmpuint(access.value, ==, 3);
    g_assert_cmphex(access.reply_vector, ==, 0x341);

    /* A Router responder must include its own ingress port in the reply. */
    for (unsigned int i = 0; i < G_N_ELEMENTS(requests); i++) {
        g_assert_cmpint(sn0_fabric_vector(fabric, 0, requests[i], &access), ==,
                        SN0_FABRIC_VECTOR_OK);
        g_assert_cmphex(access.reply_vector, ==, replies[i]);
    }
    /* Refused requests must not expose a previous reply or a partial path. */
    g_assert_cmpint(sn0_fabric_vector(fabric, 0, 0x347, &access), ==,
                    SN0_FABRIC_VECTOR_NONE);
    g_assert_cmphex(access.reply_vector, ==, 0);
    object_property_set_str(OBJECT(fabric), "down-links", "link1",
                            &error_abort);
    g_assert_cmpint(sn0_fabric_vector(fabric, 0, 0x341, &access), ==,
                    SN0_FABRIC_VECTOR_NONE);
    g_assert_cmphex(access.reply_vector, ==, 0);

    object_unparent(OBJECT(fabric));
    object_unref(OBJECT(fabric));
    for (unsigned int i = 0; i < routers->len; i++) {
        object_unparent(g_ptr_array_index(routers, i));
    }
}

int main(int argc, char **argv)
{
    Object *machine;

    g_test_init(&argc, &argv, NULL);
    qemu_init_main_loop(&error_abort);
    module_call_init(MODULE_INIT_QOM);
    machine = object_property_add_new_container(object_get_root(), "machine");
    object_property_add_new_container(machine, "unattached");
    g_test_add_func("/sn0-fabric-routed/vector-reply-path",
                    test_vector_reply_path);
    g_test_add_func("/sn0-fabric-routed/diagnostic-error-paths",
                    test_diagnostic_error_paths);
    g_test_add_data_func("/sn0-fabric-routed/fifteen-hops",
                        GUINT_TO_POINTER(15), test_route_length);
    g_test_add_data_func("/sn0-fabric-routed/sixteen-hops",
                        GUINT_TO_POINTER(16), test_route_length);
    return g_test_run();
}
