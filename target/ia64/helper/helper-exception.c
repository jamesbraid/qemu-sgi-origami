/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/* IA-64 TCG helper ABI adapters for exception return. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "arch/arch.h"

void HELPER(reserved_noop)(CPUIA64State *env, uint64_t ip, uint32_t slot,
                           uint64_t word)
{
    ia64_reserved_noop(env, ip, slot, word);
}

void HELPER(reserved_b_rfi)(CPUIA64State *env, uint64_t ip, uint32_t slot)
{
    ia64_reserved_noop(env, ip, slot, IA64_PAL_RESERVED_B);
    ia64_rfi_reserved_b(env, ip, slot);
}

void HELPER(raise_exception)(CPUIA64State *env, uint32_t exception,
                            uint64_t fault_ip, uint64_t fault_imm,
                            uint32_t fault_slot)
{
    ia64_raise_exception(env, exception, fault_ip, fault_imm, fault_slot);
}

void HELPER(check_native_traps)(CPUIA64State *env, uint64_t target_ip,
                               uint64_t source_ip, uint32_t slots,
                               uint32_t conditions, uint64_t base_isr,
                               uint64_t trap_psr)
{
    ia64_check_native_traps(env, target_ip, source_ip, slots, conditions,
                            base_isr, trap_psr);
}

void HELPER(raise_unaligned)(CPUIA64State *env, uint64_t addr,
                            uint64_t isr_access, uint64_t fault_info)
{
    ia64_raise_unaligned(env, addr, isr_access, fault_info);
}

void HELPER(raise_nat_consumption)(CPUIA64State *env, uint64_t isr_access,
                                  uint64_t fault_info)
{
    ia64_raise_nat_consumption(env, isr_access, fault_info);
}

void HELPER(rfi)(CPUIA64State *env, uint64_t fault_ip, uint32_t fault_slot)
{
    ia64_rfi(env, fault_ip, fault_slot);
}
