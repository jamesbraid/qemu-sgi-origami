/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The R10000 family's system helpers: the CACHE instruction's diagnostic
 * operations over the cache arrays the CPU state carries, the physical
 * watchpoint pair behind the WatchLo and WatchHi registers, and the
 * coprocessor 0 registers only this family has.
 *
 * Apart from the debug hooks, which the target registers for every MIPS
 * CPU and which reject other processors themselves, everything here is
 * reached only on a processor whose definition carries INSN_R10K; the
 * shared helpers dispatch before their own generic paths.
 *
 * Built for every MIPS system target, not only mips64: the shared
 * translator emits calls to these helpers without a TARGET_MIPS64 guard,
 * so the symbols must link everywhere, as octeon_crypto.c's do — unlike
 * lcsr_helper.c, whose call sites are 64-bit-only.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "cpu.h"
#include "internal.h"
#include "exec/cputlb.h"
#include "exec/helper-proto.h"
#include "exec/target_page.h"
#include "exec/watchpoint.h"
#include "accel/tcg/cpu-loop.h"
#include "../tcg-internal.h"

static unsigned int r10k_scache_size(const CPUMIPSState *env)
{
    if (env->cfg.scache_size == R10K_SCACHE_SIZE &&
        extract32(env->CP0_Config0, CP0C0_R10K_SS, 3) == 3) {
        return R10K_SCACHE_4M_SIZE;
    }
    return env->cfg.scache_size;
}

static unsigned int r10k_scache_sets(const CPUMIPSState *env)
{
    return r10k_scache_size(env) / R10K_SCACHE_LINE_SIZE /
           R10K_SCACHE_WAYS;
}

static unsigned int r10k_scache_quad_sets(const CPUMIPSState *env)
{
    return r10k_scache_size(env) / 16 / R10K_SCACHE_WAYS;
}

/*
 * Invalidate one primary instruction-cache way: the state bit and the state
 * parity bit go to zero, and nothing else moves — the tag address, the tag
 * parity bit and the set's least-recently-used bit all stay.
 */
static void r10k_icache_invalidate(CPUMIPSState *env, unsigned int tag)
{
    env->r10k->icache_taglo[tag] &= ~((1 << 6) | (1 << 2));
}

/*
 * Invalidate one primary data-cache way: state to invalid, the secondary-way
 * bit to zero, the state modifier back to normal (001), state parity to
 * zero. On hardware an inconsistent block is written back to the secondary
 * cache first; this model's memory is always current and its cache arrays
 * are diagnostic stores beside it, so a write back has nothing to move.
 */
static void r10k_dcache_invalidate(CPUMIPSState *env, unsigned int tag)
{
    env->r10k->dcache_taglo[tag] &= ~((3 << 6) | (1 << 2) | (1 << 1));
    env->r10k->dcache_taghi[tag] =
        (env->r10k->dcache_taghi[tag] & ~(7u << 29)) | (1u << 29);
}

/* The physical address bits 39:12 a primary cache tag carries. */
static uint64_t r10k_icache_tag_pa(CPUMIPSState *env, unsigned int tag)
{
    return ((uint64_t)env->r10k->icache_taghi[tag] << 24) |
           (env->r10k->icache_taglo[tag] >> 8);
}

static uint64_t r10k_dcache_tag_pa(CPUMIPSState *env, unsigned int tag)
{
    return ((uint64_t)(env->r10k->dcache_taghi[tag] & 0xf) << 24) |
           (env->r10k->dcache_taglo[tag] >> 8);
}

/* The physical address bits 39:18 a secondary cache tag carries. */
static uint64_t r10k_scache_tag_pa(CPUMIPSState *env, unsigned int tag)
{
    return ((uint64_t)env->r10k->scache_taghi[tag] << 18) |
           (env->r10k->scache_taglo[tag] >> 14);
}

/*
 * Invalidating a secondary block extends to the primary blocks that are its
 * subsets. The primary caches are virtually indexed, which is why the
 * secondary tag keeps its block's virtual index bits VA[13:12]: they name
 * the primary sets to interrogate, and the block's physical address decides
 * a match.
 */
static void r10k_scache_invalidate_subsets(CPUMIPSState *env, uint64_t block,
                                           unsigned int pidx)
{
    unsigned int i, w;

    for (i = 0; i < R10K_SCACHE_LINE_SIZE / R10K_ICACHE_LINE_SIZE; i++) {
        uint64_t sub = block + i * R10K_ICACHE_LINE_SIZE;
        unsigned int set = (pidx << 6) | extract64(sub, 6, 6);

        for (w = 0; w < R10K_PCACHE_WAYS; w++) {
            unsigned int tag = set * R10K_PCACHE_WAYS + w;

            if ((env->r10k->icache_taglo[tag] & (1 << 6)) &&
                r10k_icache_tag_pa(env, tag) == extract64(sub, 12, 28)) {
                r10k_icache_invalidate(env, tag);
            }
        }
    }
    for (i = 0; i < R10K_SCACHE_LINE_SIZE / R10K_DCACHE_LINE_SIZE; i++) {
        uint64_t sub = block + i * R10K_DCACHE_LINE_SIZE;
        unsigned int set = (pidx << 7) | extract64(sub, 5, 7);

        for (w = 0; w < R10K_PCACHE_WAYS; w++) {
            unsigned int tag = set * R10K_PCACHE_WAYS + w;

            if ((env->r10k->dcache_taglo[tag] & (3 << 6)) &&
                r10k_dcache_tag_pa(env, tag) == extract64(sub, 12, 28)) {
                r10k_dcache_invalidate(env, tag);
            }
        }
    }
}

/*
 * Invalidate one secondary way. The tag array is built so all of a tag's
 * bits are written at once, so the whole entry is rewritten: state to
 * invalid, the tag from the operation's physical address, the virtual
 * index from its VA[13:12]. Hardware generates fresh ECC over those
 * fields; the model keeps no ECC machinery and nothing ever checks these
 * bits, so it stores zero. The MRU bit is pointed away from the
 * invalidated way. A dirty block would be written back to the system
 * interface first, and a clean one announced deleted; the model's memory
 * is always current, so neither has anything to move.
 */
static void r10k_scache_invalidate(CPUMIPSState *env, unsigned int set,
                                   unsigned int way, uint64_t paddr,
                                   target_ulong vaddr)
{
    unsigned int tag = set * R10K_SCACHE_WAYS + way;

    r10k_scache_invalidate_subsets(env,
                                   (r10k_scache_tag_pa(env, tag) << 18) |
                                   (paddr & 0x3ff80),
                                   extract32(env->r10k->scache_taglo[tag],
                                             7, 2));
    env->r10k->scache_taglo[tag] =
        (uint32_t)(extract64(paddr, 18, 18) << 14) |
        ((uint32_t)extract64(vaddr, 12, 2) << 7);
    env->r10k->scache_taghi[tag] = extract64(paddr, 36, 4);
    env->r10k->scache_mru[set] = way ^ 1;
}

/*
 * Tell the board about a request the processor puts on its system
 * interface with no memory access behind it, and return whether the board
 * follows a block the request found. A board that wants none leaves the
 * hook unset.
 */
static bool r10k_system_request(CPUMIPSState *env, R10KSystemRequest request,
                                hwaddr paddr, uint64_t index_mask)
{
    R10KState *r10k = env->r10k;

    if (!r10k->system_request) {
        return false;
    }
    return r10k->system_request(r10k->system_request_opaque, env_cpu(env),
                                request, paddr, index_mask);
}

/*
 * A prefetch with a load hint asks for the block to read, with a store hint
 * for it exclusive (R10000 User's Manual version 2.0, Table 1-1); the other
 * hints are undefined and do nothing. A prefetch takes no memory-management
 * exception, so an address that does not translate, or translates
 * uncached, is simply dropped. Prefetches are common in copy loops, so the
 * translation is skipped while the board follows nothing.
 */
void helper_r10k_prefetch(CPUMIPSState *env, target_ulong addr, uint32_t hint)
{
    R10KState *r10k = env->r10k;
    MMUAccessType access = hint & 1 ? MMU_DATA_STORE : MMU_DATA_LOAD;
    MemTxAttrs attrs = {};
    hwaddr paddr;
    int prot;

    if (hint > 7 || hint == 2 || hint == 3 || !r10k->system_request) {
        return;
    }
    if (r10k->system_watching &&
        !r10k->system_watching(r10k->system_request_opaque)) {
        return;
    }
    if (get_physical_address(env, &paddr, &prot, addr, access,
                             mips_env_mmu_index(env), &attrs) !=
        TLBRET_MATCH || attrs.mips_uncached) {
        return;
    }
    r10k_system_request(env, hint & 1 ? R10K_REQUEST_PREFETCH_STORE :
                                        R10K_REQUEST_PREFETCH_LOAD,
                        paddr, 0);
}

/*
 * The R10000 family's CACHE instruction, dispatched from helper_cache()
 * before the generic model: every defined operation works the diagnostic
 * arrays, and undefined combinations stay inert. @ra is the helper's own
 * return address, for the exceptions translation and alignment can raise.
 */
void r10k_cache_op(CPUMIPSState *env, target_ulong addr, uint32_t op,
                   uintptr_t ra)
{
    uint32_t cache_type = extract32(op, 0, 2);
    uint32_t cache_operation = extract32(op, 2, 3);
    hwaddr paddr;

    /*
     * Combinations Table 10-1 does not list are undefined; they stay
     * inert, taking none of the exceptions the defined operations do.
     */
    if (cache_type == 2 || cache_operation == 0b011) {
        return;
    }

    /*
     * Address bit 0 of an Index operation is the way indicator and is
     * never checked; its bit 1 generates an Address Error if set. A
     * Hit operation's low bits are don't-care — the operation
     * tag-searches both ways, so no way or word applies — and take no
     * alignment error at all: the manual limits CacheOp address errors
     * to privilege-type, and a shipped kernel's secondary flush sets
     * bit 0 as a vestigial way-select it expects ignored. The
     * privilege address errors come out of translation below.
     */
    if ((addr & 2) &&
        cache_operation != 0b100 && cache_operation != 0b101) {
        if (!(env->hflags & MIPS_HFLAG_DM)) {
            env->CP0_BadVAddr = addr;
        }
        do_raise_exception_err(env, EXCP_AdEL, 0, ra);
    }

    /*
     * TLB Refill and TLB Invalid exceptions can occur on any operation,
     * index forms included — unmapped addresses avoid them by never
     * missing, not by being exempt. The translation is a load, so a
     * CacheOp never takes a TLB Modified exception, and no memory
     * access is made, so no Watch exception fires for one.
     */
    paddr = cpu_mips_translate_address(env, addr, MMU_DATA_LOAD, ra);

    /*
     * The primary caches are virtually indexed: VA[13:6] names an
     * instruction-cache set, VA[13:5] a data-cache set, VA[13:2] a word
     * for the data operations, and VA[0] the way throughout. Their index
     * diagnostic operations move whole tag and data entries between the
     * arrays and the TagLo/TagHi/ECC staging registers; the set-wide
     * least-recently-used bit rides in TagLo bit 3 either way.
     */
    if (cache_type == 0 && cache_operation == 0b000) {
        /* Index Invalidate (I) */
        unsigned int set = extract64(addr, 6, 8);

        r10k_icache_invalidate(env, set * R10K_PCACHE_WAYS + (addr & 1));
        return;
    }
    if (cache_type == 1 && cache_operation == 0b000) {
        /* Index WriteBack Invalidate (D) */
        unsigned int set = extract64(addr, 5, 9);

        r10k_dcache_invalidate(env, set * R10K_PCACHE_WAYS + (addr & 1));
        return;
    }
    if ((cache_type == 0 && cache_operation == 0b100) ||
        (cache_type == 1 && (cache_operation == 0b100 ||
                             cache_operation == 0b101))) {
        /*
         * Hit Invalidate (I)/(D) and Hit WriteBack Invalidate (D): both
         * ways at the virtual index are compared against the physical
         * address, and a valid matching way is invalidated. The write
         * back distinguishing the last of the three has nothing to move
         * in this model.
         */
        unsigned int w;

        for (w = 0; w < R10K_PCACHE_WAYS; w++) {
            if (cache_type == 0) {
                unsigned int tag = extract64(addr, 6, 8) *
                                   R10K_PCACHE_WAYS + w;

                if ((env->r10k->icache_taglo[tag] & (1 << 6)) &&
                    r10k_icache_tag_pa(env, tag) ==
                    extract64(paddr, 12, 28)) {
                    r10k_icache_invalidate(env, tag);
                }
            } else {
                unsigned int tag = extract64(addr, 5, 9) *
                                   R10K_PCACHE_WAYS + w;

                if ((env->r10k->dcache_taglo[tag] & (3 << 6)) &&
                    r10k_dcache_tag_pa(env, tag) ==
                    extract64(paddr, 12, 28)) {
                    r10k_dcache_invalidate(env, tag);
                }
            }
        }
        return;
    }
    if (cache_type == 0 && (cache_operation == 0b001 ||
                            cache_operation == 0b010)) {
        unsigned int way = addr & 1;
        unsigned int set = extract64(addr, 6, 8);
        unsigned int tag = set * R10K_PCACHE_WAYS + way;

        if (cache_operation == 0b010) { /* Index Store Tag (I) */
            env->r10k->icache_taglo[tag] = env->CP0_TagLo &
                (R10K_ICACHE_TAGLO_MASK & ~R10K_PCACHE_TAGLO_LRU);
            env->r10k->icache_taghi[tag] =
                env->CP0_TagHi & R10K_ICACHE_TAGHI_MASK;
            env->r10k->icache_lru[set] = extract32(env->CP0_TagLo, 3, 1);
        } else { /* Index Load Tag (I) */
            env->CP0_TagLo = env->r10k->icache_taglo[tag] |
                (env->r10k->icache_lru[set] << 3);
            env->CP0_TagHi = env->r10k->icache_taghi[tag];
        }
        return;
    }
    if (cache_type == 1 && (cache_operation == 0b001 ||
                            cache_operation == 0b010)) {
        unsigned int way = addr & 1;
        unsigned int set = extract64(addr, 5, 9);
        unsigned int tag = set * R10K_PCACHE_WAYS + way;

        if (cache_operation == 0b010) { /* Index Store Tag (D) */
            env->r10k->dcache_taglo[tag] = env->CP0_TagLo &
                (R10K_DCACHE_TAGLO_MASK & ~R10K_PCACHE_TAGLO_LRU);
            env->r10k->dcache_taghi[tag] =
                env->CP0_TagHi & R10K_DCACHE_TAGHI_MASK;
            env->r10k->dcache_lru[set] = extract32(env->CP0_TagLo, 3, 1);
        } else { /* Index Load Tag (D) */
            env->CP0_TagLo = env->r10k->dcache_taglo[tag] |
                (env->r10k->dcache_lru[set] << 3);
            env->CP0_TagHi = env->r10k->dcache_taghi[tag];
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
                env->r10k->icache_data[entry] =
                    (uint64_t)(env->CP0_TagHi & 0xf) << 32 |
                    (uint32_t)env->CP0_TagLo;
                env->r10k->icache_parity[entry] = env->CP0_ErrCtl & 1;
            } else { /* Index Load Data (I) */
                uint64_t data = env->r10k->icache_data[entry];

                env->CP0_TagLo = (uint32_t)data;
                env->CP0_TagHi = data >> 32;
                env->CP0_ErrCtl = env->r10k->icache_parity[entry];
            }
        } else {
            if (cache_operation == 0b111) { /* Index Store Data (D) */
                env->r10k->dcache_data[entry] = env->CP0_TagLo;
                env->r10k->dcache_parity[entry] = env->CP0_ErrCtl & 0xf;
            } else { /* Index Load Data (D) */
                /*
                 * The data-cache word format leaves TagHi "Not Used"
                 * rather than reserved, so the load fills TagLo and the
                 * parity bits and leaves TagHi alone.
                 */
                env->CP0_TagLo = env->r10k->dcache_data[entry];
                env->CP0_ErrCtl = env->r10k->dcache_parity[entry];
            }
        }
        return;
    }
    if (cache_type == 3 && cache_operation == 0b000) {
        /*
         * Index WriteBack Invalidate (S): the physical address indexes
         * the set and its bit zero names the way, with no tag compare.
         * A block already invalid is left entirely alone — no tag
         * rewrite, and the MRU bit stays where it was.
         */
        unsigned int way = paddr & 1;
        unsigned int set = (paddr / R10K_SCACHE_LINE_SIZE) &
                           (r10k_scache_sets(env) - 1);

        /*
         * A block the board follows is written back if dirty, and a
         * block write or nothing goes out on the interface either way
         * (the processor block write and eliminate protocols).
         */
        r10k_system_request(env, R10K_REQUEST_INDEX_WRITEBACK_INVALIDATE,
                            paddr, (uint64_t)(r10k_scache_sets(env) - 1) *
                                   R10K_SCACHE_LINE_SIZE);
        if (env->r10k->scache_taglo[set * R10K_SCACHE_WAYS + way] &
            (3 << 10)) {
            r10k_scache_invalidate(env, set, way, paddr, addr);
        }
        return;
    }
    if (cache_type == 3 && (cache_operation == 0b100 ||
                            cache_operation == 0b101)) {
        /*
         * Hit Invalidate (S) and Hit WriteBack Invalidate (S): both
         * ways' tags are compared against the physical address, and a
         * valid matching way is invalidated. Either way the outcome is
         * recorded in the Status register's CH bit — set on a hit,
         * cleared on a miss — which is what tells a diagnostic whether
         * its operation found the block.
         */
        unsigned int set = (paddr / R10K_SCACHE_LINE_SIZE) &
                           (r10k_scache_sets(env) - 1);
        unsigned int w;
        bool hit = r10k_system_request(env, cache_operation == 0b101 ?
                                       R10K_REQUEST_HIT_WRITEBACK_INVALIDATE :
                                       R10K_REQUEST_HIT_INVALIDATE,
                                       paddr, 0);

        for (w = 0; w < R10K_SCACHE_WAYS; w++) {
            unsigned int tag = set * R10K_SCACHE_WAYS + w;

            if ((env->r10k->scache_taglo[tag] & (3 << 10)) &&
                r10k_scache_tag_pa(env, tag) == extract64(paddr, 18, 22)) {
                r10k_scache_invalidate(env, set, w, paddr, addr);
                hit = true;
                break;
            }
        }
        if (hit) {
            env->CP0_Status |= 1 << CP0St_CH;
        } else {
            env->CP0_Status &= ~(1 << CP0St_CH);
        }
        return;
    }
    /*
     * The CPU definition supplies the default secondary-cache capacity. An
     * R10000's reset Config input can select the larger fitted cache. Index
     * diagnostic operations use the translated physical address and address
     * bit zero selects the way.
     */
    if (cache_type == 3 && (cache_operation == 0b001 ||
                           cache_operation == 0b010 ||
                           cache_operation == 0b110 ||
                           cache_operation == 0b111)) {
        unsigned int way = paddr & 1;

        if (cache_operation == 0b001 || cache_operation == 0b010) {
            unsigned int set = (paddr / R10K_SCACHE_LINE_SIZE) &
                               (r10k_scache_sets(env) - 1);
            unsigned int tag = set * R10K_SCACHE_WAYS + way;

            /* TagHi's MRU bit is set-wide; its tag nibble is per-way. */
            if (cache_operation == 0b010) { /* Index Store Tag */
                env->r10k->scache_taglo[tag] =
                    env->CP0_TagLo & R10K_SCACHE_TAGLO_MASK;
                env->r10k->scache_taghi[tag] =
                    env->CP0_TagHi & R10K_SCACHE_TAGHI_MASK;
                env->r10k->scache_mru[set] =
                    extract32(env->CP0_TagHi, 31, 1);
            } else { /* Index Load Tag */
                env->CP0_TagLo = env->r10k->scache_taglo[tag];
                env->CP0_TagHi = env->r10k->scache_taghi[tag] |
                    ((uint32_t)env->r10k->scache_mru[set] << 31);
            }
        } else {
            unsigned int quad_set = (paddr / 16) &
                (r10k_scache_quad_sets(env) - 1);
            unsigned int quad = quad_set * R10K_SCACHE_WAYS + way;
            unsigned int dword = quad * 2 + extract64(paddr, 3, 1);

            if (cache_operation == 0b111) { /* Index Store Data */
                /*
                 * The data array is one quadword wide and is written
                 * whole: the doubleword not named by the address is
                 * padded with zeroes so the stored check bits cover a
                 * known quadword.
                 */
                env->r10k->scache_data[quad * 2] = 0;
                env->r10k->scache_data[quad * 2 + 1] = 0;
                env->r10k->scache_data[dword] =
                    (uint64_t)(uint32_t)env->CP0_TagHi << 32 |
                    (uint32_t)env->CP0_TagLo;
                env->r10k->scache_ecc[quad] = env->CP0_ErrCtl & 0x3ff;
            } else { /* Index Load Data */
                uint64_t data = env->r10k->scache_data[dword];

                env->CP0_TagLo = (uint32_t)data;
                env->CP0_TagHi = data >> 32;
                env->CP0_ErrCtl = env->r10k->scache_ecc[quad];
            }
        }
    }
}

/*
 * The R10000 family never lets a TLB write create a multiple match: every
 * other entry the new one overlaps - even in the wired area - is invalidated
 * before the new entry is written, and the Status register's TS bit records
 * that this happened. No exception is raised; software clears TS by writing
 * a zero to it. The manual's Status register section describes this under
 * the diagnostic status field, contrasting it with the R4400's TLB shutdown.
 *
 * Overlap means a single translation could match both entries: the virtual
 * page numbers coincide under the wider of the two page masks, and the
 * address space identifiers agree unless either entry is global. Invalidated
 * entries are marked the way tlbinv marks them, which takes them out of
 * every match; the valid bits stay untouched because a cleared valid bit
 * still matches, it just faults instead of translating.
 *
 * Only this family's TLB write helpers call this, so it carries no test of
 * its own for the processor it belongs to.
 */
void r10k_invalidate_conflicts(CPUMIPSState *env, int idx)
{
#if defined(TARGET_MIPS64)
    target_ulong mask, VPN;
    uint16_t ASID;
    bool G, conflict = false, translating = false;
    int i;

    /* 1k pages are not supported. */
    mask = env->CP0_PageMask | ~(TARGET_PAGE_MASK << 1);
    VPN = env->CP0_EntryHi & (TARGET_PAGE_MASK << 1) & env->SEGMask;
    ASID = env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask;
    G = env->CP0_EntryLo0 & env->CP0_EntryLo1 & 1;

    for (i = 0; i < env->tlb->nb_tlb; i++) {
        r4k_tlb_t *tlb = &env->tlb->mmu.r4k.tlb[i];
        target_ulong both = mask | tlb->PageMask;

        if (i == idx || tlb->EHINV) {
            continue;
        }
        if ((tlb->VPN & ~both) != (VPN & ~both)) {
            continue;
        }
        if (!G && !tlb->G && tlb->ASID != ASID) {
            continue;
        }
        /*
         * Only an entry that could translate leaves anything behind to
         * discard. The guest writes invalid entries sharing one page
         * number as a matter of course -- a context switch installs
         * several from a table whose unused slots hold the same sentinel
         * -- and those still count as matches, so they are still
         * invalidated and still recorded, but flushing for them would
         * throw away every cached translation to remove none.
         */
        translating |= tlb->V0 || tlb->V1;
        tlb->EHINV = 1;
        conflict = true;
    }

    if (conflict) {
        env->CP0_Status |= 1 << CP0St_TS;
    }
    if (translating) {
        /*
         * An invalidated entry may live on as a shadow copy, and its pages
         * may be in QEMU's own TLB: discard both, or a stale translation
         * outlives the entry that carried it.
         */
        r4k_mips_tlb_flush_extra(env, env->tlb->nb_tlb);
        cpu_mips_tlb_flush(env);
    }
#endif
}

/*
 * The R10000 watch registers hold a doubleword's physical address, and a
 * load from it or a store to it must raise the Watch exception whichever
 * virtual window the access came through. QEMU's watchpoint machinery is
 * virtual, so the two are bridged where every translation goes by: when a
 * fill maps a page containing the watched doubleword, a CPU watchpoint is
 * planted at the doubleword's address inside that virtual page, which
 * forces the page's accesses onto the slow path and hands overlapping ones
 * to mips_debug_check_watchpoint() below. Arming flushes the QEMU TLB, so
 * pages already resident are re-discovered through this path too.
 *
 * A planted watchpoint can outlive its mapping — the guest may point the
 * virtual page somewhere else. It is not chased at remap time; instead the
 * hit check re-translates, and a hit whose page no longer holds the
 * watched doubleword is dismissed. The stale entry costs slow-path
 * execution on that page until the next write to a watch register sweeps
 * it away.
 */
static uint64_t mips_r10k_watch_pa(CPUMIPSState *env)
{
    return ((env->CP0_WatchHi[0] & 0xff) << 32) |
           (env->CP0_WatchLo[0] & CP0WL_R10K_MASK);
}

static int mips_r10k_watch_flags(CPUMIPSState *env)
{
    uint32_t lo = env->CP0_WatchLo[0];
    int flags = BP_CPU | BP_STOP_BEFORE_ACCESS;

    if (lo & CP0WL_R10K_R) {
        flags |= BP_MEM_READ;
    }
    if (lo & CP0WL_R10K_W) {
        flags |= BP_MEM_WRITE;
    }
    return flags;
}

/*
 * Called by mips_cpu_tlb_fill() for every translation that succeeds while
 * a watch enable is set: plant the watchpoint if this physical page holds
 * the watched doubleword and no earlier fill planted it already.
 */
void mips_r10k_watch_fill(CPUState *cs, CPUMIPSState *env,
                          vaddr address, hwaddr physical)
{
    uint64_t wpa = mips_r10k_watch_pa(env);
    int flags = mips_r10k_watch_flags(env);
    CPUWatchpoint *wp;
    vaddr wva;

    if ((physical & TARGET_PAGE_MASK) != (wpa & TARGET_PAGE_MASK)) {
        return;
    }
    wva = (address & TARGET_PAGE_MASK) | (wpa & ~TARGET_PAGE_MASK);
    QTAILQ_FOREACH(wp, &cs->watchpoints, entry) {
        if (wp->vaddr == wva && wp->len == 8 &&
            (wp->flags & ~BP_WATCHPOINT_HIT) == flags) {
            return;
        }
    }
    cpu_watchpoint_insert(cs, wva, 8, flags, NULL);
}

/*
 * A watch register was just written, or reset ran: nothing planted before
 * this moment can be trusted, and a WatchLo write also empties the latch
 * of a postponed exception — the manual's own way to clear one.
 */
void mips_r10k_watch_changed(CPUMIPSState *env, bool watchlo_written)
{
    CPUState *cs = env_cpu(env);

    BQL_LOCK_GUARD();

    cpu_watchpoint_remove_all(cs, BP_CPU);
    if (watchlo_written) {
        env->r10k->CP0_R10K_DelayedWatch = 0;
        cpu_reset_interrupt(cs, CPU_INTERRUPT_DWATCH);
    }
    if (env->CP0_WatchLo[0] & (CP0WL_R10K_R | CP0WL_R10K_W)) {
        tlb_flush(cs);
    }
}

/*
 * The slow path found an access overlapping a planted watchpoint: decide
 * whether the architectural watch fires. The mapping is re-checked because
 * the plant can be stale, and the enables are re-checked against the kind
 * of access that hit. With EXL or ERL set the exception is postponed: the
 * manual has the referencing instruction execute and the exception wait,
 * so the latch is set and the access is waved through.
 */
bool mips_debug_check_watchpoint(CPUState *cs, CPUWatchpoint *wp)
{
    CPUMIPSState *env = cpu_env(cs);
    uint32_t lo = env->CP0_WatchLo[0];
    int hit = (wp->flags >> BP_HIT_SHIFT) & BP_MEM_ACCESS;
    hwaddr phys;

    if (!(env->insn_flags & INSN_R10K)) {
        return false;
    }
    if (!(((hit & BP_MEM_READ) && (lo & CP0WL_R10K_R)) ||
          ((hit & BP_MEM_WRITE) && (lo & CP0WL_R10K_W)))) {
        return false;
    }
    phys = mips_cpu_get_phys_addr_debug(cs, wp->vaddr);
    if (phys == -1 || phys != mips_r10k_watch_pa(env)) {
        return false;
    }
    if (env->CP0_Status & ((1 << CP0St_EXL) | (1 << CP0St_ERL))) {
        BQL_LOCK_GUARD();

        env->r10k->CP0_R10K_DelayedWatch = 1;
        cpu_interrupt(cs, CPU_INTERRUPT_DWATCH);
        return false;
    }
    return true;
}

/*
 * A watchpoint that mips_debug_check_watchpoint() confirmed stopped the
 * access before it ran: turn the debug exception into the guest's Watch
 * exception. The vCPU state was already rewound to the referencing
 * instruction, so EPC comes out pointing at it. A watchpoint somebody
 * armed through the gdb stub carries no BP_CPU and falls through to the
 * debugger exactly as before.
 */
void mips_debug_excp_handler(CPUState *cs)
{
    CPUWatchpoint *wp = cs->watchpoint_hit;

    if (wp && (wp->flags & BP_CPU)) {
        cs->watchpoint_hit = NULL;
        cs->exception_index = EXCP_DWATCH;
        cpu_env(cs)->error_code = 0;
        cpu_loop_exit(cs);
    }
    if (!wp) {
        mips_debug_breakpoint_hit(cs);
    }
}

/*
 * A postponed Watch exception drains at the first block boundary with
 * both EXL and ERL clear — the ERET or Status write that cleared them
 * ended its block, so EPC comes out holding the next unexecuted
 * instruction, which is where the manual wants it. The request bit
 * stays up while the latch is full, so mips_cpu_exec_interrupt()
 * revisits this until then; the bit comes down here on delivery, or in
 * mips_r10k_watch_changed() when an MTC0 to WatchLo empties the latch,
 * and Watch is maskable only by EXL and ERL, so unlike the hardware
 * interrupt lines there is no masked window to idle through. Returns
 * true when the exception was delivered.
 */
bool mips_r10k_deliver_delayed_watch(CPUState *cs)
{
    CPUMIPSState *env = cpu_env(cs);

    if (env->r10k->CP0_R10K_DelayedWatch &&
        !(env->CP0_Status & ((1 << CP0St_EXL) | (1 << CP0St_ERL)))) {
        env->r10k->CP0_R10K_DelayedWatch = 0;
        cpu_reset_interrupt(cs, CPU_INTERRUPT_DWATCH);
        cs->exception_index = EXCP_DWATCH;
        env->error_code = 0;
        mips_cpu_do_interrupt(cs);
        return true;
    }
    if (!env->r10k->CP0_R10K_DelayedWatch) {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_DWATCH);
    }
    return false;
}

void helper_mtc0_r10k_diagnostic(CPUMIPSState *env, target_ulong arg1)
{
    /*
     * Software writes the branch prediction algorithm, and the index,
     * operation and state fields through which it reaches the branch
     * prediction table. The rest of the register reports what the branch
     * caches and the instruction TLB last did, and its gaps are reserved and
     * read as zero.
     *
     * Every write also names a table operation, because the operation field
     * has no idle value: 00 reads the indexed entry into the state field, 01
     * writes the state field into the indexed entry, 10 and 11 set a whole
     * line of eight entries to zeroes or ones. On hardware a read's result
     * arrives only once the table port frees up, which is why the manual's
     * read sequence parks in a jump before looking; here it is simply done.
     */
    uint32_t idx, line, i;

    env->r10k->CP0_Diagnostic = arg1 & env->cfg.diagnostic_rw_bitmask;

    idx = extract64(env->r10k->CP0_Diagnostic, CP0DI_BPIdx, 9);
    switch (extract64(env->r10k->CP0_Diagnostic, CP0DI_BPOp, 2)) {
    case 0: /* table read */
        env->r10k->CP0_Diagnostic =
            deposit64(env->r10k->CP0_Diagnostic, CP0DI_BPState, 2,
                      env->r10k->bpt[idx]);
        break;
    case 1: /* table write */
        env->r10k->bpt[idx] =
            extract64(env->r10k->CP0_Diagnostic, CP0DI_BPState, 2);
        break;
    case 2: /* initialize a line to all zeroes (strongly not taken) */
    case 3: /* initialize a line to all ones (strongly taken) */
        line = idx & ~(R10K_BPT_LINE - 1);
        for (i = 0; i < R10K_BPT_LINE; i++) {
            env->r10k->bpt[line + i] =
                (env->r10k->CP0_Diagnostic & (1 << CP0DI_BPOp)) ? 3 : 0;
        }
        break;
    }
}

/*
 * The Diagnostic and CacheErr registers live behind env->r10k, so a
 * generated CP0 read reaches them through these helpers rather than an
 * offsetof into CPUMIPSState. CacheErr reads zero on every other processor,
 * as it did while the register was a shared field.
 */
uint64_t helper_mfc0_r10k_diagnostic(CPUMIPSState *env)
{
    return env->r10k ? env->r10k->CP0_Diagnostic : 0;
}

target_ulong helper_mfc0_r10k_cacherr(CPUMIPSState *env)
{
    return env->r10k ? (int32_t)env->r10k->CP0_CacheErr : 0;
}

void helper_mtc0_r10k_performance(CPUMIPSState *env, target_ulong arg1,
                                  uint32_t sel)
{
    /*
     * Odd selectors are the 32-bit counters, even ones the control
     * registers, which keep an event number, an interrupt enable, and one
     * count enable per processor mode. The store also settles what the
     * write means for IP[7] and for the counting machinery.
     */
    cpu_mips_store_r10k_perf(env, arg1, sel);
}

target_ulong helper_mfc0_r10k_performance(CPUMIPSState *env, uint32_t sel)
{
    /* A counter that is counting cycles is assembled from the clock. */
    return (int32_t)cpu_mips_get_r10k_perf(env, sel);
}

/*
 * The R10000 family's TLB writes take every entry the new one would overlap
 * out of matching, between the old entry going and the new one landing. The
 * shared write path prepares the index and fills the entry; only the
 * conflict scan between them is this family's own.
 */
static void r10k_helper_tlbwi(CPUMIPSState *env)
{
    int idx = r4k_tlbwi_prepare(env);

    r4k_invalidate_tlb(env, idx, 0);
    r10k_invalidate_conflicts(env, idx);
    r4k_fill_tlb(env, idx);
}

static void r10k_helper_tlbwr(CPUMIPSState *env)
{
    int r = cpu_mips_get_random(env);

    r4k_invalidate_tlb(env, r, 1);
    if (r4k_shadow_covers_staged(env)) {
        r4k_mips_tlb_flush_extra(env, env->tlb->nb_tlb);
    }
    r10k_invalidate_conflicts(env, r);
    r4k_fill_tlb(env, r);
}

/*
 * The R10000 family's TLB read. Config5 does not exist on this processor, so
 * its memory map identifier bit is clear and an entry is matched by its
 * address space identifier.
 */
static void r10k_helper_tlbr(CPUMIPSState *env)
{
    uint16_t ASID = env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask;
    r4k_tlb_t *tlb;
    int idx;

    idx = (env->CP0_Index & ~0x80000000) % env->tlb->nb_tlb;
    tlb = &env->tlb->mmu.r4k.tlb[idx];

    /* If this will change the current ASID, flush qemu's TLB.  */
    if (ASID != tlb->ASID) {
        cpu_mips_tlb_flush(env);
    }

    r4k_mips_tlb_flush_extra(env, env->tlb->nb_tlb);

    if (tlb->EHINV) {
        /*
         * This processor has no EHINV bit, but r10k_invalidate_conflicts()
         * marks a conflicting entry this way to take it out of matching, so
         * the read can still land on one. EntryHi bits 12:8 read as zero,
         * and the bit would stick if presented, because the EntryHi write
         * helper only clears it on a processor whose Config4 grants the
         * feature. Once stuck, the fill helper reads it back as "write an
         * invalid entry" and every later TLB write is silently thrown away.
         * Present the EntryHi and EntryLo of such an entry as zeroes.
         *
         * The PageMask register keeps the entry's own stale mask: a
         * read-back loads PageMask like any other read, and a guest that
         * sets PageMask once and relies on read-backs restoring the one
         * size it ever writes would otherwise inherit a 4 Kbyte mask for
         * its next TLB write.
         */
        env->CP0_EntryHi = 0;
        env->CP0_PageMask = tlb->PageMask;
        env->CP0_EntryLo0 = 0;
        env->CP0_EntryLo1 = 0;
    } else {
        /*
         * The TLBR pseudocode (R10000 User's Manual version 2.0, section
         * 14.35) reads EntryHi as the entry and-not its page mask word, so
         * virtual page number bits a guest wrote under a large page's mask
         * present as zeroes. The fill keeps the entry as written, and lookup
         * masks at compare time, so only this architectural read-back needs
         * the clearing.
         */
        env->CP0_EntryHi = (tlb->VPN & ~(target_ulong)tlb->PageMask) |
                           tlb->ASID;
        env->CP0_MemoryMapID = tlb->MMID;
        env->CP0_PageMask = tlb->PageMask;
        r4k_tlbr_load_entrylo(env, tlb);
    }
}

/*
 * The R4000-style MMU with this family's own TLB writes and reads. The
 * lookup and the fill stay shared: their differences are a few lines apiece
 * inside long functions, and a private copy of either would drift. Wire the
 * shared R4000 helpers first, then replace the three the family redefines.
 */
void r10k_mmu_init(CPUMIPSState *env, const mips_def_t *def)
{
    r4k_mmu_init(env, def);
    env->tlb->helper_tlbwi = r10k_helper_tlbwi;
    env->tlb->helper_tlbwr = r10k_helper_tlbwr;
    env->tlb->helper_tlbr = r10k_helper_tlbr;
}
