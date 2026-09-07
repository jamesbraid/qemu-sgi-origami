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
#include "qapi/qapi-commands-misc-mips.h"
#include "hw/core/cpu.h"
#include "cpu.h"
#include "internal.h"

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

static MIPSCPU *mips_qmp_cpu(int64_t cpu_index, Error **errp)
{
    CPUState *cs = NULL;

    if (cpu_index >= 0 && cpu_index <= INT_MAX) {
        cs = qemu_get_cpu(cpu_index);
    }
    if (!cs) {
        error_setg(errp, "no CPU with index %" PRId64, cpu_index);
        return NULL;
    }
    return MIPS_CPU(cs);
}

void qmp_x_mips_debug_breakpoint(int64_t cpu_index, uint64_t pc,
                                 bool has_asid, uint16_t asid,
                                 bool has_exccode, uint8_t exccode,
                                 bool has_remove, bool remove, Error **errp)
{
    MIPSDebugBreakpoint bp = {
        .pc = pc,
        .asid = asid,
        .exccode = exccode,
        .has_asid = has_asid,
        .has_exccode = has_exccode,
    };
    MIPSCPU *cpu = mips_qmp_cpu(cpu_index, errp);

    if (!cpu) {
        return;
    }
    if (has_asid && (asid & ~cpu->env.CP0_EntryHi_ASID_mask)) {
        error_setg(errp, "ASID 0x%x does not fit CPU %" PRId64 "'s ASID field",
                   asid, cpu_index);
        return;
    }
    if (has_exccode && exccode > 31) {
        error_setg(errp, "ExcCode %u exceeds 31", exccode);
        return;
    }
    mips_debug_breakpoint_update(cpu, &bp, has_remove && remove, errp);
}

MipsDebugBreakpointList *qmp_x_query_mips_debug_breakpoints(Error **errp)
{
    MipsDebugBreakpointList *head = NULL, **tail = &head;
    CPUState *cs;

    CPU_FOREACH(cs) {
        MIPSCPU *cpu = MIPS_CPU(cs);
        int i;

        for (i = 0; i < MIPS_DEBUG_BREAKPOINTS; i++) {
            const MIPSDebugBreakpoint *bp = &cpu->debug_bp[i];
            MipsDebugBreakpoint *info;

            if (!bp->used) {
                continue;
            }
            info = g_new0(MipsDebugBreakpoint, 1);
            info->cpu_index = cs->cpu_index;
            info->pc = bp->pc;
            info->has_asid = bp->has_asid;
            info->asid = bp->asid;
            info->has_exccode = bp->has_exccode;
            info->exccode = bp->exccode;
            QAPI_LIST_APPEND(tail, info);
        }
    }
    return head;
}
