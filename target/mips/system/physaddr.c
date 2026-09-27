/*
 * MIPS TLB (Translation lookaside buffer) helpers.
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
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/page-protection.h"
#include "../internal.h"

static int is_seg_am_mapped(unsigned int am, bool eu, int mmu_idx)
{
    /*
     * Interpret access control mode and mmu_idx.
     *           AdE?     TLB?
     *      AM  K S U E  K S U E
     * UK    0  0 1 1 0  0 - - 0
     * MK    1  0 1 1 0  1 - - !eu
     * MSK   2  0 0 1 0  1 1 - !eu
     * MUSK  3  0 0 0 0  1 1 1 !eu
     * MUSUK 4  0 0 0 0  0 1 1 0
     * USK   5  0 0 1 0  0 0 - 0
     * -     6  - - - -  - - - -
     * UUSK  7  0 0 0 0  0 0 0 0
     */
    int32_t adetlb_mask;

    switch (mmu_idx) {
    case 3: /* ERL */
        /* If EU is set, always unmapped */
        if (eu) {
            return 0;
        }
        /* fall through */
    case MIPS_HFLAG_KM:
        /* Never AdE, TLB mapped if AM={1,2,3} */
        adetlb_mask = 0x70000000;
        goto check_tlb;

    case MIPS_HFLAG_SM:
        /* AdE if AM={0,1}, TLB mapped if AM={2,3,4} */
        adetlb_mask = 0xc0380000;
        goto check_ade;

    case MIPS_HFLAG_UM:
        /* AdE if AM={0,1,2,5}, TLB mapped if AM={3,4} */
        adetlb_mask = 0xe4180000;
        /* fall through */
    check_ade:
        /* does this AM cause AdE in current execution mode */
        if ((adetlb_mask << am) < 0) {
            return TLBRET_BADADDR;
        }
        adetlb_mask <<= 8;
        /* fall through */
    check_tlb:
        /* is this AM mapped in current execution mode */
        return ((adetlb_mask << am) < 0);
    default:
        g_assert_not_reached();
    };
}

static int get_seg_physical_address(CPUMIPSState *env, hwaddr *physical,
                                    int *prot, target_ulong real_address,
                                    MMUAccessType access_type, int mmu_idx,
                                    unsigned int am, bool eu,
                                    target_ulong segmask,
                                    hwaddr physical_base, unsigned int cca,
                                    MemTxAttrs *attrs)
{
    int mapped = is_seg_am_mapped(am, eu, mmu_idx);

    if (mapped < 0) {
        /* is_seg_am_mapped can report TLBRET_BADADDR */
        return mapped;
    } else if (mapped) {
        /* The segment is TLB mapped */
        return env->tlb->map_address(env, physical, prot, real_address,
                                     access_type, attrs);
    } else {
        /*
         * The segment is unmapped. An unmapped reference whose cache
         * algorithm is uncached or uncached accelerated is an uncached
         * request on the system interface, and the machine is entitled to
         * tell it apart from a cached one — the Origin's boot firmware
         * depends on the distinction while it runs with its data cache
         * standing in for memory.
         */
        if (env->cfg.uncached_attribute && (cca == 2 || cca == 7)) {
            attrs->mips_uncached = 1;
        }
        /* The algorithm decides which coherent request a miss makes. */
        if (env->cfg.uncached_attribute) {
            attrs->mips_cca = cca;
            attrs->requester_id = env->r10k->requester_id;
        }
        *physical = physical_base | (real_address & segmask);
        *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        return TLBRET_MATCH;
    }
}

static int get_segctl_physical_address(CPUMIPSState *env, hwaddr *physical,
                                       int *prot, target_ulong real_address,
                                       MMUAccessType access_type, int mmu_idx,
                                       uint16_t segctl, target_ulong segmask,
                                       MemTxAttrs *attrs)
{
    unsigned int am = (segctl & CP0SC_AM_MASK) >> CP0SC_AM;
    bool eu = (segctl >> CP0SC_EU) & 1;
    hwaddr pa = ((hwaddr)segctl & CP0SC_PA_MASK) << 20;
    unsigned int cca = (segctl & CP0SC_C_MASK) >> CP0SC_C;

    return get_seg_physical_address(env, physical, prot, real_address,
                                    access_type, mmu_idx, am, eu, segmask,
                                    pa & ~(hwaddr)segmask, cca, attrs);
}

int get_physical_address(CPUMIPSState *env, hwaddr *physical,
                         int *prot, target_ulong real_address,
                         MMUAccessType access_type, int mmu_idx,
                         MemTxAttrs *attrs)
{
    /* User mode can only access useg/xuseg */
#if defined(TARGET_MIPS64)
    int user_mode = mmu_idx == MIPS_HFLAG_UM;
    int supervisor_mode = mmu_idx == MIPS_HFLAG_SM;
    int kernel_mode = !user_mode && !supervisor_mode;
    int UX = (env->CP0_Status & (1 << CP0St_UX)) != 0;
    int SX = (env->CP0_Status & (1 << CP0St_SX)) != 0;
    int KX = (env->CP0_Status & (1 << CP0St_KX)) != 0;
#endif
    int ret = TLBRET_MATCH;
    target_ulong address = real_address;

    if (address <= USEG_LIMIT) {
        /* useg */
        uint16_t segctl;

        if (address >= 0x40000000UL) {
            segctl = env->CP0_SegCtl2;
        } else {
            segctl = env->CP0_SegCtl2 >> 16;
        }
        ret = get_segctl_physical_address(env, physical, prot,
                                          real_address, access_type,
                                          mmu_idx, segctl, 0x3FFFFFFF,
                                          attrs);
#if defined(TARGET_MIPS64)
    } else if (address < 0x4000000000000000ULL) {
        /*
         * xuseg. The space above 2 Gbytes exists only while ERL is clear:
         * the manual's 64-bit kernel mode user space section says that with
         * UX = 0 or ERL = 1 only the low 2 Gbytes are selected, and
         * everything from 0x0000000080000000 to the top of the region is an
         * address error. The low 2 Gbytes are the useg branch above, which
         * the segment control's erl-unmapped bit already turns into an
         * unmapped alias of physical memory.
         */
        bool erl = env->cfg.erl_restricts_xuseg && mmu_idx == 3;

        if (UX && !erl && address <= (0x3FFFFFFFFFFFFFFFULL & env->SEGMask)) {
            ret = env->tlb->map_address(env, physical, prot,
                                        real_address, access_type, attrs);
        } else {
            ret = TLBRET_BADADDR;
        }
    } else if (address < 0x8000000000000000ULL) {
        /* xsseg */
        if ((supervisor_mode || kernel_mode) &&
            SX && address <= (0x7FFFFFFFFFFFFFFFULL & env->SEGMask)) {
            ret = env->tlb->map_address(env, physical, prot,
                                        real_address, access_type, attrs);
        } else {
            ret = TLBRET_BADADDR;
        }
    } else if (address < 0xC000000000000000ULL) {
        /* xkphys */
        uint64_t pa = address & 0x07FFFFFFFFFFFFFFULL;
        unsigned int cca = (address >> 59) & 0x7;
        /*
         * An uncached or uncached accelerated reference carries a two-bit
         * attribute in address bits 58:57 that is not part of the physical
         * address. The R10000 family drives it out to the system interface,
         * which places the reference in one of four windows above memory;
         * the window helper returns that offset and clears the attribute
         * bits out of pa, and returns zero for every other reference.
         */
        hwaddr window = mips_r10k_xkphys_uncached_window(env, address, cca,
                                                         &pa);

        if (pa <= env->PAMask) {
            /* KX/SX/UX bit to check for each xkphys EVA access mode */
            static const uint8_t am_ksux[8] = {
                [CP0SC_AM_UK]    = (1u << CP0St_KX),
                [CP0SC_AM_MK]    = (1u << CP0St_KX),
                [CP0SC_AM_MSK]   = (1u << CP0St_SX),
                [CP0SC_AM_MUSK]  = (1u << CP0St_UX),
                [CP0SC_AM_MUSUK] = (1u << CP0St_UX),
                [CP0SC_AM_USK]   = (1u << CP0St_SX),
                [6]              = (1u << CP0St_KX),
                [CP0SC_AM_UUSK]  = (1u << CP0St_UX),
            };
            unsigned int am = CP0SC_AM_UK;
            unsigned int xr = (env->CP0_SegCtl2 & CP0SC2_XR_MASK) >> CP0SC2_XR;

            if (xr & (1 << ((address >> 59) & 0x7))) {
                am = (env->CP0_SegCtl1 & CP0SC1_XAM_MASK) >> CP0SC1_XAM;
            }
            /* Does CP0_Status.KX/SX/UX permit the access mode (am) */
            if (env->CP0_Status & am_ksux[am]) {
                ret = get_seg_physical_address(env, physical, prot,
                                               real_address, access_type,
                                               mmu_idx, am, false, env->PAMask,
                                               window, cca, attrs);
            } else {
                ret = TLBRET_BADADDR;
            }
        } else {
            ret = TLBRET_BADADDR;
        }
    } else if (address < 0xFFFFFFFF80000000ULL) {
        /*
         * xkseg. Everything above the segment's bound is an address error.
         * The R10000 family ends the segment early (env->r10k->xkseg_limit);
         * every other processor uses the generic R4000 bound.
         */
        target_ulong xkseg_limit = env->r10k ? env->r10k->xkseg_limit :
                                   (0xFFFFFFFF7FFFFFFFULL & env->SEGMask);

        if (kernel_mode && KX && address <= xkseg_limit) {
            ret = env->tlb->map_address(env, physical, prot,
                                        real_address, access_type, attrs);
        } else {
            ret = TLBRET_BADADDR;
        }
#endif
    } else if (address < KSEG1_BASE) {
        /* kseg0 */
        ret = get_segctl_physical_address(env, physical, prot, real_address,
                                          access_type, mmu_idx,
                                          env->CP0_SegCtl1 >> 16, 0x1FFFFFFF,
                                          attrs);
    } else if (address < KSEG2_BASE) {
        /* kseg1 */
        ret = get_segctl_physical_address(env, physical, prot, real_address,
                                          access_type, mmu_idx,
                                          env->CP0_SegCtl1, 0x1FFFFFFF,
                                          attrs);
    } else if (address < KSEG3_BASE) {
        /* sseg (kseg2) */
        ret = get_segctl_physical_address(env, physical, prot, real_address,
                                          access_type, mmu_idx,
                                          env->CP0_SegCtl0 >> 16, 0x1FFFFFFF,
                                          attrs);
    } else {
        /*
         * kseg3
         * XXX: debug segment is not emulated
         */
        ret = get_segctl_physical_address(env, physical, prot, real_address,
                                          access_type, mmu_idx,
                                          env->CP0_SegCtl0, 0x1FFFFFFF,
                                          attrs);
    }
    return ret;
}

hwaddr mips_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr)
{
    CPUMIPSState *env = cpu_env(cs);
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    hwaddr phys_addr;
    int prot;

    if (get_physical_address(env, &phys_addr, &prot, addr, MMU_DATA_LOAD,
                             mips_env_mmu_index(env), &attrs) != 0) {
        return -1;
    }
    return phys_addr;
}
