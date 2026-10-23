/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Parse embedded hw/mips/sgi/sgi-machines.json into qapi/sgi-machines.json's
 * SGIMachines once. Retain it for the process lifetime;
 * hw/uefi/var-service-json.c also uses qobject_from_json() and
 * qobject_input_visitor_new() to parse JSON into QAPI types.
 */

#include "qemu/osdep.h"
#include "qapi/clone-visitor.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-sgi-machines.h"
#include "qapi/qapi-types-sgi-machines.h"
#include "qapi/qapi-visit-sgi-machines.h"
#include "qapi/qobject-input-visitor.h"
#include "qapi/util.h"
#include "qobject/qjson.h"
#include "qobject/qobject.h"
#include "qobject/qdict.h"
#include "qobject/qstring.h"
#include "hw/mips/sgi/sgi-machines.h"

/*
 * sgi_machines_json embeds hw/mips/sgi/sgi-machines.json via
 * hw/mips/sgi/meson.build.
 */
#include "sgi-machines-embed.c.inc"

static SGIMachines *machines;

static void sgi_machines_load(void)
{
    QObject *qobj;
    Visitor *v;

    if (machines) {
        return;
    }

    qobj = qobject_from_json(sgi_machines_json, &error_abort);
    v = qobject_input_visitor_new(qobj);
    visit_type_SGIMachines(v, NULL, &machines, &error_fatal);
    visit_free(v);
    qobject_unref(qobj);
}

const SGIMachines *sgi_machines(void)
{
    sgi_machines_load();
    return machines;
}

const SGIModelTopology *sgi_model_find_topology(const char *name)
{
    SGIModelTopologyList *entry;

    sgi_machines_load();
    for (entry = machines->topologies; entry; entry = entry->next) {
        if (!strcmp(entry->value->name, name)) {
            return entry->value;
        }
    }
    return NULL;
}

const char *sgi_model_default_topology(const char *product)
{
    const SGIModelOfferingList *entry;

    sgi_machines_load();
    for (entry = machines->offerings; entry; entry = entry->next) {
        if (!strcmp(entry->value->product, product) &&
            entry->value->default_topology) {
            return entry->value->default_topology;
        }
    }
    return NULL;
}

SGIMachines *qmp_query_sgi_machines(Error **errp)
{
    return QAPI_CLONE(SGIMachines, sgi_machines());
}

/* @t's position named @name, or NULL. */
static const SGIModelGraphicsPosition *sgi_model_graphics_position(
    const SGIModelTopology *t, const char *name)
{
    const SGIModelGraphicsPositionList *entry;

    for (entry = t->graphics ? t->graphics->positions : NULL; entry;
         entry = entry->next) {
        if (g_str_equal(entry->value->name, name)) {
            return entry->value;
        }
    }
    return NULL;
}

/* @position's option with configuration @id, or NULL. */
static const SGIModelGraphicsOption *sgi_model_graphics_option(
    const SGIModelGraphicsPosition *position, const char *id)
{
    const SGIModelGraphicsOptionList *entry;

    for (entry = position->options; entry; entry = entry->next) {
        if (g_str_equal(entry->value->configuration->id, id)) {
            return entry->value;
        }
    }
    return NULL;
}

static const SGIModelGraphicsOption *sgi_model_graphics_default(
    const SGIModelGraphicsPosition *position)
{
    const SGIModelGraphicsOptionList *entry;

    for (entry = position->options; entry; entry = entry->next) {
        if (entry->value->q_default) {
            return entry->value;
        }
    }
    g_assert_not_reached();
}

bool sgi_model_head_fitted(const SGIHeadChoice *choice)
{
    return choice->option->configuration->family !=
           SGI_MODEL_GRAPHICS_FAMILY_NONE;
}

/* Check one heads.<name>=<value> entry; return the option it names. */
static const SGIModelGraphicsOption *sgi_model_check_head(
    const char *machine, const SGIModelTopology *t, const char *name,
    const QObject *value, Error **errp)
{
    const SGIModelGraphicsPosition *position =
        sgi_model_graphics_position(t, name);
    const SGIModelGraphicsOption *option;
    const SGIModelGraphicsPositionList *entry;
    const SGIModelGraphicsOptionList *item;
    const QString *string = qobject_to(QString, value);
    const char *id;
    g_autoptr(GString) list = g_string_new(NULL);

    if (!position) {
        for (entry = t->graphics ? t->graphics->positions : NULL; entry;
             entry = entry->next) {
            g_string_append_printf(list, "%s%s", list->len ? ", " : "",
                                   entry->value->name);
        }
        if (!list->len) {
            error_setg(errp, "%s: topology '%s' has no graphics position "
                       "'%s'; it has no graphics positions", machine,
                       t->name, name);
        } else {
            error_setg(errp, "%s: topology '%s' has no graphics position "
                       "'%s'; its positions are %s", machine, t->name, name,
                       list->str);
        }
        return NULL;
    }
    if (!string) {
        error_setg(errp, "%s: heads.%s must name one graphics id", machine,
                   name);
        return NULL;
    }
    id = qstring_get_str(string);
    if (!position->modelled) {
        error_setg(errp, "%s: topology '%s' cannot fit graphics at %s: %s",
                   machine, t->name, name, position->reason);
        return NULL;
    }
    option = sgi_model_graphics_option(position, id);
    if (!option) {
        for (item = position->options; item; item = item->next) {
            if (item->value->selectable) {
                g_string_append_printf(list, "%s%s", list->len ? ", " : "",
                                       item->value->configuration->id);
            }
        }
        error_setg(errp, "%s: topology '%s' offers no graphics '%s' at %s; "
                   "it offers %s", machine, t->name, id, name, list->str);
        return NULL;
    }
    if (!option->selectable) {
        error_setg(errp, "%s: topology '%s' does not build %s (%s) at %s: %s",
                   machine, t->name, option->configuration->product, id,
                   name, option->refused);
        return NULL;
    }
    return option;
}

static int sgi_model_compare_keys(gconstpointer a, gconstpointer b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

SGIHeadChoice *sgi_model_resolve_heads(const char *machine,
                                       const SGIModelTopology *t,
                                       const QDict *heads, size_t *count,
                                       Error **errp)
{
    const SGIModelGraphicsPositionList *entry;
    g_autoptr(GPtrArray) keys = g_ptr_array_new();
    g_autofree SGIHeadChoice *choices = NULL;
    const SGIHeadChoice *first = NULL;
    const QDictEntry *key;
    size_t n = 0, fitted = 0, i;

    /* Sorted, so one bad command line always gives the same error. */
    for (key = heads ? qdict_first(heads) : NULL; key;
         key = qdict_next(heads, key)) {
        g_ptr_array_add(keys, (gpointer)qdict_entry_key(key));
    }
    g_ptr_array_sort(keys, sgi_model_compare_keys);
    for (i = 0; i < keys->len; i++) {
        const char *name = g_ptr_array_index(keys, i);

        if (!sgi_model_check_head(machine, t, name, qdict_get(heads, name),
                                  errp)) {
            return NULL;
        }
    }

    /* One spare, so a topology with no position still returns non-NULL. */
    choices = g_new0(SGIHeadChoice,
                     (t->graphics ? QAPI_LIST_LENGTH(t->graphics->positions) :
                      0) + 1);
    for (entry = t->graphics ? t->graphics->positions : NULL; entry;
         entry = entry->next) {
        const SGIModelGraphicsPosition *position = entry->value;
        const QString *named;

        if (!position->modelled) {
            continue;
        }
        named = heads ? qobject_to(QString, qdict_get(heads, position->name)) :
                NULL;
        choices[n].position = position;
        choices[n].option = named ?
            sgi_model_graphics_option(position, qstring_get_str(named)) :
            sgi_model_graphics_default(position);
        if (sgi_model_head_fitted(&choices[n])) {
            if (++fitted > t->graphics->max_heads) {
                error_setg(errp, "%s: topology '%s' builds at most %" PRIu32
                           " graphics head(s); %s and %s both select one",
                           machine, t->name, t->graphics->max_heads,
                           first ? first->position->name : position->name,
                           position->name);
                return NULL;
            }
            first = first ? first : &choices[n];
        }
        n++;
    }
    *count = n;
    return g_steal_pointer(&choices);
}
