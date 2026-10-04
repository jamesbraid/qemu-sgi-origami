/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Writable stores an SGI machine binds through machine properties named
 * for the catalogue's storage items. The SN family and Octane share this,
 * so both refuse an unknown, missing or wrongly sized store the same way.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "block/block.h"
#include "block/graph-lock.h"
#include "hw/core/qdev.h"
#include "hw/mips/sgi/sgi-storage.h"

static void sgi_storage_get(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    GHashTable **bindings = opaque;
    g_autofree char *value = g_strdup(sgi_storage_node(*bindings, name));

    if (!value) {
        value = g_strdup("");
    }
    visit_type_str(v, name, &value, errp);
}

static void sgi_storage_set(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    GHashTable **bindings = opaque;
    char *value;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "%s cannot change once the machine is built", name);
        return;
    }
    if (!visit_type_str(v, name, &value, errp)) {
        return;
    }
    if (!*bindings) {
        *bindings = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, g_free);
    }
    if (*value) {
        g_hash_table_insert(*bindings, g_strdup(name), value);
    } else {
        g_hash_table_remove(*bindings, name);
        g_free(value);
    }
}

void sgi_storage_add_property(Object *obj, GHashTable **bindings,
                              const char *name)
{
    g_autofree char *description = g_strdup_printf(
        "Block node or drive backing %s; it must be exactly the size "
        "query-sgi-machines lists for the configuration", name);

    object_property_add(obj, name, "str", sgi_storage_get, sgi_storage_set,
                        NULL, bindings);
    object_property_set_description(obj, name, description);
}

const char *sgi_storage_node(GHashTable *bindings, const char *name)
{
    return bindings ? g_hash_table_lookup(bindings, name) : NULL;
}

/* @product's storage item @name for the configuration @t builds, or NULL. */
static const SGIModelStorageItem *sgi_storage_item(const char *product,
                                                   const SGIModelTopology *t,
                                                   const char *name)
{
    size_t nodes = QAPI_LIST_LENGTH(t->nodes);
    const SGIModelOfferingList *entry;

    for (entry = sgi_machines()->offerings; entry; entry = entry->next) {
        const SGIModelOffering *offering = entry->value;
        const SGIModelStorageItemList *item;

        if (!g_str_equal(offering->product, product) ||
            !g_str_equal(offering->topology, t->name) ||
            offering->nodes != nodes) {
            continue;
        }
        for (item = offering->storage; item; item = item->next) {
            if (g_str_equal(item->value->name, name)) {
                return item->value;
            }
        }
        return NULL;
    }
    return NULL;
}

static int64_t sgi_storage_length(BlockDriverState *bs)
{
    GRAPH_RDLOCK_GUARD_MAINLOOP();
    return bdrv_getlength(bs);
}

/* g_ptr_array_sort() order for an array of strings. */
static gint sgi_storage_compare_names(gconstpointer a, gconstpointer b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

bool sgi_storage_check(GHashTable *bindings, const char *machine,
                       const char *product, const SGIModelTopology *t,
                       Error **errp)
{
    g_autoptr(GHashTable) claimed = NULL;
    g_autoptr(GPtrArray) names = NULL;
    GHashTableIter iter;
    gpointer key;
    guint i;

    if (!bindings) {
        return true;
    }
    /* In name order, so a refusal names the same items every run. */
    names = g_ptr_array_new();
    g_hash_table_iter_init(&iter, bindings);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        g_ptr_array_add(names, key);
    }
    g_ptr_array_sort(names, sgi_storage_compare_names);
    claimed = g_hash_table_new(NULL, NULL);
    for (i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index(names, i);
        const char *node = g_hash_table_lookup(bindings, name);
        const SGIModelStorageItem *item = sgi_storage_item(product, t, name);
        bool clock = g_str_has_suffix(name, "-clock");
        const char *other;
        BlockDriverState *bs;
        int64_t length;

        if (!item) {
            size_t nodes = QAPI_LIST_LENGTH(t->nodes);

            error_setg(errp, "%s: topology '%s' with %zu node board%s has "
                       "no %s", machine, t->name, nodes,
                       nodes == 1 ? "" : "s", name);
            return false;
        }
        bs = bdrv_lookup_bs(node, node, NULL);
        if (!bs) {
            error_setg(errp, "%s: %s names '%s', which is no block node or "
                       "drive", machine, name, node);
            return false;
        }
        /* Two parts writing one node would corrupt each other. */
        other = g_hash_table_lookup(claimed, bs);
        if (other) {
            error_setg(errp, "%s: %s and %s both name '%s'", machine, other,
                       name, node);
            return false;
        }
        g_hash_table_insert(claimed, bs, (gpointer)name);
        length = sgi_storage_length(bs);
        if (length < 0) {
            error_setg_errno(errp, -length, "%s: cannot read the size of %s",
                             machine, name);
            return false;
        }
        if (clock ? (uint64_t)length < item->size :
                    (uint64_t)length != item->size) {
            error_setg(errp, "%s: %s is %" PRId64 " bytes; it must be %s%"
                       PRIu64, machine, name, length,
                       clock ? "at least " : "exactly ", item->size);
            return false;
        }
    }
    return true;
}
