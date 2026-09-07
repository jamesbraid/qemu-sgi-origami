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

typedef struct MIPSTlbQuery {
    bool shadow;
    MipsTlbState *state;
} MIPSTlbQuery;

/* Runs on the CPU's own thread, so registers and entries are one snapshot. */
static void mips_tlb_query_work(CPUState *cs, run_on_cpu_data data)
{
    MIPSTlbQuery *q = data.host_ptr;
    CPUMIPSState *env = cpu_env(cs);
    MipsTlbState *state = g_new0(MipsTlbState, 1);
    MipsTlbEntryList **tail = &state->entries;
    uint32_t count = q->shadow ? env->tlb->tlb_in_use : env->tlb->nb_tlb;
    uint32_t i;

    state->cpu_index = cs->cpu_index;
    state->entryhi = env->CP0_EntryHi;
    state->asid = env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask;
    state->index = env->CP0_Index;
    state->wired = env->CP0_Wired;
    state->nb_tlb = env->tlb->nb_tlb;
    state->tlb_in_use = env->tlb->tlb_in_use;
    for (i = 0; i < count; i++) {
        const r4k_tlb_t *tlb = &env->tlb->mmu.r4k.tlb[i];
        MipsTlbEntry *e = g_new0(MipsTlbEntry, 1);

        e->index = i;
        e->vpn2 = tlb->VPN;
        e->asid = tlb->ASID;
        e->global = tlb->G;
        e->page_mask = tlb->PageMask;
        e->pfn0 = tlb->PFN[0];
        e->valid0 = tlb->V0;
        e->dirty0 = tlb->D0;
        e->cache0 = tlb->C0;
        e->uc0 = tlb->UC0;
        e->pfn1 = tlb->PFN[1];
        e->valid1 = tlb->V1;
        e->dirty1 = tlb->D1;
        e->cache1 = tlb->C1;
        e->uc1 = tlb->UC1;
        QAPI_LIST_APPEND(tail, e);
    }
    q->state = state;
}

MipsTlbState *qmp_x_query_mips_tlb(int64_t cpu_index, bool has_shadow,
                                   bool shadow, Error **errp)
{
    MIPSTlbQuery q = { .shadow = has_shadow && shadow };
    MIPSCPU *cpu = mips_qmp_cpu(cpu_index, errp);

    if (!cpu) {
        return NULL;
    }
    switch (cpu->env.cpu_model->mmu_type) {
    case MMU_TYPE_R4000:
    case MMU_TYPE_R10000:
        break;
    default:
        error_setg(errp, "CPU %" PRId64 " has no standard TLB", cpu_index);
        return NULL;
    }
    run_on_cpu(CPU(cpu), mips_tlb_query_work, RUN_ON_CPU_HOST_PTR(&q));
    return q.state;
}
