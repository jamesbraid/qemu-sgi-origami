/*
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 CPU QOM and execution-engine glue.
 *
 * Instruction decoding, family generators, and architectural helper logic
 * live in decode/, translate/, and arch/ respectively.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/qemu-print.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "cpu.h"
#include "arch/arch.h"
#include "arch/system.h"
#include "debug.h"
#include "exec-access.h"
#include "translate/translate.h"
#include "exec/cputlb.h"
#include "exec/cpu-common.h"
#include "exec/tlb-flags.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "hw/core/boards.h"
#include "hw/core/sysemu-cpu-ops.h"
#include "hw/core/qdev-properties.h"
#include "accel/tcg/cpu-ops.h"
#include "accel/tcg/cpu-loop.h"
#include "tcg/debug-assert.h"
#include "exec/translator.h"
#include "exec/helper-proto.h"
#include "system/cpus.h"
#include "system/runstate.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/physmem.h"
#include "system/qtest.h"
#include "system/tcg.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

static void ia64_cpu_set_pc(CPUState *cs, vaddr value)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    cpu->env.ip = value;
}

static vaddr ia64_cpu_get_pc(CPUState *cs)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    return cpu->env.ip;
}

static TCGTBCPUState ia64_get_tb_cpu_state(CPUState *cs)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    uint64_t psr = cpu->env.psr;

    uint32_t flags =
        ((uint32_t)psr &
         (IA64_TB_FLAG_BE | IA64_TB_FLAG_PSR_IC | IA64_TB_FLAG_DT)) |
        ((psr >> (IA64_PSR_CPL_SHIFT - IA64_TB_FLAG_CPL_SHIFT)) &
         (IA64_TB_FLAG_CPL_MASK | IA64_TB_FLAG_IT | IA64_TB_FLAG_RI_MASK)) |
        ((uint32_t)cpu->env.instruction_group_start << 7);

    flags |= (psr & IA64_PSR_FAULT_SUPPRESS_MASK) != 0
                 ? IA64_TB_FLAG_PSR_SUPPRESS
                 : 0;
    flags |= (psr & IA64_PSR_TB) ? IA64_TB_FLAG_PSR_TB : 0;
    flags |= (psr & IA64_PSR_SS) ? IA64_TB_FLAG_PSR_SS : 0;
    flags |= (psr & IA64_PSR_DB) ? IA64_TB_FLAG_PSR_DB : 0;
    flags |=
        cpu->env.exception_state.psr_i_deferred ? IA64_TB_FLAG_IRQ_DEFER : 0;
    if (likely((cpu->env.nat[0] | cpu->env.nat[1]) == 0)) {
        flags |= IA64_TB_FLAG_NAT_CLEAR;
    }

    return (TCGTBCPUState) {
        .pc = cpu->env.ip,
        .flags = flags,
    };
}

void ia64_tlb_bump_generation(CPUIA64State *env, bool is_ifetch)
{
    IA64MicroTlbEntry *micro =
        is_ifetch ? env->mmu.tlb_inst_micro : env->mmu.tlb_data_micro;
    uint32_t *generation = is_ifetch ? &env->mmu.tlb_inst_generation
                                     : &env->mmu.tlb_data_generation;

    (*generation)++;
    if (*generation == 0) {
        IA64MicroTlbEntry *victims =
            is_ifetch ? env->mmu.tlb_inst_victim : env->mmu.tlb_data_victim;

        *generation = 1;
        memset(micro, 0, sizeof(*micro) * IA64_MICRO_TLB_SIZE);
        memset(victims, 0, sizeof(*victims) * IA64_MICRO_TLB_VICTIM_SIZE);
    }
}

void ia64_tlb_bump_slot_generation(CPUIA64State *env, bool is_ifetch,
                                   uint16_t slot)
{
    IA64TlbEntry *tlb = is_ifetch ? env->mmu.tlb_inst : env->mmu.tlb_data;

    g_assert(slot < IA64_TLB_MAX);
    if (++tlb[slot].micro_generation == 0) {
        /*
         * A wrapped slot version could validate a very old hint.  Make the
         * wrap unambiguous by invalidating all hints before reusing version
         * one.  This path requires over four billion changes to one slot.
         */
        tlb[slot].micro_generation = 1;
        ia64_tlb_bump_generation(env, is_ifetch);
    }
}

const IA64TlbEntry *ia64_tlb_find_slow(CPUIA64State *env, uint64_t va,
                                       uint32_t rid, bool is_ifetch)
{
    IA64TlbEntry *tlb = is_ifetch ? env->mmu.tlb_inst : env->mmu.tlb_data;
    IA64MicroTlbEntry *micro =
        is_ifetch ? env->mmu.tlb_inst_micro : env->mmu.tlb_data_micro;
    uint16_t tlb_count =
        is_ifetch ? env->mmu.tlb_inst_count : env->mmu.tlb_data_count;
    uint32_t generation =
        is_ifetch ? env->mmu.tlb_inst_generation : env->mmu.tlb_data_generation;
    uint16_t i;

    /*
     * Merced's DTLB1 and DTLB2 are non-inclusive.  Although DTLB1 is not
     * architecturally enumerated as additional TR/TC storage, an entry
     * cached there continues to translate accesses after its DTLB2 source
     * has been replaced.  The host softmmu TLB is only an optimization and
     * may be flushed independently, so it cannot stand in for this lookup.
     */
    if (!is_ifetch && ia64_env_cpu_class(env)->model == IA64_CPU_MODEL_MERCED) {
        int cached = ia64_merced_dtlb1_lookup(env, va, rid);

        if (cached >= 0) {
            return &env->mmu.tlb_data_l1[cached];
        }
        for (i = 0; i < IA64_DTLB1_MAX; i++) {
            IA64TlbEntry *entry = &env->mmu.tlb_data_l1[i];

            if (ia64_tlb_match(entry, va, rid)) {
                return entry;
            }
        }
    }

    for (i = 0; i < tlb_count; i++) {
        IA64TlbEntry *entry = &tlb[i];

        /*
         * Main TLB entries are validated when inserted or loaded, so a
         * valid entry always has a nonzero page size.  Check the more
         * selective RID before valid: active operating-system TLBs are
         * dense, while a substantial fraction belongs to another RID.
         */
        if (entry->rid == rid && entry->valid &&
            ((va ^ entry->va) & entry->page_mask) == 0) {
            IA64MicroTlbEntry *cached = &micro[ia64_micro_tlb_index(va, rid)];

            if (cached->valid && cached->generation == generation) {
                IA64MicroTlbEntry *victims = is_ifetch
                                                 ? env->mmu.tlb_inst_victim
                                                 : env->mmu.tlb_data_victim;
                uint8_t *next = is_ifetch ? &env->mmu.tlb_inst_victim_next
                                          : &env->mmu.tlb_data_victim_next;

                victims[*next] = *cached;
                *next = (*next + 1) % IA64_MICRO_TLB_VICTIM_SIZE;
            }
            *cached = (IA64MicroTlbEntry) {
                .va = entry->va,
                .page_mask = entry->page_mask,
                .pte = entry->pte,
                .rid = entry->rid,
                .generation = generation,
                .slot_generation = entry->micro_generation,
                .slot = i,
                .valid = true,
            };
            return entry;
        }
    }
    return NULL;
}

static void ia64_cpu_synchronize_from_tb(CPUState *cs,
                                         const TranslationBlock *tb)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    uint64_t ri = (tb->flags & IA64_TB_FLAG_RI_MASK) >> IA64_TB_FLAG_RI_SHIFT;

    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu->env.ip = tb->pc;
    /*
     * Translation-time instruction fetch faults occur before generated TCG
     * can update PSR.ri.  Restore the slot encoded in the TB key along with
     * its bundle address; otherwise a stale slot from the preceding TB is
     * saved in IPSR and rfi can skip the faulting bundle's prologue.
     */
    cpu->env.psr =
        (cpu->env.psr & ~IA64_PSR_RI_MASK) | (ri << IA64_PSR_RI_SHIFT);
}

static void ia64_restore_state_to_opc(CPUState *cs, const TranslationBlock *tb,
                                      const uint64_t *data)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    cpu->env.ip = data[0];
}

static int ia64_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    if (cpu->env.psr & (ifetch ? IA64_PSR_IT : IA64_PSR_DT)) {
        return MMU_IDX_VIRT_CPL(ia64_psr_cpl(cpu->env.psr));
    }
    return MMU_PHYS_IDX;
}

static vaddr ia64_pointer_wrap(CPUState *cs, int mmu_idx, vaddr result,
                               vaddr base)
{
    return result;
}

static int ia64_tlb_perm_to_prot(uint8_t perm)
{
    int prot = 0;

    if (perm & IA64_TLB_R) {
        prot |= PAGE_READ;
    }
    if (perm & IA64_TLB_W) {
        prot |= PAGE_WRITE;
    }
    if (perm & IA64_TLB_X) {
        prot |= PAGE_EXEC;
    }
    return prot;
}

static int ia64_tlb_prot_for_pte_psr(uint64_t pte, uint8_t perm, bool is_ifetch,
                                     uint64_t psr)
{
    int prot = ia64_tlb_perm_to_prot(perm);

    /* IA-64 has independent instruction and data translation caches. */
    prot &= is_ifetch ? PAGE_EXEC : (PAGE_READ | PAGE_WRITE);

    /*
     * QEMU's software TLB may satisfy later accesses without re-entering
     * tlb_fill.  Do not cache write permission for a clean IA-64 PTE: a
     * later store must take Data Dirty so the OS can update the PTE or break
     * copy-on-write sharing.
     */
    if (!is_ifetch && !(psr & IA64_PSR_DA)) {
        if (!(pte & IA64_PTE_ACCESSED)) {
            prot &= ~(PAGE_READ | PAGE_WRITE);
        }
        if (!(pte & IA64_PTE_DIRTY)) {
            prot &= ~PAGE_WRITE;
        }
    } else if (is_ifetch && !(psr & IA64_PSR_IA) &&
               !(pte & IA64_PTE_ACCESSED)) {
        prot &= ~PAGE_EXEC;
    }

    return prot;
}

static int ia64_tlb_prot_for_pte(CPUIA64State *env, uint64_t pte, uint8_t perm,
                                 bool is_ifetch)
{
    return ia64_tlb_prot_for_pte_psr(pte, perm, is_ifetch, env->psr);
}

static void ia64_record_suppressed_tlb_fill(CPUIA64State *env, vaddr addr,
                                            int mmu_idx)
{
    uint64_t page = addr & TARGET_PAGE_MASK;
    uint16_t idxmap = 1u << mmu_idx;
    uint8_t i;

    for (i = 0; i < env->exception_state.suppressed_tlb_count; i++) {
        if (env->exception_state.suppressed_tlb_pages[i] == page) {
            env->exception_state.suppressed_tlb_idxmaps[i] |= idxmap;
            return;
        }
    }

    if (env->exception_state.suppressed_tlb_count == IA64_SUPPRESSED_TLB_MAX) {
        env->exception_state.suppressed_tlb_overflow = true;
        return;
    }

    i = env->exception_state.suppressed_tlb_count++;
    env->exception_state.suppressed_tlb_pages[i] = page;
    env->exception_state.suppressed_tlb_idxmaps[i] = idxmap;
}

static void ia64_record_suppressed_tlb_fill_if_needed(CPUIA64State *env,
                                                      vaddr addr, int mmu_idx,
                                                      uint64_t pte,
                                                      uint8_t perm,
                                                      bool is_ifetch, int prot)
{
    uint64_t unsuppressed_psr = env->psr & ~(IA64_PSR_DA | IA64_PSR_IA);
    int unsuppressed_prot;

    if (!(env->psr & (IA64_PSR_DA | IA64_PSR_IA))) {
        return;
    }

    unsuppressed_prot =
        ia64_tlb_prot_for_pte_psr(pte, perm, is_ifetch, unsuppressed_psr);
    if (prot != unsuppressed_prot) {
        ia64_record_suppressed_tlb_fill(env, addr, mmu_idx);
    }
}

void ia64_flush_suppressed_tlb(CPUIA64State *env)
{
    CPUState *cs = env_cpu(env);
    uint8_t i;

    if (env->exception_state.suppressed_tlb_overflow) {
        tlb_flush(cs);
    } else {
        for (i = 0; i < env->exception_state.suppressed_tlb_count; i++) {
            tlb_flush_page_by_mmuidx(
                cs, env->exception_state.suppressed_tlb_pages[i],
                env->exception_state.suppressed_tlb_idxmaps[i]);
        }
    }

    env->exception_state.suppressed_tlb_count = 0;
    env->exception_state.suppressed_tlb_overflow = false;
}

static void ia64_tlb_set_entry_page(CPUState *cs, vaddr addr, hwaddr pa,
                                    uint64_t page_size, int prot, int mmu_idx,
                                    IA64MemorySpeculation speculation,
                                    uint8_t memory_attribute)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    uint64_t page = addr & TARGET_PAGE_MASK;
    CPUTLBEntryFull full = {
        .phys_addr = pa & TARGET_PAGE_MASK,
        .attrs = MEMTXATTRS_UNSPECIFIED,
        .prot = prot,
        .lg_page_size = TARGET_PAGE_BITS,
    };

    (void)page_size;
    cpu->page_attr[mmu_idx][(page >> TARGET_PAGE_BITS) %
                            IA64_PAGE_ATTR_SLOTS] = (IA64PageAttr) {
        .page = page,
        .speculation = speculation,
        .memory_attribute = memory_attribute,
        .valid = true,
    };
    tlb_set_page_full(cs, mmu_idx, addr & TARGET_PAGE_MASK, &full);
}

static bool ia64_cpu_translate_for_debug(CPUState *cs, vaddr addr,
                                         TranslateForDebugResult *result)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    uint64_t pa;

    if (!ia64_mmu_translate_debug(&cpu->env, addr, &pa)) {
        return false;
    }
    *result = (TranslateForDebugResult) {
        .physaddr = pa,
        .attrs = MEMTXATTRS_UNSPECIFIED,
        .lg_page_size = TARGET_PAGE_BITS,
    };
    result->attrs.debug = true;
    return true;
}

static bool ia64_cpu_tlb_fill(CPUState *cs, vaddr addr, int size,
                              MMUAccessType access_type, int mmu_idx,
                              bool probe, uintptr_t retaddr)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    bool is_ifetch = (access_type == MMU_INST_FETCH);
    uint8_t needed =
        is_ifetch ? IA64_TLB_X
                  : (access_type == MMU_DATA_STORE ? IA64_TLB_W : IA64_TLB_R);
    uint64_t pa;
    uint8_t perm;
    uint32_t rid;
    IA64Exception excp;
    bool is_rse = !is_ifetch && cpu->env.rse.rse_access;
    uint8_t access_level;
    bool virt_translation_enabled;

    rid = ia64_region_rid(&cpu->env, addr);
    if (mmu_idx == MMU_PHYS_IDX) {
        if (!ia64_pa_is_implemented(&cpu->env, addr)) {
            if (probe) {
                return false;
            }
            excp = is_ifetch ? IA64_EXCP_UNIMPL_INST_ADDR
                             : IA64_EXCP_UNIMPL_DATA_ADDR;
            if (is_ifetch) {
                cpu->env.ip = ia64_pa_canonicalize(&cpu->env, addr);
            }
            goto raise_exception;
        }
        pa = ia64_physical_address(addr);
        ia64_tlb_set_entry_page(cs, addr, pa, TARGET_PAGE_SIZE,
                                PAGE_READ | PAGE_WRITE | PAGE_EXEC, mmu_idx,
                                (addr & IA64_PHYS_UC_BIT)
                                    ? IA64_MEM_NON_SPECULATIVE
                                    : IA64_MEM_LIMITED_SPECULATION,
                                (addr & IA64_PHYS_UC_BIT) ? 4 : 0);
        return true;
    }

    if (is_rse) {
        access_level = ia64_rsc_pl(cpu->env.ar_rsc);
    } else {
        g_assert(mmu_idx >= MMU_IDX_VIRT_CPL0 && mmu_idx <= MMU_IDX_VIRT_CPL3);
        access_level = mmu_idx - MMU_IDX_VIRT_CPL0;
    }

    /* A translated MMU index is itself the serialized translation state. */
    virt_translation_enabled = true;
    if (virt_translation_enabled && !ia64_va_is_implemented(&cpu->env, addr)) {
        if (probe) {
            return false;
        }
        excp =
            is_ifetch ? IA64_EXCP_UNIMPL_INST_ADDR : IA64_EXCP_UNIMPL_DATA_ADDR;
        if (is_ifetch) {
            cpu->env.ip = ia64_va_canonicalize(&cpu->env, addr);
        }
        goto raise_exception;
    }

    {
        const IA64TlbEntry *entry =
            ia64_tlb_find_cached(&cpu->env, addr, rid, is_ifetch);

        if (entry) {
            int prot;
            IA64Exception pte_excp;

            ia64_tlb_entry_translate(entry, addr, access_level, &pa, &perm);
            pte_excp = ia64_tlb_exception_for_access(
                &cpu->env, entry, perm, needed, is_ifetch,
                access_type == MMU_DATA_STORE, is_rse);
            if (pte_excp != IA64_EXCP_NONE) {
                if (probe) {
                    return false;
                }
                excp = pte_excp;
                goto raise_exception;
            }
            prot =
                ia64_tlb_prot_for_pte(&cpu->env, entry->pte, perm, is_ifetch);
            ia64_record_suppressed_tlb_fill_if_needed(
                &cpu->env, addr, mmu_idx, entry->pte, perm, is_ifetch, prot);
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 tlb hit %c va=0x%016" PRIx64 " rid=0x%06" PRIx32
                          " pa=0x%016" PRIx64 " perm=0x%x\n",
                          is_ifetch ? 'i' : 'd', (uint64_t)addr, rid, pa, perm);
            /*
             * An L1 replacement can flush a derived softmmu entry.  Do it
             * before installing this access's host entry so the fill caller
             * never resumes through an entry invalidated during the fill.
             */
            if (!is_ifetch && !probe) {
                ia64_mmu_data_access(&cpu->env, addr, size, true);
            }
            ia64_tlb_set_entry_page(cs, addr, pa, entry->ps, prot, mmu_idx,
                                    ia64_pte_memory_speculation(entry->pte),
                                    (entry->pte >> 2) & 7);
            return true;
        }
    }

    if (!is_ifetch) {
        const IA64TlbEntry *new_entry;
        uint64_t pte = 0;
        uint32_t key = 0;

        if (ia64_vhpt_walk_full(&cpu->env, addr, rid, false, is_rse,
                                access_level, &pa, &perm, &pte, &key,
                                &new_entry)) {
            int prot;
            IA64Exception pte_excp;
            uint64_t page_size = new_entry ? new_entry->ps : TARGET_PAGE_SIZE;

            pte_excp = new_entry
                           ? ia64_tlb_exception_for_access(
                                 &cpu->env, new_entry, perm, needed, false,
                                 access_type == MMU_DATA_STORE, is_rse)
                           : ia64_translation_exception_for_access(
                                 &cpu->env, pte, key, perm, needed, false,
                                 access_type == MMU_DATA_STORE, is_rse);
            if (pte_excp != IA64_EXCP_NONE) {
                if (probe) {
                    return false;
                }
                excp = pte_excp;
                goto raise_exception;
            }
            prot = ia64_tlb_prot_for_pte(
                &cpu->env, new_entry ? new_entry->pte : pte, perm, false);
            ia64_record_suppressed_tlb_fill_if_needed(
                &cpu->env, addr, mmu_idx, new_entry ? new_entry->pte : pte,
                perm, false, prot);
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 vhpt hit d va=0x%016" PRIx64 " rid=0x%06" PRIx32
                          " pa=0x%016" PRIx64 " perm=0x%x iha=0x%016" PRIx64
                          "\n",
                          (uint64_t)addr, rid, pa, perm,
                          ia64_vhpt_hash_address(&cpu->env, addr));
            if (!probe) {
                ia64_mmu_data_access(&cpu->env, addr, size, true);
            }
            ia64_tlb_set_entry_page(
                cs, addr, pa, page_size, prot, mmu_idx,
                ia64_pte_memory_speculation(new_entry ? new_entry->pte : pte),
                ((new_entry ? new_entry->pte : pte) >> 2) & 7);
            return true;
        }
    }

    if (is_ifetch) {
        const IA64TlbEntry *new_entry;
        uint64_t pte = 0;
        uint32_t key = 0;

        if (ia64_vhpt_walk_full(&cpu->env, addr, rid, true, false, access_level,
                                &pa, &perm, &pte, &key, &new_entry)) {
            int prot;
            IA64Exception pte_excp;
            uint64_t page_size = new_entry ? new_entry->ps : TARGET_PAGE_SIZE;

            pte_excp =
                new_entry
                    ? ia64_tlb_exception_for_access(&cpu->env, new_entry, perm,
                                                    needed, true, false, false)
                    : ia64_translation_exception_for_access(&cpu->env, pte, key,
                                                            perm, needed, true,
                                                            false, false);
            if (pte_excp != IA64_EXCP_NONE) {
                if (probe) {
                    return false;
                }
                excp = pte_excp;
                goto raise_exception;
            }
            prot = ia64_tlb_prot_for_pte(
                &cpu->env, new_entry ? new_entry->pte : pte, perm, true);
            ia64_record_suppressed_tlb_fill_if_needed(
                &cpu->env, addr, mmu_idx, new_entry ? new_entry->pte : pte,
                perm, true, prot);
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 vhpt hit i va=0x%016" PRIx64 " rid=0x%06" PRIx32
                          " pa=0x%016" PRIx64 " perm=0x%x iha=0x%016" PRIx64
                          "\n",
                          (uint64_t)addr, rid, pa, perm,
                          ia64_vhpt_hash_address(&cpu->env, addr));
            ia64_tlb_set_entry_page(
                cs, addr, pa, page_size, prot, mmu_idx,
                ia64_pte_memory_speculation(new_entry ? new_entry->pte : pte),
                ((new_entry ? new_entry->pte : pte) >> 2) & 7);
            return true;
        }
    }
    if (probe) {
        return false;
    }

    {
        uint64_t vhpt_entry_va;
        uint8_t vhpt_size;
        bool vhpt_long_format;
        bool vhpt_enabled = ia64_vhpt_walker_enabled(
            &cpu->env, addr, is_ifetch, is_rse, &vhpt_size, &vhpt_long_format);

        if (!is_ifetch && ia64_data_nested_tlb_active(&cpu->env)) {
            excp = IA64_EXCP_DATA_NESTED_TLB;
        } else if (vhpt_enabled &&
                   ia64_vhpt_pte_not_present(&cpu->env, addr, is_ifetch, is_rse,
                                             &vhpt_entry_va)) {
            excp = IA64_EXCP_PAGE_NOT_PRESENT;
        } else if (!ia64_vhpt_entry_accessible(&cpu->env, addr, is_ifetch,
                                               is_rse, &vhpt_entry_va)) {
            excp = IA64_EXCP_VHPT_FAULT;
        } else if (vhpt_enabled) {
            excp = is_ifetch ? IA64_EXCP_ITLB_FAULT : IA64_EXCP_DTLB_FAULT;
        } else {
            excp = is_ifetch ? IA64_EXCP_ALT_ITLB : IA64_EXCP_ALT_DTLB;
        }
    }
raise_exception:
    /*
     * A softmmu access can fault after translated code has advanced beyond
     * the last architecturally synchronized bundle.  Restore the precise
     * instruction before deriving IA-64 state such as IPSR.ri and ISR.ed.
     * cpu_loop_exit_restore() would otherwise do this only after those
     * fields had already been computed.
     */
    if (retaddr) {
        cpu_restore_state(cs, retaddr);
        retaddr = 0;
    }
    if (is_ifetch && excp == IA64_EXCP_PAGE_NOT_PRESENT &&
        (cpu->env.psr & IA64_PSR_IC)) {
        /*
         * IIP receives IP on interruption entry, and for faults it must point
         * at the faulting instruction bundle when interruption collection is
         * enabled.  Instruction fetch page-not-present faults may be raised
         * while looking up the next TB, before env->ip has otherwise advanced
         * to the fetched bundle.
         */
        cpu->env.ip = ia64_ip_bundle_addr(addr);
    }
    /*
     * IPSR.ri must name the slot execution resumes at.  PSR.ri holds
     * the current slot for data references and, for instruction
     * fetches, the slot the fetch will resume at (0 after a branch;
     * the interrupted slot when refetching after an rfi).  Without
     * this, an instruction-fetch fault would reuse a stale fault_slot
     * and the handler's rfi would skip slots of the target bundle.
     */
    cpu->env.exception_state.fault_slot =
        (cpu->env.psr & IA64_PSR_RI_MASK) >> IA64_PSR_RI_SHIFT;
    if (cpu->env.psr & IA64_PSR_IC) {
        cpu->env.cr_ifa = addr;
        if (ia64_exception_initializes_iha(excp)) {
            cpu->env.cr_iha = ia64_vhpt_hash_address(&cpu->env, addr);
        }
        cpu->env.cr_itir = ia64_region_itir(
            &cpu->env, excp == IA64_EXCP_VHPT_FAULT ? cpu->env.cr_iha : addr);
    }
    if (excp != IA64_EXCP_DATA_NESTED_TLB) {
        if (excp == IA64_EXCP_UNIMPL_DATA_ADDR) {
            cpu->env.cr_isr =
                IA64_GENEX_UNIMPL_DATA_ADDR |
                (access_type == MMU_DATA_STORE ? IA64_ISR_W : IA64_ISR_R);
        } else if (excp == IA64_EXCP_UNIMPL_INST_ADDR) {
            cpu->env.cr_isr = IA64_GENEX_UNIMPL_INST_ADDR | IA64_ISR_X;
        } else {
            cpu->env.cr_isr =
                is_ifetch
                    ? IA64_ISR_X
                    : (access_type == MMU_DATA_STORE ? IA64_ISR_W : IA64_ISR_R);
            if (excp == IA64_EXCP_NAT_CONSUMPTION) {
                /*
                 * NaT Page Consumption reports ISR.code{5:4} = 2; the
                 * non-access code in ISR.code{3:0} is zero for an access.
                 */
                cpu->env.cr_isr |= IA64_ISR_CODE_NAT_PAGE;
            }
        }
        if (is_rse) {
            cpu->env.cr_isr |= IA64_ISR_RS;
            if (cpu->env.rse.rse_dirty < 0 || cpu->env.rse.rse_dirty_nat < 0) {
                /* Mandatory load for an incomplete frame (SDM 6.8). */
                cpu->env.cr_isr |= IA64_ISR_IR;
            }
        } else if (!is_ifetch && excp != IA64_EXCP_NAT_CONSUMPTION &&
                   ia64_current_code_tlb_ed(&cpu->env)) {
            /* NaT Page Consumption always reports ISR.ed as 0. */
            cpu->env.cr_isr |= IA64_ISR_ED;
        }
    }
    qemu_log_mask(CPU_LOG_MMU,
                  "ia64 tlb miss %c va=0x%016" PRIx64 " rid=0x%06" PRIx32
                  " ps=0x%016" PRIx64 " iha=0x%016" PRIx64 " pta=0x%016" PRIx64
                  " isr=0x%016" PRIx64 "\n",
                  is_ifetch ? 'i' : (access_type == MMU_DATA_STORE ? 'w' : 'r'),
                  (uint64_t)addr, rid, cpu->env.cr_itir, cpu->env.cr_iha,
                  cpu->env.cr_pta, cpu->env.cr_isr);
    cs->exception_index = excp;
    if (is_rse) {
        cpu->env.rse.rse_access = false;
    }
    cpu_loop_exit_restore(cs, retaddr);
}

static void ia64_cpu_set_architectural_reset_rse(CPUIA64State *env)
{
    /*
     * SDM Vol.2 6.12: reset exposes all 96 stacked registers in one frame,
     * with BOF at physical GR32.  rse_bol is zero-based within the physical
     * stacked-register file, so zero denotes physical GR32.
     */
    env->cfm_sof = IA64_STACKED_GR_COUNT;
    env->cfm_sol = 0;
    env->cfm_sor = 0;
    env->cfm_rrb_gr = 0;
    env->cfm_rrb_fr = 0;
    env->cfm_rrb_pr = 0;
    env->rse.rse_bol = 0;
    env->rse.rse_dirty = 0;
    env->rse.rse_dirty_nat = 0;
    env->rse.rse_clean = 0;
    env->rse.rse_clean_nat = 0;
    env->rse.rse_invalid = 0;
}

static void ia64_cpu_reset_hold(Object *obj, ResetType type)
{
    IA64CPUClass *icc = IA64_CPU_GET_CLASS(obj);
    IA64CPU *cpu = IA64_CPU(obj);

    if (icc->parent_phases.hold) {
        icc->parent_phases.hold(obj, type);
    }

    if (cpu->itm_timer != NULL) {
        timer_del(cpu->itm_timer);
    }
    memset(cpu->msr, 0, sizeof(cpu->msr));
    cpu->msr1552_written = false;
    cpu->msr1212_complete = false;
    ia64_cache_array_reset(cpu);
    ia64_alat_write_abort(&cpu->env);
    memset(&cpu->env, 0, sizeof(cpu->env));
    memset(cpu->alat_seen_sequence, 0, cpu->max_cpus * sizeof(uint64_t));
    cpu->env.fp.fr[IA64_FR_ONE_INDEX] = IA64_FR_ONE;
    cpu->env.pr[IA64_PR_TRUE] = 1;
    cpu->env.psr = 0;
    /* Inferred Madison PALE_RESET vector. */
    if (icc->model == IA64_CPU_MODEL_MADISON) {
        cpu->env.ip = UINT64_C(0xffffffb0);
    }
    cpu->env.ar_rsc = 0;
    ia64_cpu_set_architectural_reset_rse(&cpu->env);
    ia64_rse_rnat_undefined(&cpu->env, "processor reset");
    cpu->env.ar_fpsr = IA64_FPSR_DEFAULT;
    cpu->env.cr_iva = 0;
    cpu->env.instruction_group_start = true;
    ia64_itc_reset(&cpu->env);
    set_float_2nan_prop_rule(float_2nan_prop_ab, &cpu->env.fp.fp_status);
    set_float_3nan_prop_rule(float_3nan_prop_abc, &cpu->env.fp.fp_status);
    set_float_infzeronan_rule(float_infzeronan_dnan_never |
                                  float_infzeronan_suppress_invalid,
                              &cpu->env.fp.fp_status);
    set_float_default_nan_pattern(0b11000000, &cpu->env.fp.fp_status);
    cpu->env.cr[IA64_CR_SAPIC_LID] = (uint64_t)cpu->sapic_id << 16;
    cpu->env.cr[IA64_CR_SAPIC_TPR] = 0;
    cpu->env.cr[IA64_CR_ITV] = IA64_VECTOR_MASKED;
    cpu->env.cr[IA64_CR_PMV] = IA64_VECTOR_MASKED;
    cpu->env.cr[IA64_CR_CMCV] = IA64_VECTOR_MASKED;
    cpu->env.interrupt.sapic_xtp = IA64_SAPIC_XTP_DISABLE;
    if (icc->model == IA64_CPU_MODEL_MERCED) {
        cpu->env.pmc[8] = 0xf00000003ffffff8ULL;
        cpu->env.pmc[9] = 0xf00000003ffffff8ULL;
        cpu->env.pmc[11] = 1ULL << 28;
        cpu->env.pmc[13] = 1;
    }
    ia64_store_watch_update(&cpu->env);
}

static const struct {
    const char *name;
    const char *model;
} ia64_cpu_aliases[] = {
    { "merced", "merced-800" },        { "mckinley", "mckinley-1000" },
    { "deerfield", "deerfield-1000" }, { "madison", "madison-1600-3m" },
    { "montecito", "montecito-9050" }, { "montvale", "montvale-9150n" },
    { "tukwila", "tukwila-9350" },     { "itanium-9300", "tukwila-9350" },
    { "itanium", "merced-800" },       { "itanium2", "montecito-9050" },
};

static ObjectClass *ia64_cpu_class_by_name(const char *cpu_model)
{
    g_autofree char *typename = NULL;

    for (size_t i = 0; i < G_N_ELEMENTS(ia64_cpu_aliases); i++) {
        if (g_str_equal(cpu_model, ia64_cpu_aliases[i].name)) {
            cpu_model = ia64_cpu_aliases[i].model;
            break;
        }
    }
    typename = g_strdup_printf(IA64_CPU_TYPE_NAME("%s"), cpu_model);
    return object_class_by_name(typename);
}

static void ia64_cpu_list(void)
{
    GSList *list = object_class_get_list_sorted(TYPE_IA64_CPU, false);

    qemu_printf("Available CPUs:\n");
    for (GSList *entry = list; entry; entry = entry->next) {
        const char *typename = object_class_get_name(entry->data);
        g_autofree char *model = cpu_model_from_type(typename);

        qemu_printf("  %s\n", model);
    }
    g_slist_free(list);

    qemu_printf("CPU aliases:\n");
    for (size_t i = 0; i < G_N_ELEMENTS(ia64_cpu_aliases); i++) {
        qemu_printf("  %s (alias for %s)\n", ia64_cpu_aliases[i].name,
                    ia64_cpu_aliases[i].model);
    }
}

static void ia64_cpu_del_l3_ways(IA64CPU *cpu, unsigned count)
{
    for (unsigned way = 0; way < count; way++) {
        memory_region_del_subregion(&cpu->l3_root, &cpu->l3_way[way]);
    }
}

static void ia64_cpu_realize(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    IA64CPU *cpu = IA64_CPU(dev);
    IA64CPUClass *icc = IA64_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu->semantic_profile_id = icc->semantic_profile_id;
    cpu->semantic_profile_abi = icc->semantic_profile_abi;

    /* Each CPU sees system memory through its own container for L3 ways. */
    memory_region_init(&cpu->l3_root, OBJECT(cpu), "ia64-cpu-memory",
                       UINT64_MAX);
    memory_region_init_alias(&cpu->l3_system, OBJECT(cpu), "ia64-system-memory",
                             get_system_memory(), 0, UINT64_MAX);
    memory_region_add_subregion(&cpu->l3_root, 0, &cpu->l3_system);
    object_property_set_link(OBJECT(cs), "memory", OBJECT(&cpu->l3_root),
                             &error_abort);

    cpu_common_realize(cs, &local_err);
    if (local_err != NULL) {
        memory_region_del_subregion(&cpu->l3_root, &cpu->l3_system);
        error_propagate(errp, local_err);
        return;
    }
    cpu->max_cpus = current_machine->smp.max_cpus;
    if (cs->cpu_index >= cpu->max_cpus) {
        error_setg(&local_err, "ia64: CPU index %d is not below the machine's"
                   " maximum of %u CPUs", cs->cpu_index, cpu->max_cpus);
        goto fail;
    }

    /* The PAL maps a way by enabling it at a new address; see cache-array.c. */
    for (unsigned way = 0; way < IA64_L3_WAYS; way++) {
        g_autofree char *name = g_strdup_printf("ia64-l3-cpu%d-way%u",
                                                cs->cpu_index, way);

        memory_region_init_ram(&cpu->l3_way[way], OBJECT(cpu), name,
                               IA64_L3_WAY_SIZE, &local_err);
        if (local_err != NULL) {
            ia64_cpu_del_l3_ways(cpu, way);
            goto fail;
        }
        memory_region_set_enabled(&cpu->l3_way[way], false);
        memory_region_add_subregion_overlap(&cpu->l3_root, 0,
                                            &cpu->l3_way[way], 1);
    }

    cpu->alat_seen_sequence = g_new0(uint64_t, cpu->max_cpus);
    cpu->itm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ia64_itm_timer_cb, cpu);

    qemu_init_vcpu(cs);
    cpu_reset(cs);

    /* Stores need the ALAT log as soon as a second CPU exists. */
    if (CPU_NEXT(first_cpu)) {
        CPUState *other;

        CPU_FOREACH(other) {
            IA64CPU *peer = ia64_cpu_from_cpu_state(other);

            peer->has_peers = true;
            ia64_store_watch_update(&peer->env);
        }
    }

    icc->parent_realize(dev, errp);
    return;

fail:
    cpu_common_unrealize(cs);
    memory_region_del_subregion(&cpu->l3_root, &cpu->l3_system);
    error_propagate(errp, local_err);
}

static void ia64_cpu_finalize(Object *obj)
{
    IA64CPU *cpu = IA64_CPU(obj);

    if (cpu->itm_timer != NULL) {
        timer_free(cpu->itm_timer);
        cpu->itm_timer = NULL;
    }
    g_clear_pointer(&cpu->alat_seen_sequence, g_free);
    g_clear_pointer(&cpu->ca_lines, g_hash_table_destroy);
    g_clear_pointer(&cpu->ca_l3, g_hash_table_destroy);
    g_clear_pointer(&cpu->ca_mirror, g_free);
}

static void ia64_cpu_unrealize(DeviceState *dev)
{
    IA64CPU *cpu = IA64_CPU(dev);
    IA64CPUClass *icc = IA64_CPU_GET_CLASS(dev);

    ia64_cpu_del_l3_ways(cpu, IA64_L3_WAYS);
    memory_region_del_subregion(&cpu->l3_root, &cpu->l3_system);
    icc->parent_unrealize(dev);
}

static const struct SysemuCPUOps ia64_sysemu_ops = {
    .has_work = ia64_cpu_has_work,
    .translate_for_debug = ia64_cpu_translate_for_debug,
};

#ifndef CONFIG_USER_ONLY
/* No machine check is modelled: a refused access is a checkstop. */
static void ia64_cpu_do_transaction_failed(CPUState *cs, hwaddr physaddr,
                                           vaddr addr, unsigned size,
                                           MMUAccessType access_type,
                                           int mmu_idx, MemTxAttrs attrs,
                                           MemTxResult response,
                                           uintptr_t retaddr)
{
    static const char *const kind[] = { "load", "store", "fetch" };
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    char why[96];

    cpu_restore_state(cs, retaddr);
    snprintf(why, sizeof(why), "bus error on %s of %u bytes at 0x%016" PRIx64
             " (physical 0x%" HWADDR_PRIx ")", kind[access_type], size,
             (uint64_t)addr, physaddr);
    ia64_cpu_checkstop(&cpu->env, why);
}
#endif

static const TCGCPUOps ia64_tcg_ops = {
    /*
     * Native IA-64 loads and stores are weakly ordered; the translator emits
     * acquire, release and mf explicitly, so TCG adds no barriers. Store-load
     * ordering of UC and UCE pages is not enforced.
     */
    .guest_default_memory_order = 0,
    .mttcg_supported = true,
    .precise_smc = true,
    .initialize = ia64_translate_init,
    .translate_code = ia64_translate_code,
    .get_tb_cpu_state = ia64_get_tb_cpu_state,
    .synchronize_from_tb = ia64_cpu_synchronize_from_tb,
    .restore_state_to_opc = ia64_restore_state_to_opc,
    .mmu_index = ia64_cpu_mmu_index,
    .tlb_fill = ia64_cpu_tlb_fill,
    .pointer_wrap = ia64_pointer_wrap,
#ifndef CONFIG_USER_ONLY
    .do_unaligned_access = ia64_cpu_do_unaligned_access,
    .do_transaction_failed = ia64_cpu_do_transaction_failed,
#endif
    .cpu_exec_interrupt = ia64_cpu_exec_interrupt,
    .cpu_exec_reset = cpu_reset,
    .do_interrupt = ia64_cpu_do_interrupt,
};

#define IA64_ITANIUM2_MEMORY_ATTRIBUTE_MASK \
    ((1U << IA64_PTE_MA_WB) | (1U << IA64_PTE_MA_UC) | \
     (1U << IA64_PTE_MA_UCE) | (1U << IA64_PTE_MA_WC))

#define IA64_FREQUENCY_RATIO(numerator, denominator) \
    (((uint64_t)(numerator) << 32) | (denominator))

static const Property ia64_cpu_properties[] = {
    DEFINE_PROP_UINT16("sapic-id", IA64CPU, sapic_id, 0),
    DEFINE_PROP_UINT64("pal-auth-key-addr", IA64CPU, pal_auth_key_addr, 0),
};

static void ia64_cpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);
    IA64CPUClass *icc = IA64_CPU_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, ia64_cpu_realize, &icc->parent_realize);
    device_class_set_parent_unrealize(dc, ia64_cpu_unrealize,
                                      &icc->parent_unrealize);
    resettable_class_set_parent_phases(rc, NULL, ia64_cpu_reset_hold, NULL,
                                       &icc->parent_phases);

    cc->class_by_name = ia64_cpu_class_by_name;
    cc->list_cpus = ia64_cpu_list;
    cc->dump_state = ia64_cpu_dump_state;
    cc->set_pc = ia64_cpu_set_pc;
    cc->get_pc = ia64_cpu_get_pc;
    cc->sysemu_ops = &ia64_sysemu_ops;
    cc->gdb_read_register = ia64_cpu_gdb_read_register;
    cc->gdb_write_register = ia64_cpu_gdb_write_register;
    cc->gdb_num_core_regs = IA64_GDB_NUM_CORE_REGS;
    cc->tcg_ops = &ia64_tcg_ops;
    dc->vmsd = &vmstate_ia64_cpu;
    device_class_set_props(dc, ia64_cpu_properties);

    icc->semantic_profile_abi = 1;
    icc->model = IA64_CPU_MODEL_MONTECITO;
    icc->cpuid_version = 0x0000000020000704ULL;
    icc->cpuid_features = IA64_CPUID4_LB | IA64_CPUID4_AO;
    icc->frequency_base_hz = 100000000;
    icc->itc_frequency_hz = 400000000;
    icc->processor_frequency_ratio = IA64_FREQUENCY_RATIO(16, 1);
    icc->bus_frequency_ratio = IA64_FREQUENCY_RATIO(16, 3);
    icc->itc_frequency_ratio = IA64_FREQUENCY_RATIO(4, 1);
    icc->insertable_page_size_mask = IA64_INSERTABLE_PAGE_SIZE_MASK;
    icc->purgeable_page_size_mask = IA64_PURGEABLE_PAGE_SIZE_MASK;
    icc->itr_count = 32;
    icc->dtr_count = 32;
    icc->itlb_entries = IA64_TLB_MAX;
    icc->dtlb_entries = IA64_TLB_MAX;
    icc->phys_addr_bits = IA64_IMPL_PA_BITS;
    icc->impl_va_msb = IA64_IMPL_VA_MSB;
    icc->rid_bits = IA64_IMPL_RID_BITS;
    icc->key_bits = IA64_IMPL_KEY_BITS;
    icc->hash_tag_id = 8;
    icc->unique_tcs = 4;
    icc->tc_levels = 2;
    icc->perf_counter_width = 48;
    /* WB, UC, UCE, and WC are distinct; WC buffering is not modeled. */
    icc->memory_attribute_mask = IA64_ITANIUM2_MEMORY_ATTRIBUTE_MASK;
    icc->fc_line_size = 128;
    icc->implemented_pmc_mask = 0x3fffULL;
    icc->implemented_pmd_mask = 0x3ffffULL;
    icc->perf_cycles_mask = 0xf0ULL;
    icc->perf_retired_mask = 0xf0ULL;
    /* Intel document 308065-001, table 4-40: the clean count is always zero. */
    icc->rse_has_clean_partition = false;
    icc->has_virtualization = true;
    icc->is_montecito = true;
}

typedef struct IA64CPUModelDef {
    uint64_t semantic_profile_id;
    IA64CPUModel model;
    uint64_t cpuid_version;
    uint64_t cpuid_features;
    uint32_t frequency_base_hz;
    uint32_t itc_frequency_hz;
    uint64_t processor_frequency_ratio;
    uint64_t bus_frequency_ratio;
    uint64_t itc_frequency_ratio;
    uint64_t insertable_page_size_mask;
    uint64_t purgeable_page_size_mask;
    uint8_t itr_count;
    uint8_t dtr_count;
    uint16_t itlb_entries;
    uint16_t dtlb_entries;
    uint8_t phys_addr_bits;
    uint8_t impl_va_msb;
    uint8_t rid_bits;
    uint8_t key_bits;
    uint8_t hash_tag_id;
    uint8_t unique_tcs;
    uint8_t tc_levels;
    uint8_t perf_counter_width;
    uint8_t memory_attribute_mask;
    uint16_t fc_line_size;
    uint64_t implemented_pmc_mask;
    uint64_t implemented_pmd_mask;
    uint64_t perf_cycles_mask;
    uint64_t perf_retired_mask;
    bool rse_has_clean_partition;
    bool data_debug_cross_16byte;
    bool has_virtualization;
    bool is_montecito;
} IA64CPUModelDef;

static void ia64_cpu_model_class_init(ObjectClass *oc, const void *data)
{
    IA64CPUClass *icc = IA64_CPU_CLASS(oc);
    const IA64CPUModelDef *model = data;

    icc->semantic_profile_id = model->semantic_profile_id;
    icc->model = model->model;
    icc->cpuid_version = model->cpuid_version;
    icc->cpuid_features = model->cpuid_features;
    icc->frequency_base_hz = model->frequency_base_hz;
    icc->itc_frequency_hz = model->itc_frequency_hz;
    icc->processor_frequency_ratio = model->processor_frequency_ratio;
    icc->bus_frequency_ratio = model->bus_frequency_ratio;
    icc->itc_frequency_ratio = model->itc_frequency_ratio;
    icc->insertable_page_size_mask = model->insertable_page_size_mask;
    icc->purgeable_page_size_mask = model->purgeable_page_size_mask;
    icc->itr_count = model->itr_count;
    icc->dtr_count = model->dtr_count;
    icc->itlb_entries = model->itlb_entries;
    icc->dtlb_entries = model->dtlb_entries;
    icc->phys_addr_bits = model->phys_addr_bits;
    icc->impl_va_msb = model->impl_va_msb;
    icc->rid_bits = model->rid_bits;
    icc->key_bits = model->key_bits;
    icc->hash_tag_id = model->hash_tag_id;
    icc->unique_tcs = model->unique_tcs;
    icc->tc_levels = model->tc_levels;
    icc->perf_counter_width = model->perf_counter_width;
    icc->memory_attribute_mask = model->memory_attribute_mask;
    icc->fc_line_size = model->fc_line_size;
    icc->implemented_pmc_mask = model->implemented_pmc_mask;
    icc->implemented_pmd_mask = model->implemented_pmd_mask;
    icc->perf_cycles_mask = model->perf_cycles_mask;
    icc->perf_retired_mask = model->perf_retired_mask;
    icc->rse_has_clean_partition = model->rse_has_clean_partition;
    icc->data_debug_cross_16byte = model->data_debug_cross_16byte;
    icc->has_virtualization = model->has_virtualization;
    icc->is_montecito = model->is_montecito;

    g_assert(model->itlb_entries > 0 && model->itlb_entries <= IA64_TLB_MAX);
    g_assert(model->dtlb_entries > 0 && model->dtlb_entries <= IA64_TLB_MAX);
    g_assert(model->itr_count <= model->itlb_entries);
    g_assert(model->dtr_count <= model->dtlb_entries);
    g_assert(model->frequency_base_hz > 0);
    g_assert(model->itc_frequency_hz > 0 &&
             model->itc_frequency_hz <= 1600000000);
    g_assert((uint32_t)model->itc_frequency_ratio != 0);
    g_assert((uint64_t)model->frequency_base_hz *
                 (model->itc_frequency_ratio >> 32) /
                 (uint32_t)model->itc_frequency_ratio ==
             model->itc_frequency_hz);
    g_assert(model->fc_line_size >= 32 && is_power_of_2(model->fc_line_size));
}

static const IA64CPUModelDef ia64_cpu_model_merced = {
    .semantic_profile_id = 0x6d65726365640001ULL,
    .model = IA64_CPU_MODEL_MERCED,
    .cpuid_version = 0x0000000007000804ULL,
    .cpuid_features = 0,
    /*
     * PAL_A model 8 and PAL_B model 8 use revision 30 for this release.
     */
    /* The 800 MHz model advances ITC at the processor frequency. */
    .frequency_base_hz = 100000000,
    .itc_frequency_hz = 800000000,
    .processor_frequency_ratio = IA64_FREQUENCY_RATIO(8, 1),
    .bus_frequency_ratio = IA64_FREQUENCY_RATIO(4, 3),
    .itc_frequency_ratio = IA64_FREQUENCY_RATIO(8, 1),
    /* Include the architecturally required 64 MiB page size. */
    .insertable_page_size_mask =
        (1ULL << 12) | (1ULL << 13) | (1ULL << 14) | (1ULL << 16) |
        (1ULL << 18) | (1ULL << 20) | (1ULL << 22) | (1ULL << 24) |
        (1ULL << 26) | (1ULL << 28),
    .purgeable_page_size_mask =
        (1ULL << 12) | (1ULL << 13) | (1ULL << 14) | (1ULL << 16) |
        (1ULL << 18) | (1ULL << 20) | (1ULL << 22) | (1ULL << 24) |
        (1ULL << 26) | (1ULL << 28) | (1ULL << 32),
    .itr_count = 8,
    .dtr_count = 48,
    /*
     * The architecturally visible translation storage is the 64-entry
     * instruction TLB and the 96-entry main data TLB.  The 32-entry
     * first-level data TLB only caches the main data TLB and is not extra
     * TR/TC storage.
     */
    .itlb_entries = 64,
    .dtlb_entries = 96,
    .phys_addr_bits = 44,
    .impl_va_msb = 50,
    .rid_bits = 18,
    .key_bits = 21,
    .hash_tag_id = 0,
    .unique_tcs = 3,
    .tc_levels = 2,
    .perf_counter_width = 32,
    .memory_attribute_mask = (1U << IA64_PTE_MA_WB) |
                             (1U << IA64_PTE_MA_UC) |
                             (1U << IA64_PTE_MA_WC) |
                             (1U << IA64_PTE_MA_NATPAGE),
    /* The first-generation L2/L3 cache line is 64 bytes. */
    .fc_line_size = 64,
    .implemented_pmc_mask = 0x3fffULL,
    .implemented_pmd_mask = 0x3ffffULL,
    .perf_cycles_mask = 0xf0ULL,
    /*
     * The processor specification update corrects the product manual's 0x10
     * result: PAL 8.8.30 reports the implemented PMC4/PMC5 pair.
     */
    .perf_retired_mask = 0x30ULL,
    .rse_has_clean_partition = false,
};

static const IA64CPUModelDef ia64_cpu_model_madison = {
    .semantic_profile_id = 0x6d616469736f0001ULL,
    .model = IA64_CPU_MODEL_MADISON,
    /* Family 0x1f, model 1, revision 5, CPUID[4] is the last register. */
    .cpuid_version = 0x000000001f010504ULL,
    /*
     * The selected family 0x1f/model 1/revision 5 processor reports only
     * long-branch support in CPUID[4].  In particular, early deferral
     * selected through PAL is not the CPUID spontaneous-deferral feature.
     */
    .cpuid_features = IA64_CPUID4_LB,
    /* Latest documented PAL release for the selected B1 model. */
    .frequency_base_hz = 100000000,
    .itc_frequency_hz = 1600000000,
    .processor_frequency_ratio = IA64_FREQUENCY_RATIO(16, 1),
    .bus_frequency_ratio = IA64_FREQUENCY_RATIO(4, 1),
    .itc_frequency_ratio = IA64_FREQUENCY_RATIO(16, 1),
    .insertable_page_size_mask = IA64_INSERTABLE_PAGE_SIZE_MASK,
    .purgeable_page_size_mask = IA64_PURGEABLE_PAGE_SIZE_MASK,
    .itr_count = 64,
    .dtr_count = 64,
    .itlb_entries = IA64_TLB_MAX,
    .dtlb_entries = IA64_TLB_MAX,
    .phys_addr_bits = IA64_IMPL_PA_BITS,
    .impl_va_msb = IA64_IMPL_VA_MSB,
    .rid_bits = IA64_IMPL_RID_BITS,
    .key_bits = IA64_IMPL_KEY_BITS,
    .hash_tag_id = 8,
    .unique_tcs = 4,
    .tc_levels = 2,
    .perf_counter_width = 47,
    .memory_attribute_mask = IA64_ITANIUM2_MEMORY_ATTRIBUTE_MASK,
    /* Intel order 251110-003, section 5.8: each fc invalidates 128 bytes. */
    .fc_line_size = 128,
    .implemented_pmc_mask = 0xffffULL,
    .implemented_pmd_mask = 0x3ffffULL,
    .perf_cycles_mask = 0xf0ULL,
    .perf_retired_mask = 0xf0ULL,
    /* Intel document 251110-003, table 11-35: clean count is always zero. */
    .rse_has_clean_partition = false,
    /* Intel order 251110-003, section 12.3. */
    .data_debug_cross_16byte = true,
    .has_virtualization = false,
};

static const IA64CPUModelDef ia64_cpu_model_montecito = {
    .semantic_profile_id = 0x6d6f6e7465630001ULL,
    .model = IA64_CPU_MODEL_MONTECITO,
    /* Family 0x20, model 0, C2 revision 7, CPUID[4] is the last register. */
    .cpuid_version = 0x0000000020000704ULL,
    /* C2 reports long-branch and 16-byte atomic support (CPUID[4] = 5). */
    .cpuid_features = IA64_CPUID4_LB | IA64_CPUID4_AO,
    /* Latest documented PAL release for the selected C2 model. */
    /* Keep ITC advancement consistent with the advertised 4:1 ratio. */
    .frequency_base_hz = 100000000,
    .itc_frequency_hz = 400000000,
    .processor_frequency_ratio = IA64_FREQUENCY_RATIO(16, 1),
    .bus_frequency_ratio = IA64_FREQUENCY_RATIO(16, 3),
    .itc_frequency_ratio = IA64_FREQUENCY_RATIO(4, 1),
    .insertable_page_size_mask = IA64_INSERTABLE_PAGE_SIZE_MASK,
    .purgeable_page_size_mask = IA64_PURGEABLE_PAGE_SIZE_MASK,
    .itr_count = 32,
    .dtr_count = 32,
    .itlb_entries = IA64_TLB_MAX,
    .dtlb_entries = IA64_TLB_MAX,
    .phys_addr_bits = IA64_IMPL_PA_BITS,
    .impl_va_msb = IA64_IMPL_VA_MSB,
    .rid_bits = IA64_IMPL_RID_BITS,
    .key_bits = IA64_IMPL_KEY_BITS,
    .hash_tag_id = 8,
    .unique_tcs = 4,
    .tc_levels = 2,
    .perf_counter_width = 48,
    /*
     * The dual-core update inherits the Madison WB/UC/UCE/WC memory
     * attributes and explicitly gives UC/UCE/WC restrictions for its new
     * 16-byte operations.
     */
    .memory_attribute_mask = IA64_ITANIUM2_MEMORY_ATTRIBUTE_MASK,
    /* Montecito's L2 and L3 cache lines are 128 bytes. */
    .fc_line_size = 128,
    .implemented_pmc_mask = 0x3fffULL,
    .implemented_pmd_mask = 0x3ffffULL,
    .perf_cycles_mask = 0xf0ULL,
    .perf_retired_mask = 0xf0ULL,
    /* Intel document 308065-001, table 4-40: clean count is always zero. */
    .rse_has_clean_partition = false,
    /* Virtualization mode is not modeled; vmsw raises Virtualization Fault. */
    .has_virtualization = true,
    .is_montecito = true,
};

typedef struct IA64CPUVariantDef {
    uint64_t semantic_profile_id;
    uint64_t cpuid_version;
    uint32_t itc_frequency_hz;
    uint32_t l3_kib;
    uint64_t processor_ratio;
    uint64_t bus_ratio;
    uint64_t itc_ratio;
} IA64CPUVariantDef;

static void ia64_cpu_variant_class_init(ObjectClass *oc, const void *data)
{
    IA64CPUClass *icc = IA64_CPU_CLASS(oc);
    const IA64CPUVariantDef *variant = data;

    icc->semantic_profile_id = variant->semantic_profile_id;
    if (variant->cpuid_version) {
        icc->cpuid_version = variant->cpuid_version;
    }
    icc->itc_frequency_hz = variant->itc_frequency_hz;
    icc->l3_kib = variant->l3_kib;
    icc->processor_frequency_ratio = variant->processor_ratio;
    icc->bus_frequency_ratio = variant->bus_ratio;
    icc->itc_frequency_ratio = variant->itc_ratio;

    g_assert((uint64_t)icc->frequency_base_hz *
                 (icc->itc_frequency_ratio >> 32) /
                 (uint32_t)icc->itc_frequency_ratio ==
             icc->itc_frequency_hz);
}

#define IA64_SINGLE_CORE_VARIANT(_name, _mhz, _cache_kib) \
    static const IA64CPUVariantDef _name = {                          \
        .semantic_profile_id = UINT64_C(0x1000000000000000) |        \
                               ((uint64_t)(_mhz) << 20) |            \
                               (_cache_kib),                         \
        .itc_frequency_hz = (_mhz) * 1000000U,                        \
        .l3_kib = (_cache_kib),                                       \
        .processor_ratio = IA64_FREQUENCY_RATIO((_mhz), 100),         \
        .bus_ratio = IA64_FREQUENCY_RATIO(4, 1),                      \
        .itc_ratio = IA64_FREQUENCY_RATIO((_mhz), 100),               \
    }

static const IA64CPUVariantDef ia64_cpu_mckinley = {
    .semantic_profile_id = 0x6d636b696e6c0001ULL,
    .cpuid_version = 0x000000001f000704ULL,
    .itc_frequency_hz = 1000000000U,
    .processor_ratio = IA64_FREQUENCY_RATIO(10, 1),
    .bus_ratio = IA64_FREQUENCY_RATIO(4, 1),
    .itc_ratio = IA64_FREQUENCY_RATIO(10, 1),
};

IA64_SINGLE_CORE_VARIANT(ia64_cpu_mckinley_900, 900, 1536);
IA64_SINGLE_CORE_VARIANT(ia64_cpu_deerfield, 1000, 1536);
IA64_SINGLE_CORE_VARIANT(ia64_cpu_madison_1500k, 1400, 1536);
IA64_SINGLE_CORE_VARIANT(ia64_cpu_madison_3m, 1600, 3072);
IA64_SINGLE_CORE_VARIANT(ia64_cpu_madison_4m, 1400, 4096);
IA64_SINGLE_CORE_VARIANT(ia64_cpu_madison_6m, 1500, 6144);
static const IA64CPUVariantDef ia64_cpu_madison_9m = {
    .semantic_profile_id = 0x6d6164396d000001ULL,
    .cpuid_version = 0x000000001f020204ULL,
    .itc_frequency_hz = 1600000000U,
    .processor_ratio = IA64_FREQUENCY_RATIO(16, 1),
    .bus_ratio = IA64_FREQUENCY_RATIO(16, 3),
    .itc_ratio = IA64_FREQUENCY_RATIO(16, 1),
};

static const IA64CPUVariantDef ia64_cpu_montecito_9010 = {
    .semantic_profile_id = 0x6d6f6e0039303130ULL,
    .itc_frequency_hz = 400000000U,
    .processor_ratio = IA64_FREQUENCY_RATIO(16, 1),
    .bus_ratio = IA64_FREQUENCY_RATIO(16, 3),
    .itc_ratio = IA64_FREQUENCY_RATIO(4, 1),
};

static const IA64CPUVariantDef ia64_cpu_montecito_9015 = {
    .semantic_profile_id = 0x6d6f6e0039303135ULL,
    .itc_frequency_hz = 350000000U,
    .processor_ratio = IA64_FREQUENCY_RATIO(14, 1),
    .bus_ratio = IA64_FREQUENCY_RATIO(4, 1),
    .itc_ratio = IA64_FREQUENCY_RATIO(7, 2),
};

static const IA64CPUVariantDef ia64_cpu_montecito_9020 = {
    .semantic_profile_id = 0x6d6f6e0039303230ULL,
    .itc_frequency_hz = 355000000U,
    .processor_ratio = IA64_FREQUENCY_RATIO(142, 10),
    .bus_ratio = IA64_FREQUENCY_RATIO(16, 3),
    .itc_ratio = IA64_FREQUENCY_RATIO(71, 20),
};

static const IA64CPUVariantDef ia64_cpu_montecito_9030 = {
    .semantic_profile_id = 0x6d6f6e0039303330ULL,
    .itc_frequency_hz = 400000000U,
    .processor_ratio = IA64_FREQUENCY_RATIO(16, 1),
    .bus_ratio = IA64_FREQUENCY_RATIO(16, 3),
    .itc_ratio = IA64_FREQUENCY_RATIO(4, 1),
};

static const IA64CPUVariantDef ia64_cpu_montecito_9040 = {
    .semantic_profile_id = 0x6d6f6e0039303430ULL,
    .itc_frequency_hz = 400000000U,
    .processor_ratio = IA64_FREQUENCY_RATIO(16, 1),
    .bus_ratio = IA64_FREQUENCY_RATIO(16, 3),
    .itc_ratio = IA64_FREQUENCY_RATIO(4, 1),
};

static const IA64CPUVariantDef ia64_cpu_montecito_9050 = {
    .semantic_profile_id = 0x6d6f6e0039303530ULL,
    .itc_frequency_hz = 400000000U,
    .processor_ratio = IA64_FREQUENCY_RATIO(16, 1),
    .bus_ratio = IA64_FREQUENCY_RATIO(16, 3),
    .itc_ratio = IA64_FREQUENCY_RATIO(4, 1),
};

#define IA64_MONTVALE_VARIANT(_name, _mhz, _l3_mb, _package_mb,            \
                              _bus_num, _bus_den)                       \
    static const IA64CPUVariantDef _name = {                           \
        .semantic_profile_id = UINT64_C(0x2000000000000000) |         \
                               ((uint64_t)(_mhz) << 24) |             \
                               ((uint64_t)(_package_mb) << 8) |       \
                               (_l3_mb),                             \
        .itc_frequency_hz = ((_mhz) * 1000000U) / 4,                  \
        .processor_ratio = IA64_FREQUENCY_RATIO((_mhz), 100),         \
        .bus_ratio = IA64_FREQUENCY_RATIO((_bus_num), (_bus_den)),    \
        .itc_ratio = IA64_FREQUENCY_RATIO((_mhz), 400),               \
    }

IA64_MONTVALE_VARIANT(ia64_cpu_montvale_9110n, 1600, 12, 12, 16, 3);
IA64_MONTVALE_VARIANT(ia64_cpu_montvale_9120n, 1420, 6, 12, 16, 3);
IA64_MONTVALE_VARIANT(ia64_cpu_montvale_9130m, 1666, 4, 8, 20, 3);
IA64_MONTVALE_VARIANT(ia64_cpu_montvale_9140m, 1666, 9, 18, 20, 3);
IA64_MONTVALE_VARIANT(ia64_cpu_montvale_9140n, 1600, 9, 18, 16, 3);
IA64_MONTVALE_VARIANT(ia64_cpu_montvale_9150m, 1666, 12, 24, 20, 3);
IA64_MONTVALE_VARIANT(ia64_cpu_montvale_9152m, 1666, 12, 24, 20, 3);

static const IA64CPUVariantDef ia64_cpu_montvale = {
    .semantic_profile_id = 0x6d6f6e0039313530ULL,
    .cpuid_version = 0x0000000020010104ULL,
    .itc_frequency_hz = 400000000U,
    .processor_ratio = IA64_FREQUENCY_RATIO(16, 1),
    .bus_ratio = IA64_FREQUENCY_RATIO(16, 3),
    .itc_ratio = IA64_FREQUENCY_RATIO(4, 1),
};

/*
 * Intel 323169, table 3: E0 CPUID 0020020404; Intel 322821,
 * table 1-1: 50 physical address bits. Execution and PAL controls retain
 * the Montecito profile.
 */
static void ia64_cpu_tukwila_class_init(ObjectClass *oc, const void *data)
{
    IA64CPUClass *icc = IA64_CPU_CLASS(oc);

    ia64_cpu_variant_class_init(oc, data);
    icc->cpuid_version = 0x0000000020020404ULL;
    icc->phys_addr_bits = 50;
    icc->impl_va_msb = 60;
}

#define IA64_TUKWILA_VARIANT(_number, _mhz) \
    static const IA64CPUVariantDef ia64_cpu_tukwila_##_number = { \
        .semantic_profile_id = UINT64_C(0x74756b0000000000) | (_number), \
        .itc_frequency_hz = 400000000U, \
        .processor_ratio = IA64_FREQUENCY_RATIO((_mhz), 100), \
        .bus_ratio = IA64_FREQUENCY_RATIO(24, 1), \
        .itc_ratio = IA64_FREQUENCY_RATIO(4, 1), \
    }

IA64_TUKWILA_VARIANT(9310, 1600);
IA64_TUKWILA_VARIANT(9320, 1333);
IA64_TUKWILA_VARIANT(9330, 1466);
IA64_TUKWILA_VARIANT(9340, 1600);
IA64_TUKWILA_VARIANT(9350, 1733);

static const TypeInfo ia64_cpu_type_info[] = {
    {
        .name = TYPE_IA64_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(IA64CPU),
        .instance_align = __alignof__(IA64CPU),
        .instance_finalize = ia64_cpu_finalize,
        .class_size = sizeof(IA64CPUClass),
        .class_init = ia64_cpu_class_init,
        .abstract = true,
    },
    {
        .name = IA64_CPU_TYPE_NAME("merced-800"),
        .parent = TYPE_IA64_CPU,
        .class_init = ia64_cpu_model_class_init,
        .class_data = &ia64_cpu_model_merced,
    },
    {
        .name = IA64_CPU_TYPE_NAME("madison-base"),
        .parent = TYPE_IA64_CPU,
        .class_init = ia64_cpu_model_class_init,
        .class_data = &ia64_cpu_model_madison,
        .abstract = true,
    },
    {
        .name = IA64_CPU_TYPE_NAME("mckinley-1000"),
        .parent = IA64_CPU_TYPE_NAME("madison-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_mckinley,
    },
    {
        .name = IA64_CPU_TYPE_NAME("mckinley-900"),
        .parent = IA64_CPU_TYPE_NAME("mckinley-1000"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_mckinley_900,
    },
    {
        .name = IA64_CPU_TYPE_NAME("deerfield-1000"),
        .parent = IA64_CPU_TYPE_NAME("madison-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_deerfield,
    },
    {
        .name = IA64_CPU_TYPE_NAME("madison-1400-1.5m"),
        .parent = IA64_CPU_TYPE_NAME("madison-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_madison_1500k,
    },
    {
        .name = IA64_CPU_TYPE_NAME("madison-1600-3m"),
        .parent = IA64_CPU_TYPE_NAME("madison-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_madison_3m,
    },
    {
        .name = IA64_CPU_TYPE_NAME("madison-1400-4m"),
        .parent = IA64_CPU_TYPE_NAME("madison-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_madison_4m,
    },
    {
        .name = IA64_CPU_TYPE_NAME("madison-1500"),
        .parent = IA64_CPU_TYPE_NAME("madison-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_madison_6m,
    },
    {
        .name = IA64_CPU_TYPE_NAME("madison-1600-9m"),
        .parent = IA64_CPU_TYPE_NAME("madison-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_madison_9m,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montecito-base"),
        .parent = TYPE_IA64_CPU,
        .class_init = ia64_cpu_model_class_init,
        .class_data = &ia64_cpu_model_montecito,
        .abstract = true,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montecito-9010"),
        .parent = IA64_CPU_TYPE_NAME("montecito-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montecito_9010,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montecito-9015"),
        .parent = IA64_CPU_TYPE_NAME("montecito-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montecito_9015,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montecito-9020"),
        .parent = IA64_CPU_TYPE_NAME("montecito-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montecito_9020,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montecito-9030"),
        .parent = IA64_CPU_TYPE_NAME("montecito-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montecito_9030,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montecito-9040"),
        .parent = IA64_CPU_TYPE_NAME("montecito-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montecito_9040,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montecito-9050"),
        .parent = IA64_CPU_TYPE_NAME("montecito-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montecito_9050,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montvale-9150n"),
        .parent = IA64_CPU_TYPE_NAME("montecito-base"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montvale,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montvale-9110n"),
        .parent = IA64_CPU_TYPE_NAME("montvale-9150n"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montvale_9110n,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montvale-9120n"),
        .parent = IA64_CPU_TYPE_NAME("montvale-9150n"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montvale_9120n,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montvale-9130m"),
        .parent = IA64_CPU_TYPE_NAME("montvale-9150n"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montvale_9130m,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montvale-9140m"),
        .parent = IA64_CPU_TYPE_NAME("montvale-9150n"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montvale_9140m,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montvale-9140n"),
        .parent = IA64_CPU_TYPE_NAME("montvale-9150n"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montvale_9140n,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montvale-9150m"),
        .parent = IA64_CPU_TYPE_NAME("montvale-9150n"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montvale_9150m,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montvale-9152m"),
        .parent = IA64_CPU_TYPE_NAME("montvale-9150n"),
        .class_init = ia64_cpu_variant_class_init,
        .class_data = &ia64_cpu_montvale_9152m,
    },
    {
        .name = IA64_CPU_TYPE_NAME("tukwila-9310"),
        .parent = IA64_CPU_TYPE_NAME("montecito-base"),
        .class_init = ia64_cpu_tukwila_class_init,
        .class_data = &ia64_cpu_tukwila_9310,
    },
    {
        .name = IA64_CPU_TYPE_NAME("tukwila-9320"),
        .parent = IA64_CPU_TYPE_NAME("montecito-base"),
        .class_init = ia64_cpu_tukwila_class_init,
        .class_data = &ia64_cpu_tukwila_9320,
    },
    {
        .name = IA64_CPU_TYPE_NAME("tukwila-9330"),
        .parent = IA64_CPU_TYPE_NAME("montecito-base"),
        .class_init = ia64_cpu_tukwila_class_init,
        .class_data = &ia64_cpu_tukwila_9330,
    },
    {
        .name = IA64_CPU_TYPE_NAME("tukwila-9340"),
        .parent = IA64_CPU_TYPE_NAME("montecito-base"),
        .class_init = ia64_cpu_tukwila_class_init,
        .class_data = &ia64_cpu_tukwila_9340,
    },
    {
        .name = IA64_CPU_TYPE_NAME("tukwila-9350"),
        .parent = IA64_CPU_TYPE_NAME("montecito-base"),
        .class_init = ia64_cpu_tukwila_class_init,
        .class_data = &ia64_cpu_tukwila_9350,
    },
};

DEFINE_TYPES(ia64_cpu_type_info)
