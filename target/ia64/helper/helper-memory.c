/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/* IA-64 TCG helper ABI adapters for atomic memory operations. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "arch/arch.h"
#include "arch/system.h"
#include "system/memory.h"
#include "exec/target_page.h"

/* A physical-mode load while the array port is active: a modelled line. */
uint64_t HELPER(cache_array_load)(CPUIA64State *env, uint64_t addr,
                                  uint64_t value, uint32_t size)
{
    IA64CPU *cpu = env_archcpu(env);

    if (cpu->ca_lines || cpu->ca_l3) {
        value = ia64_cache_array_load(env, ia64_physical_address(addr), value,
                                      size);
    }
    return value;
}

uint64_t HELPER(cmpxchg)(CPUIA64State *env, uint64_t addr, uint64_t cmp,
                        uint64_t val, uint32_t size)
{
    return ia64_memory_cmpxchg(env, addr, cmp, val, size, GETPC());
}

uint64_t HELPER(cmp8xchg16)(CPUIA64State *env, uint64_t addr, uint64_t cmp,
                           uint64_t val, uint64_t csd)
{
    return ia64_memory_cmp8xchg16(env, addr, cmp, val, csd, GETPC());
}
