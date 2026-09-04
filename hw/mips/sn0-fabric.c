/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The SN0 fabric and its direct adapter.
 *
 * The fabric owns the topology and answers two questions for a Hub's
 * endpoint: which endpoint is reachable as node number d, and what does a
 * vector reach. The direct adapter is the fabric of two Hubs joined by one
 * link -- the Origin 200's CrayLink cable, or a deskside Origin 2000's Null
 * Router: each Hub reaches itself by its own number and the other Hub by
 * the number that Hub has programmed, and a vector of zero names the other
 * Hub while any other vector, which would need a Router, is refused. Two
 * Hubs programmed the same number cannot reach each other; the issuer
 * always wins its own number, so no access is ever served by the wrong Hub.
 *
 * Contract: the routed-fabric design in the Origami project tree
 * (docs/designs/2026-09-03-origin2000-routed-fabric-design.md).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev.h"
#include "migration/vmstate.h"
#include "hw/mips/sn0-fabric.h"
#include "hw/misc/sgi-hub.h"

static SN0FabricEndpoint *sn0_fabric_endpoint(SN0FabricState *f,
                                              unsigned int index)
{
    g_assert(index < f->endpoints->len);
    return &g_array_index(f->endpoints, SN0FabricEndpoint, index);
}

static SN0FabricLink *sn0_fabric_link(SN0FabricState *f, unsigned int index)
{
    g_assert(index < f->links->len);
    return &g_array_index(f->links, SN0FabricLink, index);
}

static bool sn0_fabric_link_down(SN0FabricState *f, unsigned int index)
{
    return index < 64 && (f->down_mask & (1ULL << index));
}

unsigned int sn0_fabric_add_endpoint(SN0FabricState *f, SGIHubState *hub,
                                     uint32_t stride)
{
    SN0FabricEndpoint endpoint = {
        .hub = hub,
        .stride = stride,
        .number = 0,
    };
    unsigned int n;

    g_assert(!f->built);
    for (n = 0; n < SN0_FABRIC_NUMBERS; n++) {
        endpoint.map[n] = SN0_FABRIC_NONE;
    }
    g_array_append_val(f->endpoints, endpoint);
    return f->endpoints->len - 1;
}

void sn0_fabric_add_link(SN0FabricState *f, const char *name,
                         unsigned int a, unsigned int b)
{
    SN0FabricLink link = {
        .name = g_strdup(name),
        .a = a,
        .b = b,
    };

    g_assert(!f->built);
    g_assert(a < f->endpoints->len && b < f->endpoints->len && a != b);
    g_assert(f->links->len < 64);
    g_array_append_val(f->links, link);
}

unsigned int sn0_fabric_endpoint_count(SN0FabricState *f)
{
    return f->endpoints->len;
}

static void sn0_fabric_recompute(SN0FabricState *f)
{
    if (f->built) {
        SN0_FABRIC_GET_CLASS(f)->recompute(f);
    }
}

/* Every endpoint hears a link change, map or not: its status bit moves. */
static void sn0_fabric_notify_all(SN0FabricState *f)
{
    unsigned int n;

    if (!f->built) {
        return;
    }
    for (n = 0; n < f->endpoints->len; n++) {
        sgi_hub_fabric_changed(sn0_fabric_endpoint(f, n)->hub);
    }
}

void sn0_fabric_build(SN0FabricState *f)
{
    f->built = true;
    sn0_fabric_recompute(f);
    sn0_fabric_notify_all(f);
}

/*
 * Install a freshly computed map and tell the Hub only when it changed:
 * every table or number write recomputes, and most change nothing.
 */
static void sn0_fabric_publish(SN0FabricState *f, unsigned int index,
                               const int16_t *map)
{
    SN0FabricEndpoint *endpoint = sn0_fabric_endpoint(f, index);

    if (memcmp(endpoint->map, map, sizeof(endpoint->map)) != 0) {
        memcpy(endpoint->map, map, sizeof(endpoint->map));
        sgi_hub_fabric_changed(endpoint->hub);
    }
}

void sn0_fabric_report_number(SN0FabricState *f, unsigned int endpoint,
                              uint32_t number)
{
    sn0_fabric_endpoint(f, endpoint)->number = number;
    sn0_fabric_recompute(f);
}

const int16_t *sn0_fabric_map(SN0FabricState *f, unsigned int endpoint)
{
    return sn0_fabric_endpoint(f, endpoint)->map;
}

uint32_t sn0_fabric_endpoint_stride(SN0FabricState *f, unsigned int endpoint)
{
    return sn0_fabric_endpoint(f, endpoint)->stride;
}

uint32_t sn0_fabric_endpoint_number(SN0FabricState *f, unsigned int endpoint)
{
    return sn0_fabric_endpoint(f, endpoint)->number;
}

SGIHubState *sn0_fabric_hub_for_number(SN0FabricState *f,
                                       unsigned int endpoint,
                                       uint32_t number)
{
    const int16_t *map = sn0_fabric_map(f, endpoint);

    if (number >= SN0_FABRIC_NUMBERS || map[number] == SN0_FABRIC_NONE) {
        return NULL;
    }
    return sn0_fabric_endpoint(f, map[number])->hub;
}

bool sn0_fabric_link_up(SN0FabricState *f, unsigned int endpoint)
{
    unsigned int n;

    for (n = 0; n < f->links->len; n++) {
        SN0FabricLink *link = sn0_fabric_link(f, n);

        if ((link->a == endpoint || link->b == endpoint) &&
            !sn0_fabric_link_down(f, n)) {
            return true;
        }
    }
    return false;
}

SN0FabricVectorResult sn0_fabric_vector(SN0FabricState *f,
                                        unsigned int endpoint,
                                        uint64_t vector,
                                        SN0FabricVectorAccess *access)
{
    return SN0_FABRIC_GET_CLASS(f)->vector(f, endpoint, vector, access);
}

/* The direct adapter: one link, two endpoints, no Router. */

static int sn0_fabric_direct_peer(SN0FabricState *f, unsigned int endpoint)
{
    unsigned int n;

    for (n = 0; n < f->links->len; n++) {
        SN0FabricLink *link = sn0_fabric_link(f, n);

        if (sn0_fabric_link_down(f, n)) {
            continue;
        }
        if (link->a == endpoint) {
            return link->b;
        }
        if (link->b == endpoint) {
            return link->a;
        }
    }
    return SN0_FABRIC_NONE;
}

static void sn0_fabric_direct_recompute(SN0FabricState *f)
{
    unsigned int index;

    for (index = 0; index < f->endpoints->len; index++) {
        SN0FabricEndpoint *self = sn0_fabric_endpoint(f, index);
        int16_t map[SN0_FABRIC_NUMBERS];
        int peer = sn0_fabric_direct_peer(f, index);
        unsigned int n;

        for (n = 0; n < SN0_FABRIC_NUMBERS; n++) {
            map[n] = SN0_FABRIC_NONE;
        }
        /* Own number first: two Hubs both at zero each keep zero. */
        if (self->number < SN0_FABRIC_NUMBERS) {
            map[self->number] = index;
        }
        if (peer != SN0_FABRIC_NONE) {
            SN0FabricEndpoint *other = sn0_fabric_endpoint(f, peer);

            if (other->number < SN0_FABRIC_NUMBERS &&
                other->number != self->number) {
                map[other->number] = peer;
            }
        }
        sn0_fabric_publish(f, index, map);
    }
}

/*
 * A vector of zero names the directly wired peer; any other route needs a
 * Router this fabric does not have and fails with the address-error
 * response, as the fork answered before this change. With no link up nothing
 * answers at all.
 */
static SN0FabricVectorResult
sn0_fabric_direct_vector(SN0FabricState *f, unsigned int endpoint,
                         uint64_t vector, SN0FabricVectorAccess *access)
{
    int peer = sn0_fabric_direct_peer(f, endpoint);
    SGIHubState *target;
    bool performed;

    if (peer == SN0_FABRIC_NONE) {
        return SN0_FABRIC_VECTOR_NONE;
    }
    target = sn0_fabric_endpoint(f, peer)->hub;
    if (vector != 0) {
        access->source = sgi_hub_programmed_nasid(target);
        return SN0_FABRIC_VECTOR_ERROR;
    }
    performed = sgi_hub_vector_access(target, access);
    /*
     * The reply names the responder as it is when it replies: a write that
     * renumbers the target reports the new number, which is how firmware
     * confirms an assignment took.
     */
    access->source = sgi_hub_programmed_nasid(target);
    return performed ? SN0_FABRIC_VECTOR_OK : SN0_FABRIC_VECTOR_ERROR;
}

static char *sn0_fabric_direct_routes(SN0FabricState *f)
{
    GString *routes = g_string_new(NULL);
    unsigned int from, to;

    for (from = 0; from < f->endpoints->len; from++) {
        for (to = 0; to < f->endpoints->len; to++) {
            bool linked;

            if (from == to) {
                continue;
            }
            linked = sn0_fabric_direct_peer(f, from) == (int)to;
            g_string_append_printf(routes, "%s%u>%u=%s", routes->len ? " " : "",
                                   from, to, linked ? "0x0" : "-");
        }
    }
    return g_string_free(routes, false);
}

/* The properties tests read and write: which links are down, and the routes. */

static char *sn0_fabric_get_down_links(Object *obj, Error **errp)
{
    SN0FabricState *f = SN0_FABRIC(obj);
    GString *names = g_string_new(NULL);
    unsigned int n;

    for (n = 0; n < f->links->len; n++) {
        if (sn0_fabric_link_down(f, n)) {
            g_string_append_printf(names, "%s%s", names->len ? "," : "",
                                   sn0_fabric_link(f, n)->name);
        }
    }
    return g_string_free(names, false);
}

static void sn0_fabric_set_down_links(Object *obj, const char *value,
                                      Error **errp)
{
    SN0FabricState *f = SN0_FABRIC(obj);
    g_auto(GStrv) names = g_strsplit(value, ",", -1);
    uint64_t mask = 0;
    unsigned int i;

    for (i = 0; names[i]; i++) {
        const char *name = g_strstrip(names[i]);
        unsigned int n;

        if (!*name) {
            continue;
        }
        for (n = 0; n < f->links->len; n++) {
            if (!strcmp(sn0_fabric_link(f, n)->name, name)) {
                mask |= 1ULL << n;
                break;
            }
        }
        if (n == f->links->len) {
            error_setg(errp, "the fabric has no link named '%s'", name);
            return;
        }
    }
    f->down_mask = mask;
    sn0_fabric_recompute(f);
    sn0_fabric_notify_all(f);
}

static char *sn0_fabric_get_routes(Object *obj, Error **errp)
{
    SN0FabricState *f = SN0_FABRIC(obj);

    return SN0_FABRIC_GET_CLASS(f)->routes(f);
}

static int sn0_fabric_post_load(void *opaque, int version_id)
{
    sn0_fabric_recompute(opaque);
    sn0_fabric_notify_all(opaque);
    return 0;
}

static const VMStateDescription vmstate_sn0_fabric = {
    .name = "sn0-fabric",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sn0_fabric_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(down_mask, SN0FabricState),
        VMSTATE_END_OF_LIST()
    },
};

static void sn0_fabric_init(Object *obj)
{
    SN0FabricState *f = SN0_FABRIC(obj);

    f->endpoints = g_array_new(false, true, sizeof(SN0FabricEndpoint));
    f->links = g_array_new(false, true, sizeof(SN0FabricLink));
}

static void sn0_fabric_finalize(Object *obj)
{
    SN0FabricState *f = SN0_FABRIC(obj);
    unsigned int n;

    for (n = 0; n < f->links->len; n++) {
        g_free(sn0_fabric_link(f, n)->name);
    }
    g_array_free(f->links, true);
    g_array_free(f->endpoints, true);
}

static void sn0_fabric_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_sn0_fabric;
    dc->user_creatable = false;
    object_class_property_add_str(klass, "down-links",
                                  sn0_fabric_get_down_links,
                                  sn0_fabric_set_down_links);
    object_class_property_set_description(klass, "down-links",
        "The fabric links marked down, by the model's bus identifiers, "
        "separated by commas");
    object_class_property_add_str(klass, "routes", sn0_fabric_get_routes,
                                  NULL);
    object_class_property_set_description(klass, "routes",
        "The route each ordered pair of endpoints takes, as source>destination="
        "vector in the PROM manual's notation, or - when unreachable");
}

static void sn0_fabric_direct_class_init(ObjectClass *klass, const void *data)
{
    SN0FabricClass *fc = SN0_FABRIC_CLASS(klass);

    fc->recompute = sn0_fabric_direct_recompute;
    fc->vector = sn0_fabric_direct_vector;
    fc->routes = sn0_fabric_direct_routes;
}

static const TypeInfo sn0_fabric_types[] = {
    {
        .name = TYPE_SN0_FABRIC,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(SN0FabricState),
        .instance_init = sn0_fabric_init,
        .instance_finalize = sn0_fabric_finalize,
        .class_size = sizeof(SN0FabricClass),
        .class_init = sn0_fabric_class_init,
        .abstract = true,
    },
    {
        .name = TYPE_SN0_FABRIC_DIRECT,
        .parent = TYPE_SN0_FABRIC,
        .class_init = sn0_fabric_direct_class_init,
    },
};

DEFINE_TYPES(sn0_fabric_types)
