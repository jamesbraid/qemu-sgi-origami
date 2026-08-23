/*
 * QEMU MIPS CPU (monitor definitions)
 *
 * SPDX-FileCopyrightText: 2012 SUSE LINUX Products GmbH
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "qemu/target-info.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-machine.h"
#include "cpu.h"
#include CONFIG_DEVICES
#ifdef CONFIG_SGI_SN0
#include "hw/mips/sn0.h"
#endif

CpuModelExpansionInfo *
qmp_query_cpu_model_expansion(CpuModelExpansionType type,
                              CpuModelInfo *model,
                              Error **errp)
{
    error_setg(errp, "CPU model expansion is not supported on this target");
    return NULL;
}

static void mips_cpu_add_definition(gpointer data, gpointer user_data)
{
    ObjectClass *oc = data;
    CpuDefinitionInfoList **cpu_list = user_data;
    CpuDefinitionInfo *info;
    const char *typename;

    typename = object_class_get_name(oc);
    info = g_malloc0(sizeof(*info));
    info->name = cpu_model_from_type(typename);
    info->q_typename = g_strdup(typename);

    QAPI_LIST_PREPEND(*cpu_list, info);
}

CpuDefinitionInfoList *qmp_query_cpu_definitions(Error **errp)
{
    CpuDefinitionInfoList *cpu_list = NULL;
    GSList *list;

    list = object_class_get_list(target_cpu_type(), false);
    g_slist_foreach(list, mips_cpu_add_definition, &cpu_list);
    g_slist_free(list);

    return cpu_list;
}

SgiOriginState *qmp_x_query_origin_state(bool has_event_limit,
                                         uint32_t event_limit,
                                         Error **errp)
{
#ifdef CONFIG_SGI_SN0
    if (!has_event_limit) {
        event_limit = 256;
    }
    if (event_limit > 4096) {
        error_setg(errp, "event-limit must not exceed 4096");
        return NULL;
    }
    return sn0_query_origin_state(event_limit, errp);
#else
    error_setg(errp, "SGI Origin state is not supported on this target");
    return NULL;
#endif
}
