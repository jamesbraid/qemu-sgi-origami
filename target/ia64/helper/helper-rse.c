/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/* IA-64 TCG helper ABI adapters for the Register Stack Engine. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "arch/arch.h"
#include "trace.h"

void HELPER(br_call_rse)(CPUIA64State *env, uint32_t b_reg, uint64_t next_ip,
                        uint64_t target)
{
    ia64_rse_br_call(env, b_reg, next_ip, target);
    /* The caller's outputs are r32 upward in the callee's frame. */
    trace_ia64_indirect_call(env_cpu(env)->cpu_index, next_ip - 16, target,
                             env->gr[32], env->gr[33], env->gr[34],
                             env->gr[35]);
}

void HELPER(br_ia)(CPUIA64State *env, uint32_t b_reg, uint64_t fault_ip,
                  uint32_t fault_slot)
{
    ia64_rse_br_ia(env, b_reg, fault_ip, fault_slot);
}

void HELPER(br_ret_rse)(CPUIA64State *env, uint32_t b_reg, uint64_t source_ip,
                       uint32_t source_slot)
{
    uint64_t target = env->br[b_reg & 7];

    ia64_rse_br_ret(env, b_reg, source_ip, source_slot);
    trace_ia64_br_ret(env_cpu(env)->cpu_index, source_ip, target,
                      env->gr[8], env->gr[9], env->gr[10], env->gr[11]);
}

void HELPER(alloc_rse)(CPUIA64State *env, uint32_t r1, uint32_t pfm,
                      uint64_t fault_ip, uint32_t slot)
{
    ia64_rse_alloc(env, r1, pfm, fault_ip, slot, GETPC());
}

void HELPER(cover_rse)(CPUIA64State *env)
{
    ia64_rse_cover(env);
}

void HELPER(flushrs_rse)(CPUIA64State *env)
{
    ia64_rse_flush(env, GETPC());
}

void HELPER(loadrs_rse)(CPUIA64State *env, uint64_t fault_ip, uint64_t raw,
                       uint32_t slot)
{
    ia64_rse_load(env, fault_ip, raw, slot, GETPC());
}

uint32_t HELPER(br_cexit)(CPUIA64State *env)
{
    return ia64_rse_br_cexit(env);
}

uint32_t HELPER(br_ctop)(CPUIA64State *env)
{
    return ia64_rse_br_ctop(env);
}

uint32_t HELPER(br_wexit)(CPUIA64State *env, uint32_t qp)
{
    return ia64_rse_br_wexit(env, qp);
}

uint32_t HELPER(br_wtop)(CPUIA64State *env, uint32_t qp)
{
    return ia64_rse_br_wtop(env, qp);
}

void HELPER(clrrrb_rse)(CPUIA64State *env, uint32_t predicate_only)
{
    ia64_rse_clrrrb(env, predicate_only);
}

uint64_t HELPER(cloop_fill_st1)(CPUIA64State *env, uint32_t base_reg,
                               uint64_t value, uint32_t mmu_idx,
                               uint32_t max_stores)
{
    return ia64_rse_cloop_fill_st1(env, base_reg, value, mmu_idx, max_stores,
                                   GETPC());
}
