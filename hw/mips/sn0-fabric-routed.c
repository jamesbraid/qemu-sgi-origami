/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Programmed routing for the physical Origin 2000 fabric. */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/mips/sn0-fabric-routed.h"
#include "hw/misc/sgi-hub.h"
#include "hw/misc/sgi-router.h"

OBJECT_DECLARE_SIMPLE_TYPE(SN0RoutedFabricState, SN0_FABRIC_ROUTED)

typedef struct SN0RoutedLink {
    SN0FabricVertex end[2];
} SN0RoutedLink;

#define SN0_ROUTED_VECTOR_MAX_HOPS 15

struct SN0RoutedFabricState {
    SN0FabricState parent_obj;
    GPtrArray *routers;
    GArray *links;
};

static bool sn0_vertex_equal(SN0FabricVertex a, SN0FabricVertex b)
{
    return a.kind == b.kind && a.index == b.index && a.port == b.port;
}

static bool sn0_routed_vertex_enabled(SN0RoutedFabricState *s,
                                       SN0FabricVertex vertex)
{
    return vertex.kind == SN0_FABRIC_VERTEX_HUB ||
        sgi_router_port_enabled(g_ptr_array_index(s->routers, vertex.index),
                                  vertex.port);
}

static bool sn0_routed_neighbor(SN0FabricState *f, SN0FabricVertex from,
                                 SN0FabricVertex *to)
{
    SN0RoutedFabricState *s = SN0_FABRIC_ROUTED(f);
    unsigned int i;

    for (i = 0; i < s->links->len; i++) {
        SN0RoutedLink *link = &g_array_index(s->links, SN0RoutedLink, i);
        unsigned int side;

        if (!sn0_fabric_link_enabled(f, i) ||
            !sn0_routed_vertex_enabled(s, link->end[0]) ||
            !sn0_routed_vertex_enabled(s, link->end[1])) {
            continue;
        }
        for (side = 0; side < 2; side++) {
            if (sn0_vertex_equal(from, link->end[side])) {
                *to = link->end[side ^ 1];
                return true;
            }
        }
    }
    return false;
}

static bool sn0_routed_link_up(SN0FabricState *f, unsigned int endpoint)
{
    SN0FabricVertex hub = { SN0_FABRIC_VERTEX_HUB, endpoint, 0 };
    SN0FabricVertex other;

    return sn0_routed_neighbor(f, hub, &other);
}

/* The carried turn is part of the walk state, not just the ingress port. */
static int sn0_routed_walk(SN0FabricState *f, unsigned int source,
                           uint32_t destination, uint64_t *vector)
{
    SN0RoutedFabricState *s = SN0_FABRIC_ROUTED(f);
    SN0FabricEndpoint *self =
        &g_array_index(f->endpoints, SN0FabricEndpoint, source);
    SN0FabricVertex vertex = { SN0_FABRIC_VERTEX_HUB, source, 0 };
    g_autofree uint16_t *visited = g_new0(uint16_t,
                                         s->routers->len * SGI_ROUTER_PORTS);
    uint8_t turn = 0;
    unsigned int hops = 0;

    *vector = 0;
    if (!sn0_routed_neighbor(f, vertex, &vertex)) {
        return SN0_FABRIC_NONE;
    }
    if (vertex.kind == SN0_FABRIC_VERTEX_ROUTER &&
        !sgi_hub_first_turn(self->hub, destination, &turn)) {
        return SN0_FABRIC_NONE;
    }
    while (vertex.kind == SN0_FABRIC_VERTEX_ROUTER) {
        SGIRouterState *router = g_ptr_array_index(s->routers, vertex.index);
        unsigned int key = vertex.index * SGI_ROUTER_PORTS + vertex.port - 1;
        unsigned int exit_port;
        uint8_t next_turn;

        if (hops == 15) {
            qemu_log_mask(LOG_UNIMP,
                          "sn0-fabric: refusing programmed route over 15 hops\n");
            return SN0_FABRIC_NONE;
        }
        if (turn > 15 || (visited[key] & (1U << turn))) {
            return SN0_FABRIC_NONE;
        }
        visited[key] |= 1U << turn;
        if (!sgi_router_route(router, vertex.port, turn, destination,
                               &exit_port, &next_turn)) {
            return SN0_FABRIC_NONE;
        }
        *vector |= (uint64_t)exit_port << (4 * hops);
        hops++;
        vertex.port = exit_port;
        turn = next_turn;
        if (!sn0_routed_neighbor(f, vertex, &vertex)) {
            return SN0_FABRIC_NONE;
        }
    }
    if (sn0_fabric_endpoint_number(f, vertex.index) != destination) {
        return SN0_FABRIC_NONE;
    }
    return vertex.index;
}

/*
 * Every number a Hub holds is routed for, not just a number exactly one Hub
 * holds. Firmware numbers the nodes one at a time, so for most of bring-up
 * several Hubs still carry the reset number zero, and a Hub that has taken
 * its real number has to keep reaching zero: its own relocated firmware text
 * and its exception vectors are node-field-zero addresses beyond the cached
 * alias, and with no route for zero they fall to the claim error floor and
 * fetch nothing. The walk ends at whichever Hub the programmed turns lead to
 * and already refuses an endpoint whose number is not the one asked for, so
 * a duplicate resolves the way the routes resolve it, as it does in the
 * network. A number no Hub holds is still nobody's, and skipping it keeps
 * the recompute short.
 */
static void sn0_routed_recompute(SN0FabricState *f)
{
    SN0RoutedFabricState *s = SN0_FABRIC_ROUTED(f);
    unsigned int counts[SN0_FABRIC_NUMBERS] = { 0 };
    unsigned int i, port;

    for (i = 0; i < s->routers->len; i++) {
        SGIRouterState *router = g_ptr_array_index(s->routers, i);

        for (port = 1; port <= SGI_ROUTER_PORTS; port++) {
            SN0FabricVertex vertex = { SN0_FABRIC_VERTEX_ROUTER, i, port };
            SN0FabricVertex other;

            sgi_router_set_port_link(router, port,
                                      sn0_routed_neighbor(f, vertex, &other),
                                      sgi_router_port_eight_bit_forced(router,
                                                                        port));
        }
    }
    for (i = 0; i < f->endpoints->len; i++) {
        uint32_t number = sn0_fabric_endpoint_number(f, i);

        if (number < SN0_FABRIC_NUMBERS) {
            counts[number]++;
        }
    }
    for (i = 0; i < f->endpoints->len; i++) {
        int16_t map[SN0_FABRIC_NUMBERS];
        uint32_t own = sn0_fabric_endpoint_number(f, i);
        unsigned int number;

        for (number = 0; number < SN0_FABRIC_NUMBERS; number++) {
            uint64_t vector;

            map[number] = SN0_FABRIC_NONE;
            if (number == own) {
                map[number] = i;
            } else if (counts[number] != 0) {
                map[number] = sn0_routed_walk(f, i, number, &vector);
            }
        }
        sn0_fabric_publish(f, i, map);
    }
}

static bool sn0_routed_reply_append(uint64_t *reply, unsigned int port)
{
    if (*reply > (UINT64_MAX >> 4)) {
        qemu_log_mask(LOG_UNIMP,
                      "sn0-fabric: refusing overflowing vector reply path\n");
        return false;
    }
    *reply = (*reply << 4) | port;
    return true;
}

static void sn0_routed_record_reply_error(SN0FabricState *f,
                                          SN0RoutedFabricState *s,
                                          const SN0FabricVertex *path,
                                          unsigned int path_length,
                                          SN0FabricVectorAccess *access)
{
    unsigned int i;

    if (access->op != SN0_FABRIC_VECTOR_READ) {
        return;
    }
    for (i = path_length; i-- > 1;) {
        SGIRouterState *router;
        SN0FabricVertex receiver;

        if (path[i].kind != SN0_FABRIC_VERTEX_ROUTER) {
            continue;
        }
        router = g_ptr_array_index(s->routers, path[i].index);
        if (sgi_router_port_sends_data_error(router, path[i].port)) {
            if (!sn0_routed_neighbor(f, path[i], &receiver)) {
                continue;
            }
            if (receiver.kind == SN0_FABRIC_VERTEX_ROUTER) {
                sgi_router_record_port_error(
                    g_ptr_array_index(s->routers, receiver.index),
                    receiver.port, SGI_ROUTER_PORT_CHECK_ERROR);
            } else {
                SGIHubState *hub =
                    g_array_index(f->endpoints, SN0FabricEndpoint,
                                  receiver.index).hub;

                sgi_hub_record_port_check_error(hub);
            }
        }
    }
}

static SN0FabricVectorResult
sn0_routed_vector(SN0FabricState *f, unsigned int endpoint, uint64_t vector,
                   SN0FabricVectorAccess *access)
{
    SN0RoutedFabricState *s = SN0_FABRIC_ROUTED(f);
    SGIHubState *source =
        g_array_index(f->endpoints, SN0FabricEndpoint, endpoint).hub;
    SN0FabricVertex vertex = { SN0_FABRIC_VERTEX_HUB, endpoint, 0 };
    SN0FabricVertex path[SN0_ROUTED_VECTOR_MAX_HOPS + 2] = { vertex };
    unsigned int path_length = 1;
    unsigned int hops = 0;
    uint64_t reply = 0;

    if (!sn0_routed_neighbor(f, vertex, &vertex)) {
        return SN0_FABRIC_VECTOR_NONE;
    }
    path[path_length++] = vertex;
    if (access->op == SN0_FABRIC_VECTOR_WRITE &&
        sgi_hub_sends_data_error(source) &&
        vertex.kind == SN0_FABRIC_VERTEX_ROUTER) {
        sgi_router_record_port_error(
            g_ptr_array_index(s->routers, vertex.index), vertex.port,
            SGI_ROUTER_PORT_CHECK_ERROR);
    }
    while (vector) {
        unsigned int port = vector & 0xf;

        if (vertex.kind != SN0_FABRIC_VERTEX_ROUTER || !port ||
            port > SGI_ROUTER_PORTS ||
            hops == SN0_ROUTED_VECTOR_MAX_HOPS) {
            qemu_log_mask(LOG_UNIMP,
                          "sn0-fabric: refusing unsupported vector path\n");
            return SN0_FABRIC_VECTOR_NONE;
        }
        if (!sn0_routed_reply_append(&reply, vertex.port)) {
            return SN0_FABRIC_VECTOR_NONE;
        }
        vertex.port = port;
        if (!sn0_routed_neighbor(f, vertex, &vertex)) {
            return SN0_FABRIC_VECTOR_NONE;
        }
        path[path_length++] = vertex;
        vector >>= 4;
        hops++;
    }
    if (vertex.kind == SN0_FABRIC_VERTEX_HUB) {
        SGIHubState *hub =
            g_array_index(f->endpoints, SN0FabricEndpoint, vertex.index).hub;
        bool performed = sgi_hub_vector_access(hub, access);

        access->source = sgi_hub_programmed_nasid(hub);
        access->reply_vector = reply;
        if (performed) {
            sn0_routed_record_reply_error(f, s, path, path_length,
                                          access);
        }
        return performed ? SN0_FABRIC_VECTOR_OK : SN0_FABRIC_VECTOR_ERROR;
    }
    /* A Router responder sends back through the port the request entered. */
    if (!sn0_routed_reply_append(&reply, vertex.port)) {
        return SN0_FABRIC_VECTOR_NONE;
    }
    if (sgi_router_access(g_ptr_array_index(s->routers, vertex.index),
                           vertex.port, access)) {
        /* Reviewed PROM 6.156 policy; not a physical Router identity. */
        access->source = 0;
        access->reply_vector = reply;
        sn0_routed_record_reply_error(f, s, path, path_length, access);
        return SN0_FABRIC_VECTOR_OK;
    }
    qemu_log_mask(LOG_UNIMP,
                  "sn0-fabric: refusing unsupported Router vector operation\n");
    return SN0_FABRIC_VECTOR_NONE;
}

static char *sn0_routed_routes(SN0FabricState *f)
{
    GString *result = g_string_new(NULL);
    unsigned int from, to;

    for (from = 0; from < f->endpoints->len; from++) {
        for (to = 0; to < f->endpoints->len; to++) {
            uint32_t number = sn0_fabric_endpoint_number(f, to);
            uint64_t vector;
            bool reachable;

            if (from == to) {
                continue;
            }
            reachable = number < SN0_FABRIC_NUMBERS &&
                sn0_fabric_map(f, from)[number] == to &&
                sn0_routed_walk(f, from, number, &vector) == to;
            g_string_append_printf(result, "%s%u>%u=", result->len ? " " : "",
                                     from, to);
            if (reachable) {
                g_string_append_printf(result, "0x%" PRIx64, vector);
            } else {
                g_string_append_c(result, '-');
            }
        }
    }
    return g_string_free(result, false);
}

static void sn0_routed_changed(void *opaque)
{
    sn0_fabric_recompute(opaque);
    /* A physical link can change without changing a programmed route. */
    sn0_fabric_notify_all(opaque);
}

unsigned int sn0_fabric_routed_add_router(SN0FabricState *f,
                                         SGIRouterState *router)
{
    SN0RoutedFabricState *s = SN0_FABRIC_ROUTED(f);

    g_assert(!f->built);
    object_ref(OBJECT(router));
    g_ptr_array_add(s->routers, router);
    sgi_router_set_changed(router, sn0_routed_changed, f);
    return s->routers->len - 1;
}

static void sn0_routed_check_vertex(SN0RoutedFabricState *s,
                                    SN0FabricVertex vertex)
{
    SN0FabricState *f = SN0_FABRIC(s);

    if (vertex.kind == SN0_FABRIC_VERTEX_HUB) {
        g_assert(vertex.index < f->endpoints->len && vertex.port == 0);
    } else {
        g_assert(vertex.kind == SN0_FABRIC_VERTEX_ROUTER);
        g_assert(vertex.index < s->routers->len);
        g_assert(vertex.port >= 1 && vertex.port <= SGI_ROUTER_PORTS);
    }
}

void sn0_fabric_routed_add_link(SN0FabricState *f, const char *name,
                               SN0FabricVertex a, SN0FabricVertex b)
{
    SN0RoutedFabricState *s = SN0_FABRIC_ROUTED(f);
    SN0RoutedLink link = { { a, b } };
    unsigned int i;

    sn0_routed_check_vertex(s, a);
    sn0_routed_check_vertex(s, b);
    g_assert(!sn0_vertex_equal(a, b));
    for (i = 0; i < s->links->len; i++) {
        SN0RoutedLink *other = &g_array_index(s->links, SN0RoutedLink, i);
        unsigned int side;

        for (side = 0; side < 2; side++) {
            g_assert(!sn0_vertex_equal(a, other->end[side]));
            g_assert(!sn0_vertex_equal(b, other->end[side]));
        }
    }
    g_assert(sn0_fabric_add_named_link(f, name) == s->links->len);
    g_array_append_val(s->links, link);
}

static void sn0_routed_init(Object *obj)
{
    SN0RoutedFabricState *s = SN0_FABRIC_ROUTED(obj);

    s->routers = g_ptr_array_new_with_free_func(object_unref);
    s->links = g_array_new(false, false, sizeof(SN0RoutedLink));
}

static void sn0_routed_finalize(Object *obj)
{
    SN0RoutedFabricState *s = SN0_FABRIC_ROUTED(obj);

    unsigned int i;

    for (i = 0; i < s->routers->len; i++) {
        sgi_router_set_changed(g_ptr_array_index(s->routers, i), NULL, NULL);
    }
    g_ptr_array_free(s->routers, true);
    g_array_free(s->links, true);
}

static void sn0_routed_class_init(ObjectClass *klass, const void *data)
{
    SN0FabricClass *fc = SN0_FABRIC_CLASS(klass);

    fc->link_up = sn0_routed_link_up;
    fc->recompute = sn0_routed_recompute;
    fc->vector = sn0_routed_vector;
    fc->routes = sn0_routed_routes;
}

static const TypeInfo sn0_routed_type = {
    .name = TYPE_SN0_FABRIC_ROUTED,
    .parent = TYPE_SN0_FABRIC,
    .instance_size = sizeof(SN0RoutedFabricState),
    .instance_init = sn0_routed_init,
    .instance_finalize = sn0_routed_finalize,
    .class_init = sn0_routed_class_init,
};

static void sn0_routed_register_types(void)
{
    type_register_static(&sn0_routed_type);
}

type_init(sn0_routed_register_types)
