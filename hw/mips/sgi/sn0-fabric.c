/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The SN0 fabric base class.
 *
 * The fabric owns the topology and answers two questions for a Hub's
 * endpoint: which endpoint is reachable as node number d, and what does a
 * vector reach. Every product builds the routed subclass
 * (sn0-fabric-routed.c), which answers both questions the same way whether
 * the graph holds real Routers or none: a two-Hub topology with no Router
 * (the Origin 200's CrayLink cable, or a deskside Origin 2000's Null
 * Router) walks a zero-router graph of one link, so this base class no
 * longer needs a second, direct-wired subclass of its own.
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

unsigned int sn0_fabric_add_named_link(SN0FabricState *f, const char *name)
{
    SN0FabricLink link = {
        .name = g_strdup(name),
    };

    g_assert(!f->built);
    g_assert(f->links->len < 64);
    g_array_append_val(f->links, link);
    return f->links->len - 1;
}

bool sn0_fabric_link_enabled(SN0FabricState *f, unsigned int index)
{
    g_assert(index < f->links->len);
    return !sn0_fabric_link_down(f, index);
}

unsigned int sn0_fabric_endpoint_count(SN0FabricState *f)
{
    return f->endpoints->len;
}

void sn0_fabric_recompute(SN0FabricState *f)
{
    if (f->built) {
        SN0_FABRIC_GET_CLASS(f)->recompute(f);
    }
}

/* Every endpoint hears a link change, map or not: its status bit moves. */
void sn0_fabric_notify_all(SN0FabricState *f)
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
void sn0_fabric_publish(SN0FabricState *f, unsigned int index,
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
    return SN0_FABRIC_GET_CLASS(f)->link_up(f, endpoint);
}

SN0FabricVectorResult sn0_fabric_vector(SN0FabricState *f,
                                        unsigned int endpoint,
                                        uint64_t vector,
                                        SN0FabricVectorAccess *access)
{
    access->reply_vector = 0;
    return SN0_FABRIC_GET_CLASS(f)->vector(f, endpoint, vector, access);
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
};

DEFINE_TYPES(sn0_fabric_types)
