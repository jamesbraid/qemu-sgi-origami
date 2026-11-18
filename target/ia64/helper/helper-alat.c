/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/* IA-64 TCG helper ABI adapters for the Advanced Load Address Table. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "arch/arch.h"
#include "arch/system.h"
#include "exec-access.h"

uint64_t HELPER(chk_a)(CPUIA64State *env, uint64_t va, uint32_t reg)
{
    return ia64_alat_chk_a(env, va, reg);
}

void HELPER(invala)(CPUIA64State *env)
{
    ia64_alat_invala(env);
}

uint64_t HELPER(alat_load_begin)(CPUIA64State *env)
{
    return ia64_alat_load_begin(env);
}

void HELPER(set_alat)(CPUIA64State *env, uint32_t reg, uint64_t addr,
                     uint32_t size, uint64_t generation)
{
    ia64_alat_set(env, reg, addr, size, generation);
}

void HELPER(set_alat_fp)(CPUIA64State *env, uint32_t reg, uint64_t addr,
                        uint32_t size, uint64_t generation)
{
    ia64_alat_set_fp(env, reg, addr, size, generation);
}

void HELPER(set_alat_pa)(CPUIA64State *env, uint32_t reg, uint64_t addr,
                        uint32_t size, uint64_t generation, uint64_t pa)
{
    ia64_alat_set_pa(env, reg & 127, addr, size, generation, pa, reg & 128);
}

void HELPER(invalidate_alat_reg)(CPUIA64State *env, uint32_t reg)
{
    ia64_alat_invalidate_reg(env, reg);
}

void HELPER(invalidate_alat_fp_reg)(CPUIA64State *env, uint32_t reg)
{
    ia64_alat_invalidate_fp_reg(env, reg);
}

uint64_t HELPER(check_load_alat)(CPUIA64State *env, uint32_t reg, uint32_t clear)
{
    return ia64_alat_check_load(env, reg, clear);
}

uint64_t HELPER(check_load_alat_addr)(CPUIA64State *env, uint32_t reg,
                                     uint64_t addr, uint32_t size,
                                     uint32_t clear)
{
    return ia64_alat_check_load_addr(env, reg, addr, size, clear);
}

uint64_t HELPER(check_load_alat_fp)(CPUIA64State *env, uint32_t reg,
                                   uint32_t clear)
{
    return ia64_alat_check_load_fp(env, reg, clear);
}

uint64_t HELPER(check_load_alat_fp_addr)(CPUIA64State *env, uint32_t reg,
                                        uint64_t addr, uint32_t size,
                                        uint32_t clear)
{
    return ia64_alat_check_load_fp_addr(env, reg, addr, size, clear);
}

void HELPER(alat_write_begin)(CPUIA64State *env, uint64_t addr, uint32_t size)
{
    ia64_alat_write_begin(env, addr, size);
}

void HELPER(alat_write_end)(CPUIA64State *env, uint64_t addr, uint32_t size)
{
    ia64_alat_write_end(env, addr, size);
    /* The port sees physical-mode stores only, as the load hook does. */
    if (env_archcpu(env)->ca_mirror && !(env->psr & IA64_PSR_DT)) {
        ia64_cache_array_store(env, ia64_physical_address(addr));
    }
}

void HELPER(alat_write_cancel)(CPUIA64State *env)
{
    ia64_alat_write_cancel(env);
}

void HELPER(notify_alat_store)(CPUIA64State *env)
{
    ia64_alat_notify_store(env);
}

void HELPER(alat_ensure_exclusive)(CPUIA64State *env)
{
    ia64_exec_ensure_alat_exclusive(env, GETPC());
}
