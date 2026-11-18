/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/*
 * IA-64 system-register and processor-state architecture operations.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/atomic.h"
#include "cpu.h"
#include "arch/arch.h"
#include "arch/system.h"
#include "exec/cpu-common.h"
#include "exec/cputlb.h"
#include "exec/tlb-flags.h"
#include "trace.h"

#define IA64_CPUID_VENDOR0           0x49656e69756e6547ULL /* "GenuineI" */
#define IA64_CPUID_VENDOR1           0x000000006c65746eULL /* "ntel" */
#define IA64_CPUID_SERIAL            0x0000000000000000ULL

#define IA64_MERCED_PMD_ADDR_IGNORED_MASK 0x1ff8000000000000ULL

/* PAL_PERF_MON_INFO advertises four generic counter pairs, PMD4 through PMD7. */
#define IA64_LAST_GENERIC_PMD              7

static void ia64_swap_banked_gr(CPUIA64State *env);

enum {
    IA64_PR_ROTATING_COUNT = IA64_PR_COUNT - IA64_PR_ROTATING_BASE,
};

#define IA64_PR_ROTATING_MASK ((1ULL << IA64_PR_ROTATING_COUNT) - 1)

static G_GNUC_NO_INLINE uint32_t ia64_normalize_rrb_pr_slow(uint32_t rrb)
{
    return rrb % IA64_PR_ROTATING_COUNT;
}

static inline uint32_t ia64_normalize_rrb_pr(uint32_t rrb)
{
    if (unlikely(rrb >= IA64_PR_ROTATING_COUNT)) {
        return ia64_normalize_rrb_pr_slow(rrb);
    }
    return rrb;
}

static inline uint64_t ia64_rotl_pr(uint64_t value, uint32_t shift)
{
    value &= IA64_PR_ROTATING_MASK;
    if (shift == 0) {
        return value;
    }
    return ((value << shift) | (value >> (IA64_PR_ROTATING_COUNT - shift))) &
           IA64_PR_ROTATING_MASK;
}

static inline uint64_t ia64_rotr_pr(uint64_t value, uint32_t shift)
{
    value &= IA64_PR_ROTATING_MASK;
    if (shift == 0) {
        return value;
    }
    return ((value >> shift) | (value << (IA64_PR_ROTATING_COUNT - shift))) &
           IA64_PR_ROTATING_MASK;
}

static inline QEMU_ALWAYS_INLINE uint64_t ia64_pack_pr_bit(const uint64_t *pr,
                                                           uint32_t bit)
{
    return (pr[bit] & 1) << bit;
}

static inline uint64_t ia64_pack_pr8(const uint64_t *pr)
{
    return ia64_pack_pr_bit(pr, 0) | ia64_pack_pr_bit(pr, 1) |
           ia64_pack_pr_bit(pr, 2) | ia64_pack_pr_bit(pr, 3) |
           ia64_pack_pr_bit(pr, 4) | ia64_pack_pr_bit(pr, 5) |
           ia64_pack_pr_bit(pr, 6) | ia64_pack_pr_bit(pr, 7);
}

static inline QEMU_ALWAYS_INLINE void
ia64_unpack_pr_bit(uint64_t *pr, uint64_t value, uint32_t bit)
{
    pr[bit] = (value >> bit) & 1;
}

static inline void ia64_unpack_pr8(uint64_t *pr, uint64_t value)
{
    ia64_unpack_pr_bit(pr, value, 0);
    ia64_unpack_pr_bit(pr, value, 1);
    ia64_unpack_pr_bit(pr, value, 2);
    ia64_unpack_pr_bit(pr, value, 3);
    ia64_unpack_pr_bit(pr, value, 4);
    ia64_unpack_pr_bit(pr, value, 5);
    ia64_unpack_pr_bit(pr, value, 6);
    ia64_unpack_pr_bit(pr, value, 7);
}

uint64_t ia64_system_read_pr(const CPUIA64State *env)
{
    uint64_t value;
    uint64_t rotating;
    uint32_t rrb = ia64_normalize_rrb_pr(env->cfm_rrb_pr);

    /*
     * mov r=pr operates as if CFM.rrb.pr were zero.  env->pr[] is the
     * current logical view.  Packing that view and rotating it once is
     * equivalent to reducing every rotating predicate number modulo 48,
     * without doing the reduction in every iteration.
     */
    value = ia64_pack_pr8(&env->pr[IA64_PR_TRUE]) | 1;
    value |= ia64_pack_pr8(&env->pr[IA64_PR_TRUE + 8]) << 8;
    rotating = ia64_pack_pr8(&env->pr[IA64_PR_ROTATING_BASE]);
    rotating |= ia64_pack_pr8(&env->pr[IA64_PR_ROTATING_BASE + 8]) << 8;
    rotating |= ia64_pack_pr8(&env->pr[IA64_PR_ROTATING_BASE + 16]) << 16;
    rotating |= ia64_pack_pr8(&env->pr[IA64_PR_ROTATING_BASE + 24]) << 24;
    rotating |= ia64_pack_pr8(&env->pr[IA64_PR_ROTATING_BASE + 32]) << 32;
    rotating |= ia64_pack_pr8(&env->pr[IA64_PR_ROTATING_BASE + 40]) << 40;

    return value | (ia64_rotl_pr(rotating, rrb) << IA64_PR_ROTATING_BASE);
}

static G_GNUC_NO_INLINE void
ia64_set_cfm_rrb_pr_slow(CPUIA64State *env, uint32_t new_rrb, uint32_t old_rrb)
{
    enum { ROTATING_COUNT = IA64_PR_COUNT - IA64_PR_ROTATING_BASE };
    uint32_t shift;

    if (old_rrb >= ROTATING_COUNT) {
        old_rrb %= ROTATING_COUNT;
    }
    if (new_rrb >= ROTATING_COUNT) {
        new_rrb %= ROTATING_COUNT;
    }
    if (new_rrb == old_rrb) {
        env->cfm_rrb_pr = new_rrb;
        return;
    }

    /*
     * Predicate helpers address env->pr[] by logical register number.
     * Changing RRB.PR must not move the underlying physical predicates,
     * so rebase that logical view just as ia64_set_cfm_rrb_fr() does for
     * rotating floating-point registers.
     */
    shift = new_rrb >= old_rrb ? new_rrb - old_rrb
                               : new_rrb + ROTATING_COUNT - old_rrb;
    if (shift == 1) {
        uint64_t first = env->pr[IA64_PR_ROTATING_BASE];

        memmove(&env->pr[IA64_PR_ROTATING_BASE],
                &env->pr[IA64_PR_ROTATING_BASE + 1],
                (ROTATING_COUNT - 1) * sizeof(env->pr[IA64_PR_TRUE]));
        env->pr[IA64_PR_COUNT - 1] = first;
    } else if (shift == ROTATING_COUNT - 1) {
        uint64_t last = env->pr[IA64_PR_COUNT - 1];

        memmove(&env->pr[IA64_PR_ROTATING_BASE + 1],
                &env->pr[IA64_PR_ROTATING_BASE],
                (ROTATING_COUNT - 1) * sizeof(env->pr[IA64_PR_TRUE]));
        env->pr[IA64_PR_ROTATING_BASE] = last;
    } else {
        uint64_t old_pr[ROTATING_COUNT];
        uint32_t first = ROTATING_COUNT - shift;

        memcpy(old_pr, &env->pr[IA64_PR_ROTATING_BASE], sizeof(old_pr));
        memcpy(&env->pr[IA64_PR_ROTATING_BASE], &old_pr[shift],
               first * sizeof(old_pr[0]));
        memcpy(&env->pr[IA64_PR_ROTATING_BASE + first], old_pr,
               shift * sizeof(old_pr[0]));
    }
    env->cfm_rrb_pr = new_rrb;
    env->pr[IA64_PR_TRUE] = 1;
}

void ia64_set_cfm_rrb_pr(CPUIA64State *env, uint32_t new_rrb)
{
    enum { ROTATING_COUNT = IA64_PR_COUNT - IA64_PR_ROTATING_BASE };
    uint32_t old_rrb = env->cfm_rrb_pr;

    /* br.call and most returns keep the common zero rotation unchanged. */
    if (likely(new_rrb == old_rrb && new_rrb < ROTATING_COUNT)) {
        return;
    }
    ia64_set_cfm_rrb_pr_slow(env, new_rrb, old_rrb);
}

void ia64_rotate_cfm_rrb_pr_right(CPUIA64State *env)
{
    enum { ROTATING_COUNT = IA64_PR_COUNT - IA64_PR_ROTATING_BASE };
    uint32_t old_rrb = env->cfm_rrb_pr;
    uint64_t last = env->pr[IA64_PR_COUNT - 1];

    /*
     * Loop branches always decrement RRB.PR by one.  Keep that hot path out
     * of the arbitrary-rebase helper, whose general case needs a complete
     * predicate snapshot.  env->pr[] is the logical view, so decrementing
     * RRB.PR moves its last rotating predicate to the first logical slot.
     */
    memmove(&env->pr[IA64_PR_ROTATING_BASE + 1],
            &env->pr[IA64_PR_ROTATING_BASE],
            (ROTATING_COUNT - 1) * sizeof(env->pr[IA64_PR_TRUE]));
    env->pr[IA64_PR_ROTATING_BASE] = last;
    old_rrb = ia64_normalize_rrb_pr(old_rrb);
    env->cfm_rrb_pr = old_rrb ? old_rrb - 1 : ROTATING_COUNT - 1;
    env->pr[IA64_PR_TRUE] = 1;
}

void ia64_system_epc(CPUIA64State *env, uint64_t fault_ip, uint64_t raw,
                     uint32_t fault_slot)
{
    uint8_t current_cpl = ia64_psr_cpl(env->psr);
    uint8_t pfs_ppl = (env->ar_pfs & IA64_PFS_PPL_MASK) >> IA64_PFS_PPL_SHIFT;
    uint8_t new_cpl = current_cpl;

    if (pfs_ppl < current_cpl) {
        ia64_raise_exception(env, IA64_EXCP_ILLEGAL, fault_ip, raw, fault_slot);
    }

    if (env->psr & IA64_PSR_IT) {
        uint32_t rid = ia64_region_rid(env, fault_ip);

        for (uint16_t i = 0; i < env->mmu.tlb_inst_count; i++) {
            IA64TlbEntry *entry = &env->mmu.tlb_inst[i];

            if (ia64_tlb_match(entry, fault_ip, rid) && entry->ar == 7 &&
                entry->pl < current_cpl) {
                new_cpl = entry->pl;
                break;
            }
        }
    } else {
        new_cpl = 0;
    }

    ia64_set_psr(env, (env->psr & ~IA64_PSR_CPL_MASK) |
                          ((uint64_t)new_cpl << IA64_PSR_CPL_SHIFT));
}

void ia64_system_write_pr(CPUIA64State *env, uint64_t value, uint64_t mask)
{
    uint64_t nonrotating_mask = mask & 0xfffe;
    uint64_t rotating_mask = mask >> IA64_PR_ROTATING_BASE;
    uint64_t rotating_value = value >> IA64_PR_ROTATING_BASE;
    uint32_t rrb = ia64_normalize_rrb_pr(env->cfm_rrb_pr);

    if (nonrotating_mask == 0xfffe) {
        /* Context restores commonly replace every static predicate. */
        ia64_unpack_pr8(&env->pr[IA64_PR_TRUE], value);
        ia64_unpack_pr8(&env->pr[IA64_PR_TRUE + 8], value >> 8);
    } else {
        while (nonrotating_mask) {
            uint32_t logical = ctz64(nonrotating_mask);

            nonrotating_mask &= nonrotating_mask - 1;
            env->pr[logical] = (value >> logical) & 1;
        }
    }

    /* Convert the physical mask and value to env->pr[]'s logical view. */
    rotating_mask = ia64_rotr_pr(rotating_mask, rrb);
    rotating_value = ia64_rotr_pr(rotating_value, rrb);
    if (rotating_mask == IA64_PR_ROTATING_MASK) {
        /* mov pr.rot and firmware context restores use this dense mask. */
        ia64_unpack_pr8(&env->pr[IA64_PR_ROTATING_BASE], rotating_value);
        ia64_unpack_pr8(&env->pr[IA64_PR_ROTATING_BASE + 8],
                        rotating_value >> 8);
        ia64_unpack_pr8(&env->pr[IA64_PR_ROTATING_BASE + 16],
                        rotating_value >> 16);
        ia64_unpack_pr8(&env->pr[IA64_PR_ROTATING_BASE + 24],
                        rotating_value >> 24);
        ia64_unpack_pr8(&env->pr[IA64_PR_ROTATING_BASE + 32],
                        rotating_value >> 32);
        ia64_unpack_pr8(&env->pr[IA64_PR_ROTATING_BASE + 40],
                        rotating_value >> 40);
    } else {
        while (rotating_mask) {
            uint32_t logical = ctz64(rotating_mask);

            rotating_mask &= rotating_mask - 1;
            env->pr[IA64_PR_ROTATING_BASE + logical] =
                (rotating_value >> logical) & 1;
        }
    }
    env->pr[IA64_PR_TRUE] = 1;
}

uint64_t ia64_system_read_ar(CPUIA64State *env, uint32_t ar_num)
{
    if (ar_num >= IA64_AR_COUNT) {
        return 0;
    }
    if (ar_num == 17) {
        return env->ar_bsp;
    }
    if (ar_num == IA64_AR_RNAT) {
        uint64_t value = ia64_rse_read_rnat(env);

        /* Undefined bits read back the last write while nothing has moved. */
        if (env->rse.rnat_sw_valid &&
            env->rse.rnat_sw_bsp == env->ar_bspstore &&
            env->rse.rnat_sw_base == value) {
            value |= env->rse.rnat_sw_value & ~ia64_rse_rnat_visible_mask(env);
        }
        return value;
    }
    if (ar_num == 44) {
        return ia64_itc_read(env);
    }
    return env->ar[ar_num];
}

static bool ia64_reserved_rsc_field(uint64_t value)
{
    return value & ~(0x1fULL | (0x3fffULL << IA64_RSC_LOADRS_SHIFT));
}

static bool ia64_reserved_fpsr_field(uint64_t value)
{
    return (value >> 58) != 0 || ((value >> 12) & 1) != 0 ||
           ((value >> 47) & 3) == 1 || ((value >> 34) & 3) == 1 ||
           ((value >> 21) & 3) == 1 || ((value >> 8) & 3) == 1;
}

static bool ia64_reserved_pfs_field(uint64_t value)
{
    return (value & (0xfULL << 58)) || (value & (0x3fffULL << 38));
}

static void ia64_system_check_virtualization(CPUIA64State *env,
                                             uint64_t fault_ip, uint64_t raw,
                                             uint32_t slot)
{
    if (ia64_env_cpu_class(env)->has_virtualization &&
        (env->psr & IA64_PSR_VM)) {
        ia64_raise_exception(env, IA64_EXCP_VIRTUALIZATION, fault_ip, raw,
                             slot);
    }
}

void ia64_system_validate_ar_access(CPUIA64State *env, uint64_t value,
                                    uint32_t ar_num, uint32_t write,
                                    uint64_t fault_ip, uint64_t raw,
                                    uint32_t slot)
{
    if ((ar_num == 18 || ar_num == 19) && (env->ar_rsc & IA64_RSC_MODE)) {
        env->cr_isr = 0;
        ia64_raise_exception(env, IA64_EXCP_ILLEGAL, fault_ip, raw, slot);
    }

    if (!write) {
        if ((ar_num == IA64_AR_ITC || ar_num == IA64_AR_RUC) &&
            (env->psr & IA64_PSR_SI) && ia64_psr_cpl(env->psr) != 0) {
            env->cr_isr = 0x20;
            ia64_raise_exception(env, IA64_EXCP_PRIVILEGED_REG, fault_ip, raw,
                                 slot);
        }
        if ((ar_num == IA64_AR_ITC || ar_num == IA64_AR_RUC) &&
            (env->psr & IA64_PSR_SI)) {
            ia64_system_check_virtualization(env, fault_ip, raw, slot);
        }
        return;
    }

    if ((ar_num == 16 && ia64_reserved_rsc_field(value)) ||
        (ar_num == 40 && ia64_reserved_fpsr_field(value)) ||
        (ar_num == IA64_AR_CFLG && (value >> 43)) ||
        (ar_num == 64 && ia64_reserved_pfs_field(value))) {
        qemu_log_mask(CPU_LOG_INT,
                      "ia64 reserved AR field ip=%016" PRIx64
                      " ar=%u value=%016" PRIx64 " raw=%011" PRIx64
                      " slot=%u\n",
                      fault_ip, ar_num, value, raw, slot);
        env->cr_isr = 0x30;
        ia64_raise_exception(env, IA64_EXCP_RESERVED_REG_FIELD, fault_ip, raw,
                             slot);
    }
    if ((ar_num <= IA64_AR_KR7 || ar_num == IA64_AR_CFLG ||
         ar_num == IA64_AR_ITC || ar_num == IA64_AR_RUC) &&
        ia64_psr_cpl(env->psr) != 0) {
        env->cr_isr = 0x20;
        ia64_raise_exception(env, IA64_EXCP_PRIVILEGED_REG, fault_ip, raw,
                             slot);
    }
    if (ar_num == IA64_AR_ITC || ar_num == IA64_AR_RUC) {
        ia64_system_check_virtualization(env, fault_ip, raw, slot);
    }
}

void ia64_system_write_ar(CPUIA64State *env, uint32_t ar_num, uint64_t value)
{
    uint64_t old_value;
    uint64_t rnat = 0;

    if (ar_num >= IA64_AR_COUNT) {
        return;
    }
    if ((ar_num >= 48 && ar_num <= 63) || ar_num >= 112) {
        return;
    }
    if (ar_num == 44) {
        ia64_itc_write(env, value);
        return;
    }
    if (ar_num == 16) {
        uint8_t pl = MAX(ia64_rsc_pl(value), ia64_psr_cpl(env->psr));
        uint64_t old_rsc = env->ar_rsc;

        env->ar_rsc =
            (value & ~IA64_RSC_PL) | ((uint64_t)pl << IA64_RSC_PL_SHIFT);
        if ((old_rsc ^ env->ar_rsc) & IA64_RSC_PL) {
            /* The RSE MMU index is never used for instruction fetches. */
            tlb_flush_by_mmuidx(env_cpu(env), 1u << MMU_IDX_RSE);
        }
        return;
    }
    if (ar_num == 19) {
        value &= INT64_MAX;
    } else if (ar_num == 18) {
        value &= ~7ULL;
    } else if (ar_num == 66) {
        value &= 0x3f;
    } else if (ar_num == IA64_AR_EFLAG) {
        uint8_t cpl = ia64_psr_cpl(env->psr);
        uint64_t preserve = cpl ? (3ULL << 12) | (1ULL << 17) : 0;

        if (cpl > ((env->ar_eflag >> 12) & 3)) {
            preserve |= 1ULL << 9;
        }
        value = (value & ~preserve) | (env->ar_eflag & preserve);
    }
    if (ar_num == 18) {
        rnat = ia64_system_read_ar(env, IA64_AR_RNAT);
    }
    old_value = env->ar[ar_num];
    env->ar[ar_num] = value;
    if (ar_num == IA64_AR_KR3 && value != old_value) {
        /*
         * IA-64 operating systems use KR3 as the physical base from which
         * their alternate DTLB miss handler constructs the per-CPU mapping.
         * QEMU's deterministic TC can otherwise retain a mapping made from
         * the old base indefinitely.  Hardware may replace TC entries at any
         * time, so trigger that legal replacement event when the base moves;
         * translation-register entries remain intact.
         */
        ia64_mmu_invalidate_tc(env);
    }
    if (ar_num == 19) {
        /*
         * Software supplies RNAT for BSPSTORE's group.  RNATBitIndex
         * determines the defined low subset; higher bits are undefined.
         */
        ia64_rse_rnat_reloaded(env);
        env->rse.rnat_sw_value = value & INT64_MAX;
        env->rse.rnat_sw_base = ia64_rse_read_rnat(env);
        env->rse.rnat_sw_bsp = env->ar_bspstore;
        env->rse.rnat_sw_valid = true;
    }
    if (ar_num == 18) {
        /*
         * mov-to-BSPSTORE (SDM Vol.2 6.5.3): the clean partition
         * empties and the dirty partition is preserved by rebasing
         * AR.BSP to the new address plus the dirty registers and their
         * intervening NaT collections.  No memory traffic occurs.
         * RNAT becomes architecturally undefined; it keeps its value here
         * because Linux's kernel-mode break entry writes RNAT before BSPSTORE.
         */
        int32_t dirty = MAX(env->rse.rse_dirty, 0);

        ia64_rse_rnat_undefined(env, "mov-bspstore");
        env->rse.rse_dirty_nat = ia64_rse_nat_words_grow(value, dirty);
        env->ar_bsp =
            value + (uint64_t)(env->rse.rse_dirty + env->rse.rse_dirty_nat) * 8;
        env->rse.rse_invalid += env->rse.rse_clean;
        env->rse.rse_clean = 0;
        env->rse.rse_clean_nat = 0;
        ia64_system_reset_dahr(env);
        ia64_system_write_ar(env, IA64_AR_RNAT, rnat);
        ia64_rse_check(env, "bspstore");
    }
}

uint64_t ia64_system_read_cr(CPUIA64State *env, uint32_t cr_num)
{
    if (cr_num >= IA64_CR_COUNT) {
        return 0;
    }
    switch (cr_num) {
    case IA64_CR_SAPIC_LID:
        return qatomic_read(&env->cr[cr_num]) &
               (IA64_SAPIC_LID_ID_MASK | IA64_SAPIC_LID_EID_MASK);
    case IA64_CR_SAPIC_IVR:
        return (uint64_t)ia64_sapic_get_ivr(env) & 0xFF;
    case IA64_CR_SAPIC_IRR0:
        return env->interrupt.sapic_irr[0];
    case IA64_CR_SAPIC_IRR1:
        return env->interrupt.sapic_irr[1];
    case IA64_CR_SAPIC_IRR2:
        return env->interrupt.sapic_irr[2];
    case IA64_CR_SAPIC_IRR3:
        return env->interrupt.sapic_irr[3];
    default:
        return env->cr[cr_num];
    }
}

static bool ia64_reserved_ipsr_field(CPUIA64State *env, uint64_t value)
{
    uint64_t high_reserved = ia64_env_cpu_class(env)->has_virtualization
                                 ? (value >> 47)
                                 : (value >> 46);

    return high_reserved != 0 || ((value >> 41) & 3) == 3 ||
           ((value >> 28) & 0xf) != 0 || ((value >> 16) & 1) != 0 ||
           ((value >> 6) & 0x7f) != 0 || (value & 1) != 0;
}

static bool ia64_reserved_ifs_field(uint64_t value)
{
    if (value & (0x1ffffffULL << 38)) {
        return true;
    }
    return (value >> 63) && ia64_reserved_pfs_field(value);
}

static bool ia64_reserved_cr_field(CPUIA64State *env, uint32_t cr_num,
                                   uint64_t value)
{
    switch (cr_num) {
    case 0:
        return (value >> 15) != 0 || (value & (0x1fULL << 3));
    case 8: {
        uint8_t ps = (value >> 2) & 0x3f;

        return (value & (0x3fULL << 9)) || (value & 2) || ps < 15;
    }
    case 16:
        return ia64_reserved_ipsr_field(env, value);
    case 17:
        return (value >> 44) != 0 || ((value >> 41) & 3) == 3 ||
               ((value >> 24) & 0xff) != 0;
    case 23:
        return ia64_reserved_ifs_field(value);
    case IA64_CR_SAPIC_LID:
        return (value & 0xffff) != 0;
    case IA64_CR_SAPIC_IVR:
        return (value & 0xff) != 0;
    case IA64_CR_SAPIC_TPR:
        return (value & 0xff00) != 0;
    case IA64_CR_ITV:
    case 73:
    case 74:
        return (value & (7ULL << 13)) || (value & (0xfULL << 8));
    case 80:
    case 81: {
        uint8_t delivery_mode = (value >> 8) & 7;

        return (value & (1ULL << 14)) || (value & (1ULL << 11)) ||
               delivery_mode == 1 || delivery_mode == 3 || delivery_mode == 6;
    }
    default:
        return false;
    }
}

uint64_t ia64_system_validate_cr_access(CPUIA64State *env, uint64_t value,
                                        uint32_t cr_num, uint32_t write,
                                        uint64_t fault_ip, uint64_t raw,
                                        uint32_t slot)
{
    if ((env->psr & IA64_PSR_IC) && cr_num >= 16 && cr_num <= 25) {
        env->cr_isr = 0;
        ia64_raise_exception(env, IA64_EXCP_ILLEGAL, fault_ip, raw, slot);
    }
    if (write && ia64_reserved_cr_field(env, cr_num, value)) {
        qemu_log_mask(CPU_LOG_INT | LOG_GUEST_ERROR,
                      "ia64 reserved cr write cr%u value=%016" PRIx64
                      " ip=%016" PRIx64 " raw=%016" PRIx64
                      " slot=%u psr=%016" PRIx64 "\n",
                      cr_num, value, fault_ip, raw, slot, env->psr);
        env->cr_isr = 0x30;
        ia64_raise_exception(env, IA64_EXCP_RESERVED_REG_FIELD, fault_ip, raw,
                             slot);
    }
    if (write && cr_num == IA64_CR_IFA && !ia64_va_is_implemented(env, value)) {
        env->cr_ifa = value;
        env->cr_isr = IA64_GENEX_UNIMPL_DATA_ADDR | IA64_ISR_NA;
        if (ia64_current_code_tlb_ed(env)) {
            env->cr_isr |= IA64_ISR_ED;
        }
        ia64_raise_exception(env, IA64_EXCP_UNIMPL_DATA_ADDR, fault_ip, raw,
                             slot);
    }
    ia64_system_check_virtualization(env, fault_ip, raw, slot);

    switch (cr_num) {
    case 2:
        return value & ~0x7fffULL;
    case 25:
        return value & ~3ULL;
    case IA64_CR_SAPIC_TPR:
        return value & 0x100f0;
    case IA64_CR_SAPIC_EOI:
        return 0;
    case IA64_CR_ITV:
    case 73:
    case 74:
    case 80:
    case 81:
        return value & 0x1efff;
    default:
        return value;
    }
}

uint64_t ia64_system_read_cpuid(CPUIA64State *env, uint64_t index)
{
    IA64CPUClass *icc = ia64_env_cpu_class(env);

    index &= 0xff;
    switch (index) {
    case 0:
        return IA64_CPUID_VENDOR0;
    case 1:
        return IA64_CPUID_VENDOR1;
    case 2:
        return IA64_CPUID_SERIAL;
    case 3:
        return icc->cpuid_version;
    case 4:
        return icc->cpuid_features;
    default:
        return 0;
    }
}

uint64_t ia64_system_read_dahr_indexed(CPUIA64State *env, uint64_t index)
{
    return env->dahr[index & 7] & 0x7ff;
}

void ia64_system_write_dahr(CPUIA64State *env, uint32_t index, uint64_t value)
{
    env->dahr[index & 7] = value & 0x7ff;
}

void ia64_system_reset_dahr(CPUIA64State *env)
{
    /*
     * The field values that best implement each generic locality hint are
     * implementation dependent.  This model's valid default is all zeroes.
     */
    memset(env->dahr, 0, sizeof(env->dahr));
}

static void ia64_log_unimplemented_msr(CPUIA64State *env, uint64_t index,
                                       uint64_t value, bool write)
{
    IA64CPU *cpu = env_archcpu(env);

    /* MSR 1225 is modelled as storage, so it is not an unimplemented MSR. */
    if (index == IA64_MSR_BRIA_GATE) {
        trace_ia64_msr_gate_access(CPU(cpu)->cpu_index, index, value, write,
                                   env->ip, env->exception_state.fault_slot);
        return;
    }
    trace_ia64_unimplemented_msr(CPU(cpu)->cpu_index, index, value, write,
                                 env->ip, env->exception_state.fault_slot);
    qemu_log_mask(LOG_UNIMP, "ia64: unimplemented MSR %s index=%" PRIu64
                  " value=0x%016" PRIx64 " ip=0x%016" PRIx64 "\n",
                  write ? "write" : "read", index, value, env->ip);
}

/*
 * Itanium 2 PMC/PMD bits that hold a value: PMC4-7 per Figure 10-11 and PMC8/9
 * per Figure 10-14 of 251110-003, the rest as the PAL's register test reads.
 */
static uint64_t ia64_madison_pmc_mask(uint32_t index)
{
    switch (index) {
    case 0:
        return 0xf1;
    case 1 ... 3:
        return 0;
    case 4:
        return 0x0fffff7f;
    case 5 ... 7:
        return 0x0f7fff7f;
    case 8:
        return 0xffffffff3fffffffULL;
    case 9:
        return 0xffffffff3ffffffcULL;
    case 11:
        return 0x3ffffff;
    case 12:
        return 0xffcf;
    default:
        return UINT64_MAX;
    }
}

static uint64_t ia64_madison_pmd_mask(uint32_t index)
{
    switch (index) {
    case 1:
        return 0x1fff;
    case 3:
        return 0xffff;
    case 16:
        return 0xfffffffffULL;
    default:
        return UINT64_MAX;
    }
}

/* cpuid[3] revision byte (0 reads as 1) of 4 to 8, as the PAL tests it. */
static bool ia64_cpu_revision_has_answer(IA64CPU *cpu)
{
    unsigned rev = (IA64_CPU_GET_CLASS(cpu)->cpuid_version >> 8) & 0xff;

    return MAX(rev, 1) >= 4 && MAX(rev, 1) <= 8;
}

/*
 * Estimated: an MSR 1730 write with strobe bit 48 latches the MSR 1728-selected
 * nibble to 0xf (bit 49 set) or 0; read from the PAL's training loops.
 */
static uint64_t ia64_msr1730_latch(CPUIA64State *env, uint64_t value)
{
    IA64CPU *cpu = env_archcpu(env);
    uint64_t armed = cpu->msr[IA64_MSR_TRAIN_CTL];
    uint64_t fill = (value & IA64_MSR_TRAIN_STROBE_B) ? 0xf : 0;
    uint64_t out = value;

    if (!(value & IA64_MSR_TRAIN_STROBE_A)) {
        return value;
    }
    if (armed & IA64_MSR_TRAIN_STROBE_A) {
        out = (out & ~0xfULL) | fill;
    }
    if (armed & IA64_MSR_TRAIN_STROBE_B) {
        out = (out & ~0xf00ULL) | (fill << 8);
    }
    warn_report_once("ia64: MSR 1730 training nibbles jump to the end state of"
                     " the strobe loop (an estimate: the servo loop is not"
                     " described)");
    trace_ia64_msr_estimated_completion(CPU(cpu)->cpu_index,
                                        IA64_MSR_TRAIN_STATUS, out, env->ip);
    return out;
}

uint64_t ia64_system_peek_msr(CPUIA64State *env, uint64_t index)
{
    IA64CPU *cpu = env_archcpu(env);

    return index == IA64_MSR_BRIA_GATE ? env->msr_bria_gate
                                       : cpu->msr[index];
}

/* Firmware-keyed MSR answers: estimates for a Madison and its PAL only. */
static uint64_t ia64_madison_msr_read(CPUIA64State *env, uint64_t index,
                                      uint64_t value)
{
    IA64CPU *cpu = env_archcpu(env);

    if (cpu->msr1212_complete && index == IA64_MSR_CMD_ANSWER &&
        ia64_cpu_revision_has_answer(cpu)) {
        value = IA64_MSR_CMD_ANSWER_1226;
        trace_ia64_msr_estimated_completion(CPU(cpu)->cpu_index, index, value,
                                            env->ip);
    }
    if (index == IA64_MSR_CLOCK_RATIO) {
        IA64CPUClass *icc = IA64_CPU_GET_CLASS(cpu);
        uint64_t ratio = MIN(icc->itc_frequency_hz / icc->frequency_base_hz,
                             0x1fU);

        /* Bits 21:19 name the front-side-bus agent: the slice, halved. */
        uint64_t agent = ((uint64_t)cpu->sapic_id >> 7 & 0xf) >> 1;

        value = (value & ~(IA64_MSR_CLOCK_RATIO_MASK | (7ULL << 19))) |
                (ratio << IA64_MSR_CLOCK_RATIO_SHIFT) | (agent << 19);
        warn_report_once("ia64: MSR 1559 reports a clock ratio of %" PRIu64
                         " (an estimate: the register is unknown)", ratio);
        trace_ia64_msr_estimated_completion(CPU(cpu)->cpu_index, index, value,
                                            env->ip);
    }
    if (index == IA64_MSR_L3_WAYS && IA64_CPU_GET_CLASS(cpu)->l3_kib) {
        /* Bit i of bits 59:48 hides ways 2i and 2i+1; ways are 256 KiB each. */
        unsigned clear = MIN(IA64_CPU_GET_CLASS(cpu)->l3_kib / 512, 12);
        uint64_t field = (0xfffULL << clear) & 0xfff;

        value = (value & ~(0xfffULL << 48)) | field << 48;
        warn_report_once("ia64: MSR 1536 bits 59:48 report the L3 ways of the"
                         " modelled part (an estimate: the register is"
                         " unknown)");
        trace_ia64_msr_estimated_completion(CPU(cpu)->cpu_index, index, value,
                                            env->ip);
    }
    if (index == IA64_MSR_CMD1552 && cpu->msr1552_written) {
        value |= IA64_MSR_CMD1552_DONE;
        warn_report_once("ia64: MSR 1552 bit 2 reads set once written (an"
                         " estimate: the register is unknown)");
        trace_ia64_msr_estimated_completion(CPU(cpu)->cpu_index, index, value,
                                            env->ip);
    }
    if (index == IA64_MSR_CMD_STATUS && cpu->msr1212_complete) {
        if (ia64_cpu_revision_has_answer(cpu)) {
            value = (value & ~0x3fffULL) | IA64_MSR_CMD_ANSWER_LOW;
        }
        value |= IA64_MSR_CMD_DONE_BIT;
        warn_report_once("ia64: MSR 1212 completion bit and its known answer"
                         " are estimates (the register is unknown)");
        trace_ia64_msr_estimated_completion(CPU(cpu)->cpu_index, index, value,
                                            env->ip);
    }
    return value;
}

/* The write-side counterpart: returns the value to store. */
static uint64_t ia64_madison_msr_write(CPUIA64State *env, uint64_t index,
                                       uint64_t value)
{
    IA64CPU *cpu = env_archcpu(env);

    if (index == IA64_MSR_TRAIN_STATUS) {
        value = ia64_msr1730_latch(env, value);
    }
    cpu->msr1552_written |= index == IA64_MSR_CMD1552;
    if (index == IA64_MSR_CMD_PORT &&
        (value >> 48) == IA64_MSR_CMD_CLASS4_TOP) {
        cpu->msr1212_complete = true;
    } else if (index == IA64_MSR_CMD_STATUS) {
        cpu->msr1212_complete = false;
    }
    return value;
}

uint64_t ia64_system_read_msr(CPUIA64State *env, uint64_t index)
{
    IA64CPU *cpu = env_archcpu(env);
    uint64_t value;

    if (index >= IA64_MSR_COUNT) {
        ia64_log_unimplemented_msr(env, index, 0, false);
        return 0;
    }
    value = index == IA64_MSR_BRIA_GATE ? env->msr_bria_gate : cpu->msr[index];

    if (ia64_env_cpu_class(env)->model == IA64_CPU_MODEL_MADISON) {
        value = ia64_madison_msr_read(env, index, value);
    }
    ia64_log_unimplemented_msr(env, index, value, false);
    return value;
}

/* Keep value as the MSR's stored (write-through) contents. */
void ia64_system_store_msr(CPUIA64State *env, uint64_t index, uint64_t value)
{
    env_archcpu(env)->msr[index] = value;
}

void ia64_system_write_msr(CPUIA64State *env, uint64_t index, uint64_t value)
{
    uint64_t old;

    if (index >= IA64_MSR_COUNT) {
        ia64_log_unimplemented_msr(env, index, value, true);
        return;
    }
    if (index >= IA64_MSR_WRITE_ONLY_FIRST &&
        index <= IA64_MSR_WRITE_ONLY_LAST) {
        trace_ia64_msr_write_accepted(CPU(env_archcpu(env))->cpu_index, index,
                                      value, env->ip);
        return;
    }
    ia64_log_unimplemented_msr(env, index, value, true);
    if (index == IA64_MSR_BRIA_GATE) {
        env->msr_bria_gate = value;
        return;
    }
    /* Write-through storage: a read returns the last value written. */
    if (ia64_env_cpu_class(env)->model == IA64_CPU_MODEL_MADISON) {
        value = ia64_madison_msr_write(env, index, value);
    }
    old = index == IA64_MSR_L3_ENABLE ? ia64_system_peek_msr(env, index) : 0;
    ia64_system_store_msr(env, index, value);
    if (index == IA64_MSR_CMD1552 && (value & IA64_MSR_CMD1552_START)) {
        ia64_cache_array_restart(env);
    }
    if (index == IA64_MSR_L3_ENABLE) {
        ia64_cache_array_msr1504(env, old, value);
    }
}

uint64_t ia64_system_read_dbr(CPUIA64State *env, uint32_t index)
{
    index &= 0xff;
    if (index >= IA64_DBR_IMPLEMENTED_COUNT) {
        return 0;
    }
    return env->dbr[index];
}

void ia64_system_write_dbr(CPUIA64State *env, uint32_t index, uint64_t value)
{
    index &= 0xff;
    if (index < IA64_DBR_IMPLEMENTED_COUNT) {
        if (index & 1) {
            value &= ~(3ULL << 60);
        }
        env->dbr[index] = value;
        ia64_debug_armed(env);
    }
}

/* Cache whether any breakpoint is enabled, so debug checks can skip cheaply. */
void ia64_debug_armed(CPUIA64State *env)
{
    env->ibr_armed = false;
    env->dbr_armed = false;
    for (unsigned i = 1; i < IA64_IBR_IMPLEMENTED_COUNT; i += 2) {
        env->ibr_armed |= (env->ibr[i] >> 63) & 1;
    }
    for (unsigned i = 1; i < IA64_DBR_IMPLEMENTED_COUNT; i += 2) {
        env->dbr_armed |= (env->dbr[i] >> 62) != 0;
    }
}

uint64_t ia64_system_read_ibr(CPUIA64State *env, uint32_t index)
{
    index &= 0xff;
    if (index >= IA64_IBR_IMPLEMENTED_COUNT) {
        return 0;
    }
    return env->ibr[index];
}

void ia64_system_write_ibr(CPUIA64State *env, uint32_t index, uint64_t value)
{
    index &= 0xff;
    if (index < IA64_IBR_IMPLEMENTED_COUNT) {
        if (index & 1) {
            value &= ~(7ULL << 60);
        }
        env->ibr[index] = value;
        ia64_debug_armed(env);
    }
}

void ia64_write_cr(CPUIA64State *env, uint32_t cr_num, uint64_t value)
{
    if (cr_num >= IA64_CR_COUNT) {
        return;
    }
    switch (cr_num) {
    case 1:
        env->cr[IA64_CR_ITM] = value;
        ia64_itm_update(env);
        break;
    case 2:
        env->cr[IA64_CR_IVA] = value;
        break;
    case 8:
        if (env->cr[IA64_CR_PTA] == value) {
            break;
        }
        env->cr[IA64_CR_PTA] = value;
        ia64_tlb_bump_generation(env, false);
        ia64_tlb_bump_generation(env, true);
        tlb_flush(env_cpu(env));
        break;
    case IA64_CR_SAPIC_TPR:
        env->cr[cr_num] = value & IA64_TPR_WRITABLE_MASK;
        ia64_sapic_update_interrupt(env);
        break;
    case IA64_CR_SAPIC_LID:
        qatomic_set(&env->cr[cr_num],
                    value & (IA64_SAPIC_LID_ID_MASK | IA64_SAPIC_LID_EID_MASK));
        break;
    case IA64_CR_SAPIC_EOI:
        ia64_sapic_eoi(env);
        break;
    case IA64_CR_SAPIC_IVR:
        break;
    case IA64_CR_SAPIC_IRR0:
    case IA64_CR_SAPIC_IRR1:
    case IA64_CR_SAPIC_IRR2:
    case IA64_CR_SAPIC_IRR3:
        break;
    case IA64_CR_ITV:
        env->cr[cr_num] = value;
        ia64_itm_update(env);
        break;
    default:
        env->cr[cr_num] = value;
        break;
    }
}

uint64_t ia64_system_read_pmc(CPUIA64State *env, uint32_t index)
{
    IA64CPUClass *icc = ia64_env_cpu_class(env);
    uint64_t value;

    if (index >= IA64_PMC_COUNT ||
        !(icc->implemented_pmc_mask & (1ULL << index))) {
        return 0;
    }
    value = env->pmc[index];
    if (icc->model == IA64_CPU_MODEL_MADISON) {
        return value & ia64_madison_pmc_mask(index);
    }
    if (icc->model == IA64_CPU_MODEL_MONTECITO) {
        switch (index) {
        case 0:
            return value & 0xfff1;
        case 1 ... 3:
            return 0;
        default:
            return value;
        }
    }
    if (icc->model != IA64_CPU_MODEL_MERCED) {
        return value;
    }

    switch (index) {
    case 0:
        return value & 0xf1;
    case 1 ... 3:
        return 0;
    case 4 ... 5:
        return value & 0x037f7f7f;
    case 6 ... 7:
        return value & 0x033f7f7f;
    case 8 ... 9:
        return value & 0xfffffffe3ffffff8ULL;
    case 10:
        return value & 0x030f00cf;
    case 11:
        return value & 0x130f00cf;
    case 12:
        return value & 0x0000ffcf;
    case 13:
        return value & 1;
    default:
        return 0;
    }
}

void ia64_system_write_pmc(CPUIA64State *env, uint32_t index, uint64_t value)
{
    IA64CPUClass *icc = ia64_env_cpu_class(env);

    if (index >= IA64_PMC_COUNT ||
        !(icc->implemented_pmc_mask & (1ULL << index))) {
        return;
    }
    if (icc->model == IA64_CPU_MODEL_MADISON) {
        if (!ia64_madison_pmc_mask(index)) {
            return;
        }
        value &= ia64_madison_pmc_mask(index);
    } else if (icc->model == IA64_CPU_MODEL_MONTECITO) {
        switch (index) {
        case 0:
            value &= 0xfff1;
            break;
        case 1 ... 3:
            return;
        default:
            break;
        }
    } else if (icc->model == IA64_CPU_MODEL_MERCED) {
        switch (index) {
        case 0:
            value &= 0xf1;
            break;
        case 1 ... 3:
            return;
        case 4 ... 5:
            value &= 0x037f7f7f;
            break;
        case 6 ... 7:
            value &= 0x033f7f7f;
            break;
        case 8 ... 9:
            value &= 0xfffffffe3ffffff8ULL;
            break;
        case 10:
            value &= 0x030f00cf;
            break;
        case 11:
            value &= 0x130f00cf;
            break;
        case 12:
            value &= 0x0000ffcf;
            break;
        case 13:
            value &= 1;
            break;
        default:
            return;
        }
    }
    env->pmc[index] = value;
}

uint64_t ia64_system_read_pmc_indexed(CPUIA64State *env, uint64_t index)
{
    index &= 0xff;
    return ia64_system_read_pmc(env, index);
}

void ia64_system_write_pmc_indexed(CPUIA64State *env, uint64_t index,
                                   uint64_t value)
{
    index &= 0xff;
    ia64_system_write_pmc(env, index, value);
}

uint64_t ia64_system_read_pmd(CPUIA64State *env, uint32_t index)
{
    IA64CPUClass *icc = ia64_env_cpu_class(env);
    uint64_t value;

    if (index >= IA64_PMD_COUNT ||
        !(icc->implemented_pmd_mask & (1ULL << index))) {
        return 0;
    }
    value = env->pmd[index];
    if (icc->model == IA64_CPU_MODEL_MADISON) {
        value &= ia64_madison_pmd_mask(index);
    }
    if (icc->model == IA64_CPU_MODEL_MERCED) {
        switch (index) {
        case 0:
            value &= ~(IA64_MERCED_PMD_ADDR_IGNORED_MASK | 0x1c);
            break;
        case 1:
            value &= 0xfff;
            break;
        case 2:
            value &= ~IA64_MERCED_PMD_ADDR_IGNORED_MASK;
            break;
        case 3:
            value &= 0xc000000000000fffULL;
            break;
        case 16:
            value &= 0xf;
            break;
        case 17:
            value &= ~(IA64_MERCED_PMD_ADDR_IGNORED_MASK | 0x2);
            break;
        default:
            break;
        }
        if (index == 0 || index == 2 || index == 17) {
            if (value & (1ULL << 50)) {
                value |= IA64_MERCED_PMD_ADDR_IGNORED_MASK;
            }
        }
    }
    if (index >= 4 && index <= 7) {
        uint32_t width = icc->perf_counter_width;

        g_assert(width > 0 && width <= 64);
        if (width < 64) {
            uint64_t sign = 1ULL << (width - 1);
            uint64_t mask = (sign << 1) - 1;

            value &= mask;
            return (value ^ sign) - sign;
        }
    }
    return value;
}

uint64_t ia64_system_read_pmd_checked(CPUIA64State *env, uint64_t index,
                                      uint64_t fault_ip, uint64_t raw,
                                      uint32_t slot)
{
    index &= 0xff;

    /*
     * PMD is the one indirect register file that can be read outside CPL0.
     * Secured user monitors and generic monitors marked privileged read as
     * zero; they do not raise a Privileged Operation/Register fault.  An
     * unimplemented PMD index likewise reads as zero by architectural rule.
     */
    if (ia64_psr_cpl(env->psr) != 0 &&
        ((env->psr & IA64_PSR_SP) ||
         (index > 3 && index <= IA64_LAST_GENERIC_PMD &&
          (ia64_system_read_pmc(env, index) & (1ULL << 6))))) {
        return 0;
    }

    /* Retain fault metadata in the helper ABI for restart/debug consistency. */
    (void)fault_ip;
    (void)raw;
    (void)slot;
    return ia64_system_read_pmd(env, index);
}

void ia64_system_write_pmd(CPUIA64State *env, uint32_t index, uint64_t value)
{
    IA64CPUClass *icc = ia64_env_cpu_class(env);

    if (index >= IA64_PMD_COUNT ||
        !(icc->implemented_pmd_mask & (1ULL << index))) {
        return;
    }
    if (icc->model == IA64_CPU_MODEL_MADISON) {
        value &= ia64_madison_pmd_mask(index);
    }
    if (icc->model == IA64_CPU_MODEL_MERCED) {
        switch (index) {
        case 0:
            value &= ~(IA64_MERCED_PMD_ADDR_IGNORED_MASK | 0x1c);
            break;
        case 1:
            value &= 0xfff;
            break;
        case 2:
            value &= ~IA64_MERCED_PMD_ADDR_IGNORED_MASK;
            break;
        case 3:
            value &= 0xc000000000000fffULL;
            break;
        case 16:
            value &= 0xf;
            break;
        case 17:
            value &= ~(IA64_MERCED_PMD_ADDR_IGNORED_MASK | 0x2);
            break;
        default:
            break;
        }
    }
    if (index >= 4 && index <= 7) {
        uint32_t width = icc->perf_counter_width;

        g_assert(width > 0 && width <= 64);
        if (width < 64) {
            value &= (1ULL << width) - 1;
        }
    }
    env->pmd[index] = value;
}

uint64_t ia64_system_read_pmd_indexed(CPUIA64State *env, uint64_t index)
{
    index &= 0xff;
    if (index >= IA64_PMD_COUNT) {
        return 0;
    }
    return ia64_system_read_pmd(env, index);
}

void ia64_system_write_pmd_indexed(CPUIA64State *env, uint64_t index,
                                   uint64_t value)
{
    index &= 0xff;
    if (index >= IA64_PMD_COUNT) {
        return;
    }
    ia64_system_write_pmd(env, index, value);
}

void ia64_system_st_spill_unat(CPUIA64State *env, uint32_t reg, uint64_t addr)
{
    uint32_t bit_pos = (addr >> 3) & 0x3f;

    if (ia64_gr_nat_get(env, reg)) {
        env->ar_unat |= 1ULL << bit_pos;
    } else {
        env->ar_unat &= ~(1ULL << bit_pos);
    }
}

static void ia64_swap_banked_gr(CPUIA64State *env)
{
    uint32_t i;

    for (i = 0; i < 16; i++) {
        uint32_t reg = 16 + i;
        uint64_t value = env->gr[reg];
        bool nat = ia64_gr_nat_get(env, reg);

        env->gr[reg] = env->banked_gr[i];
        ia64_gr_nat_set(env, reg, (env->banked_nat >> i) & 1);
        env->banked_gr[i] = value;
        if (nat) {
            env->banked_nat |= (uint16_t)(1U << i);
        } else {
            env->banked_nat &= (uint16_t)~(1U << i);
        }
    }
}

void ia64_set_psr(CPUIA64State *env, uint64_t value)
{
    uint64_t changed = env->psr ^ value;

    if (changed & IA64_PSR_IC) {
        env->exception_state.psr_ic_inflight = true;
    }
    if (!(value & IA64_PSR_I) || (value & IA64_PSR_IS)) {
        env->exception_state.psr_i_deferred = false;
    } else if ((changed & IA64_PSR_I) && env->exception_state.psr_ic_inflight) {
        env->exception_state.psr_i_deferred = true;
    }
    if (changed & IA64_PSR_BN) {
        ia64_swap_banked_gr(env);
    }
    env->psr = value;
    if (changed & (IA64_PSR_I | IA64_PSR_IC | IA64_PSR_MC | IA64_PSR_IS)) {
        ia64_sapic_update_interrupt(env);
    }
}

void ia64_flush_on_pk_change(CPUIA64State *env, uint64_t old_psr)
{
    if ((old_psr ^ env->psr) & IA64_PSR_PK) {
        /*
         * Protection-key checks are reflected in cached QEMU TLB
         * permissions.  rfi, ssm and rsm may toggle PSR.pk without writing
         * the PKRs, so discard only translated entries on those rare
         * transitions.  DT/IT/RT and CPL are represented by the MMU index.
         */
        ia64_tlb_bump_generation(env, false);
        ia64_tlb_bump_generation(env, true);
        tlb_flush_by_mmuidx(env_cpu(env), MMU_IDX_TRANSLATED_MASK);
    }
}

void ia64_system_clear_psr_fault_suppression(CPUIA64State *env)
{
    uint64_t old_mask = env->exception_state.psr_suppression_before_insn &
                        IA64_PSR_FAULT_SUPPRESS_MASK;
    uint64_t clear_mask = env->psr & old_mask;

    if (clear_mask) {
        ia64_set_psr(env, env->psr & ~clear_mask);
    }
    if (old_mask & (IA64_PSR_DA | IA64_PSR_IA)) {
        ia64_flush_suppressed_tlb(env);
    }
    env->exception_state.psr_suppression_before_insn = 0;
}

void ia64_set_psr_bn(CPUIA64State *env, bool bank1)
{
    uint64_t value =
        bank1 ? (env->psr | IA64_PSR_BN) : (env->psr & ~IA64_PSR_BN);

    ia64_set_psr(env, value);
}

void ia64_system_set_psr_bn(CPUIA64State *env, uint32_t bank1)
{
    ia64_set_psr_bn(env, bank1 != 0);
}

void ia64_system_ssm(CPUIA64State *env, uint64_t imm)
{
    uint64_t old_psr = env->psr;

    ia64_set_psr(env, env->psr | imm);
    ia64_flush_on_pk_change(env, old_psr);
}

void ia64_system_rsm(CPUIA64State *env, uint64_t imm)
{
    uint64_t old_psr = env->psr;

    ia64_set_psr(env, env->psr & ~imm);
    ia64_flush_on_pk_change(env, old_psr);
}
/* ---- Probe helper (optionally writes r1 and returns the probe result) ---- */
/* ---- tak / thash / ttag helpers ---- */

/* ---- mov to PSR helper ---- */

void ia64_system_mov_psr_write(CPUIA64State *env, uint64_t value,
                               uint32_t unused)
{
    uint64_t new_psr;

    if (unused) {
        new_psr = (env->psr & ~0xffffffffULL) | (value & 0xffffffffULL);
    } else {
        new_psr = value;
    }
    if (env->psr == new_psr) {
        return;
    }
    ia64_set_psr(env, new_psr);
    ia64_tlb_bump_generation(env, false);
    ia64_tlb_bump_generation(env, true);
    tlb_flush(env_cpu(env));
}

/* ---- mov from Region Register helper ---- */

uint64_t ia64_system_mov_rrgr_read(CPUIA64State *env, uint64_t rr_addr)
{
    uint32_t rr_num = (rr_addr >> 61) & 7;

    if (rr_num < 8) {
        return env->rr[rr_num];
    }
    return 0;
}

uint64_t ia64_system_validate_rr_value(CPUIA64State *env, uint64_t value,
                                       uint64_t fault_ip, uint64_t raw,
                                       uint32_t slot)
{
    uint8_t ps = (value >> 2) & 0x3f;
    uint8_t rid_bits = ia64_env_cpu_class(env)->rid_bits;
    uint64_t allowed = 1ULL | (0x3fULL << 2) | (((1ULL << rid_bits) - 1) << 8);

    if ((value & ~allowed) || !ia64_page_shift_insertable(env, ps)) {
        env->cr_isr = 0x30;
        ia64_raise_exception(env, IA64_EXCP_RESERVED_REG_FIELD, fault_ip, raw,
                             slot);
    }
    return value;
}

/* ---- mov to Region Register helper ---- */

void ia64_system_mov_grrr_write(CPUIA64State *env, uint64_t rr_addr,
                                uint64_t value)
{
    uint32_t rr_num = (rr_addr >> 61) & 7;

    if (env->rr[rr_num] == value) {
        return;
    }

    env->rr[rr_num] = value;
    ia64_tlb_bump_generation(env, false);
    ia64_tlb_bump_generation(env, true);
    /*
     * The softmmu TLB and jump cache contain virtual-address state, so both
     * must be discarded when the RID changes.  tlb_flush() does both.  The
     * global TB hash is keyed by the translated physical page as well as the
     * virtual PC, so its TBs remain valid and can be reused when this address
     * space becomes current again.
     */
    tlb_flush(env_cpu(env));
}

/* ---- mov from PKR helper ---- */

uint64_t ia64_system_mov_pkrgr_read(CPUIA64State *env, uint32_t pkr_num)
{
    if (pkr_num < IA64_PKR_COUNT) {
        return env->pkr[pkr_num];
    }
    return 0;
}

uint64_t ia64_system_mov_pkrgr_indexed_read(CPUIA64State *env, uint64_t pkr_num)
{
    pkr_num &= 0xff;
    if (pkr_num < IA64_PKR_COUNT) {
        return env->pkr[pkr_num];
    }
    return 0;
}

/* ---- mov to PKR helper ---- */

static void ia64_pkr_write(CPUIA64State *env, uint32_t pkr_num, uint64_t value)
{
    uint64_t key_mask = ia64_pkr_key_mask(env);
    uint64_t masked = value & ia64_pkr_mask(env);
    uint64_t key = masked & key_mask;
    bool changed;

    if (pkr_num >= IA64_PKR_COUNT) {
        return;
    }

    changed = env->pkr[pkr_num] != masked;
    if (masked & IA64_PKR_VALID) {
        for (uint32_t i = 0; i < IA64_PKR_COUNT; i++) {
            if (i != pkr_num && (env->pkr[i] & IA64_PKR_VALID) &&
                (env->pkr[i] & key_mask) == key) {
                env->pkr[i] &= ~IA64_PKR_VALID;
                changed = true;
            }
        }
    }
    env->pkr[pkr_num] = masked;
    if (changed) {
        memset(env->mmu.key_cache, 0, sizeof(env->mmu.key_cache));
        ia64_tlb_bump_generation(env, false);
        ia64_tlb_bump_generation(env, true);
        tlb_flush(env_cpu(env));
    }
}

void ia64_system_mov_grpkr_write(CPUIA64State *env, uint32_t pkr_num,
                                 uint64_t value)
{
    ia64_pkr_write(env, pkr_num, value);
}

void ia64_system_mov_grpkr_indexed_write(CPUIA64State *env, uint64_t pkr_num,
                                         uint64_t value)
{
    pkr_num &= 0xff;
    ia64_pkr_write(env, pkr_num, value);
}
