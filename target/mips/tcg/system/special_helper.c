/*
 *  QEMU MIPS emulation: Special opcode helpers
 *
 *  Copyright (c) 2004-2005 Jocelyn Mayer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/translation-block.h"
#include "internal.h"
#include "../tcg-internal.h"

/* Specials */
target_ulong helper_di(CPUMIPSState *env)
{
    target_ulong t0 = env->CP0_Status;

    env->CP0_Status = t0 & ~(1 << CP0St_IE);
    return t0;
}

target_ulong helper_ei(CPUMIPSState *env)
{
    target_ulong t0 = env->CP0_Status;

    env->CP0_Status = t0 | (1 << CP0St_IE);
    cpu_mips_reassert_interrupt(env);
    return t0;
}

static void debug_pre_eret(CPUMIPSState *env)
{
    if (qemu_loglevel_mask(CPU_LOG_EXEC)) {
        qemu_log("ERET: PC " TARGET_FMT_lx " EPC " TARGET_FMT_lx,
                env->active_tc.PC, env->CP0_EPC);
        if (env->CP0_Status & (1 << CP0St_ERL)) {
            qemu_log(" ErrorEPC " TARGET_FMT_lx, env->CP0_ErrorEPC);
        }
        if (env->hflags & MIPS_HFLAG_DM) {
            qemu_log(" DEPC " TARGET_FMT_lx, env->CP0_DEPC);
        }
        qemu_log("\n");
    }
}

static void debug_post_eret(CPUMIPSState *env)
{
    if (qemu_loglevel_mask(CPU_LOG_EXEC)) {
        qemu_log("  =>  PC " TARGET_FMT_lx " EPC " TARGET_FMT_lx,
                env->active_tc.PC, env->CP0_EPC);
        if (env->CP0_Status & (1 << CP0St_ERL)) {
            qemu_log(" ErrorEPC " TARGET_FMT_lx, env->CP0_ErrorEPC);
        }
        if (env->hflags & MIPS_HFLAG_DM) {
            qemu_log(" DEPC " TARGET_FMT_lx, env->CP0_DEPC);
        }
        switch (mips_env_mmu_index(env)) {
        case 3:
            qemu_log(", ERL\n");
            break;
        case MIPS_HFLAG_UM:
            qemu_log(", UM\n");
            break;
        case MIPS_HFLAG_SM:
            qemu_log(", SM\n");
            break;
        case MIPS_HFLAG_KM:
            qemu_log("\n");
            break;
        default:
            cpu_abort(env_cpu(env), "Invalid MMU mode!\n");
            break;
        }
    }
}

bool mips_io_recompile_replay_branch(CPUState *cs, const TranslationBlock *tb)
{
    CPUMIPSState *env = cpu_env(cs);

    if ((env->hflags & MIPS_HFLAG_BMASK) != 0
        && !tcg_cflags_has(cs, CF_PCREL) && env->active_tc.PC != tb->pc) {
        env->active_tc.PC -= (env->hflags & MIPS_HFLAG_B16 ? 2 : 4);
        env->hflags &= ~MIPS_HFLAG_BMASK;
        return true;
    }
    return false;
}

static void exception_return_mode(CPUMIPSState *env)
{
    if (env->CP0_Status & (1 << CP0St_ERL)) {
        mips_env_set_pc(env, env->CP0_ErrorEPC);
        env->CP0_Status &= ~(1 << CP0St_ERL);
    } else {
        mips_env_set_pc(env, env->CP0_EPC);
        env->CP0_Status &= ~(1 << CP0St_EXL);
    }
}

static inline void exception_return(CPUMIPSState *env)
{
    debug_pre_eret(env);
    if (cpu_mips_r10k_perf_mode_sensitive(env)) {
        /*
         * A live R10000 performance counter changes representation with
         * the mode; the freeze/thaw pair and its lock, as in
         * cpu_mips_store_status().
         */
        BQL_LOCK_GUARD();

        cpu_mips_r10k_perf_freeze(env);
        exception_return_mode(env);
        cpu_mips_r10k_perf_thaw(env);
    } else {
        exception_return_mode(env);
    }
    compute_hflags(env);
    cpu_mips_reassert_interrupt(env);
    debug_post_eret(env);
}

void helper_eret(CPUMIPSState *env)
{
    exception_return(env);
    /*
     * ERET breaks the load-linked link, and the link is env->lladdr. The
     * R10000's LLAddr register is a plain scratch word the link never
     * touches (manual 14.15), so only the older processors' register
     * takes the poison value.
     */
    if (!(env->insn_flags & INSN_R10K)) {
        env->CP0_LLAddr = 1;
    }
    env->lladdr = 1;
}

void helper_eretnc(CPUMIPSState *env)
{
    exception_return(env);
}

void helper_deret(CPUMIPSState *env)
{
    debug_pre_eret(env);

    env->hflags &= ~MIPS_HFLAG_DM;
    compute_hflags(env);
    cpu_mips_reassert_interrupt(env);

    mips_env_set_pc(env, env->CP0_DEPC);

    debug_post_eret(env);
}

void helper_cache(CPUMIPSState *env, target_ulong addr, uint32_t op)
{
    static const char *const type_name[] = {
        "Primary Instruction",
        "Primary Data or Unified Primary",
        "Tertiary",
        "Secondary"
    };
    uint32_t cache_type = extract32(op, 0, 2);
    uint32_t cache_operation = extract32(op, 2, 3);
    target_ulong index = addr & 0x1fffffff;

    if (env->insn_flags & INSN_R10K) {
        /*
         * The primary caches are virtually indexed: VA[13:6] names an
         * instruction-cache set, VA[13:5] a data-cache set, VA[13:2] a word
         * for the data operations, and VA[0] the way throughout. Their index
         * diagnostic operations move whole tag and data entries between the
         * arrays and the TagLo/TagHi/ECC staging registers; the set-wide
         * least-recently-used bit rides in TagLo bit 3 either way.
         */
        if (cache_type == 0 && (cache_operation == 0b001 ||
                                cache_operation == 0b010)) {
            unsigned int way = addr & 1;
            unsigned int set = extract64(addr, 6, 8);
            unsigned int tag = set * R10K_PCACHE_WAYS + way;

            if (cache_operation == 0b010) { /* Index Store Tag (I) */
                env->r10k_icache_taglo[tag] = env->CP0_TagLo &
                    (R10K_ICACHE_TAGLO_MASK & ~R10K_PCACHE_TAGLO_LRU);
                env->r10k_icache_taghi[tag] =
                    env->CP0_TagHi & R10K_ICACHE_TAGHI_MASK;
                env->r10k_icache_lru[set] = extract32(env->CP0_TagLo, 3, 1);
            } else { /* Index Load Tag (I) */
                env->CP0_TagLo = env->r10k_icache_taglo[tag] |
                    (env->r10k_icache_lru[set] << 3);
                env->CP0_TagHi = env->r10k_icache_taghi[tag];
            }
            return;
        }
        if (cache_type == 1 && (cache_operation == 0b001 ||
                                cache_operation == 0b010)) {
            unsigned int way = addr & 1;
            unsigned int set = extract64(addr, 5, 9);
            unsigned int tag = set * R10K_PCACHE_WAYS + way;

            if (cache_operation == 0b010) { /* Index Store Tag (D) */
                env->r10k_dcache_taglo[tag] = env->CP0_TagLo &
                    (R10K_DCACHE_TAGLO_MASK & ~R10K_PCACHE_TAGLO_LRU);
                env->r10k_dcache_taghi[tag] =
                    env->CP0_TagHi & R10K_DCACHE_TAGHI_MASK;
                env->r10k_dcache_lru[set] = extract32(env->CP0_TagLo, 3, 1);
            } else { /* Index Load Tag (D) */
                env->CP0_TagLo = env->r10k_dcache_taglo[tag] |
                    (env->r10k_dcache_lru[set] << 3);
                env->CP0_TagHi = env->r10k_dcache_taghi[tag];
            }
            return;
        }
        if ((cache_type == 0 || cache_type == 1) &&
            (cache_operation == 0b110 || cache_operation == 0b111)) {
            unsigned int way = addr & 1;
            unsigned int entry = extract64(addr, 2, 12) * R10K_PCACHE_WAYS +
                                 way;

            if (cache_type == 0) {
                if (cache_operation == 0b111) { /* Index Store Data (I) */
                    env->r10k_icache_data[entry] =
                        (uint64_t)(env->CP0_TagHi & 0xf) << 32 |
                        (uint32_t)env->CP0_TagLo;
                    env->r10k_icache_parity[entry] = env->CP0_ErrCtl & 1;
                } else { /* Index Load Data (I) */
                    uint64_t data = env->r10k_icache_data[entry];

                    env->CP0_TagLo = (uint32_t)data;
                    env->CP0_TagHi = data >> 32;
                    env->CP0_ErrCtl = env->r10k_icache_parity[entry];
                }
            } else {
                if (cache_operation == 0b111) { /* Index Store Data (D) */
                    env->r10k_dcache_data[entry] = env->CP0_TagLo;
                    env->r10k_dcache_parity[entry] = env->CP0_ErrCtl & 0xf;
                } else { /* Index Load Data (D) */
                    /*
                     * The data-cache word format leaves TagHi "Not Used"
                     * rather than reserved, so the load fills TagLo and the
                     * parity bits and leaves TagHi alone.
                     */
                    env->CP0_TagLo = env->r10k_dcache_data[entry];
                    env->CP0_ErrCtl = env->r10k_dcache_parity[entry];
                }
            }
            return;
        }
        /*
         * The CPU definition fixes the R10000 secondary cache at the Origin
         * 200 reset-mode value: 1 MiB, two ways, and 128-byte blocks.  Its
         * index diagnostic operations use the translated physical address
         * and address bit zero selects the way.
         */
        if (cache_type == 3 && (cache_operation == 0b001 ||
                               cache_operation == 0b010 ||
                               cache_operation == 0b110 ||
                               cache_operation == 0b111)) {
            hwaddr paddr = cpu_mips_translate_address(env, addr, MMU_DATA_LOAD,
                                                      GETPC());
            unsigned int way = paddr & 1;

            if (cache_operation == 0b001 || cache_operation == 0b010) {
                unsigned int set = (paddr / R10K_SCACHE_LINE_SIZE) &
                                   (R10K_SCACHE_SETS - 1);
                unsigned int tag = set * R10K_SCACHE_WAYS + way;

                /* TagHi's MRU bit is set-wide; its tag nibble is per-way. */
                if (cache_operation == 0b010) { /* Index Store Tag */
                    env->r10k_scache_taglo[tag] =
                        env->CP0_TagLo & R10K_SCACHE_TAGLO_MASK;
                    env->r10k_scache_taghi[tag] =
                        env->CP0_TagHi & R10K_SCACHE_TAGHI_MASK;
                    env->r10k_scache_mru[set] =
                        extract32(env->CP0_TagHi, 31, 1);
                } else { /* Index Load Tag */
                    env->CP0_TagLo = env->r10k_scache_taglo[tag];
                    env->CP0_TagHi = env->r10k_scache_taghi[tag] |
                        ((uint32_t)env->r10k_scache_mru[set] << 31);
                }
            } else {
                unsigned int quad_set = (paddr / 16) &
                    (R10K_SCACHE_QUADWORDS / R10K_SCACHE_WAYS - 1);
                unsigned int quad = quad_set * R10K_SCACHE_WAYS + way;
                unsigned int dword = quad * 2 + extract64(paddr, 3, 1);

                if (cache_operation == 0b111) { /* Index Store Data */
                    env->r10k_scache_data[quad * 2] = 0;
                    env->r10k_scache_data[quad * 2 + 1] = 0;
                    env->r10k_scache_data[dword] =
                        (uint64_t)(uint32_t)env->CP0_TagHi << 32 |
                        (uint32_t)env->CP0_TagLo;
                    env->r10k_scache_ecc[quad] = env->CP0_ErrCtl & 0x3ff;
                } else { /* Index Load Data */
                    uint64_t data = env->r10k_scache_data[dword];

                    env->CP0_TagLo = (uint32_t)data;
                    env->CP0_TagHi = data >> 32;
                    env->CP0_ErrCtl = env->r10k_scache_ecc[quad];
                }
            }
        }
        return;
    }

    switch (cache_operation) {
    case 0b010: /* Index Store Tag */
        memory_region_dispatch_write(env->itc_tag, index, env->CP0_TagLo,
                                     MO_64, MEMTXATTRS_UNSPECIFIED);
        break;
    case 0b001: /* Index Load Tag */
        memory_region_dispatch_read(env->itc_tag, index, &env->CP0_TagLo,
                                    MO_64, MEMTXATTRS_UNSPECIFIED);
        break;
    case 0b000: /* Index Invalidate */
    case 0b100: /* Hit Invalidate */
    case 0b110: /* Hit Writeback */
        /* no-op */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "cache operation:%u (type: %s cache)\n",
                      cache_operation, type_name[cache_type]);
        break;
    }
}
