/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/*
 * IA-64 Advanced Load Address Table architecture operations.
 */

#include "qemu/osdep.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "accel/tcg/probe.h"
#include "exec/tlb-flags.h"
#include "exec/target_page.h"
#include "cpu.h"
#include "exec/cpu-common.h"
#include "system/physmem.h"
#include "arch/arch.h"

#define IA64_ROTATING_FR_BASE 32

void ia64_store_watch_update(CPUIA64State *env)
{
    env->store_watch = (env->alat_state.alat_active_count ? 1 : 0) |
                       (env_archcpu(env)->has_peers ? 2 : 0) |
                       (env->ca_active ? 4 : 0);
}

static void ia64_alat_invalidate_entry(CPUIA64State *env, IA64AlatEntry *entry)
{
    if (!entry->valid) {
        return;
    }

    entry->valid = false;
    env->alat_state.alat_reg_slot[entry->fp][entry->reg] = 0;
    env->alat_state.alat_occupied &=
        ~(UINT32_C(1) << (entry - env->alat_state.alat));
    if (env->alat_state.alat_active_count > 0) {
        env->alat_state.alat_active_count--;
        ia64_store_watch_update(env);
    }
}

static void ia64_alat_clear(CPUIA64State *env)
{
    int i;

    memset(env->alat_state.alat_reg_slot, 0,
           sizeof(env->alat_state.alat_reg_slot));
    env->alat_state.alat_occupied = 0;
    if (env->alat_state.alat_active_count == 0) {
        return;
    }
    for (i = 0; i < IA64_ALAT_ENTRIES; i++) {
        env->alat_state.alat[i].valid = false;
    }
    env->alat_state.alat_active_count = 0;
    ia64_store_watch_update(env);
}

/*
 * Each CPU logs its recent stores' physical ranges under an odd-while-in-flight
 * sequence; another CPU replays them into its own ALAT.
 */
#define IA64_ALAT_LOG_ANY 0            /* size 0: range unknown */
#define IA64_ALAT_LOG_NONE UINT64_MAX  /* pa: the scope changed no memory */

static bool ia64_ranges_overlap(uint64_t start, uint64_t size,
                                uint64_t other_start, uint64_t other_size);

static bool ia64_alat_invalidate_overlap(CPUIA64State *env, uint64_t pa,
                                         uint64_t size)
{
    uint32_t occupied = env->alat_state.alat_occupied;
    bool hit = false;

    while (occupied) {
        unsigned i = ctz32(occupied);

        occupied &= occupied - 1;
        if (ia64_ranges_overlap(pa, size, env->alat_state.alat[i].phys_addr,
                                env->alat_state.alat[i].size)) {
            ia64_alat_invalidate_entry(env, &env->alat_state.alat[i]);
            hit = true;
        }
    }
    return hit;
}

/* Apply other CPUs' stores; true if any entry was lost. */
static bool ia64_alat_sync_remote(CPUIA64State *env)
{
    CPUState *self = env_cpu(env);
    CPUState *cs;
    bool lost = false;

    /* Order the load or entry write above before the sequence reads. */
    smp_mb();

    CPU_FOREACH(cs) {
        IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
        uint64_t seq, seen, count, k;
        unsigned inflight;
        bool clear = false;

        if (cs == self) {
            continue;
        }
        seq = qatomic_load_acquire(&cpu->alat_write_sequence);
        seen = env_archcpu(env)->alat_seen_sequence[cs->cpu_index];
        if (seq == seen) {
            continue;
        }
        /* An odd sequence has logged its range; replay it, and again later. */
        inflight = seq & 1;
        seq &= ~1ULL;
        count = (seq - seen) / 2 + inflight;
        if (count > IA64_ALAT_STORE_LOG) {
            clear = true;
        }
        for (k = 0; !clear && k < count; k++) {
            uint64_t slot = ((seen / 2) + k) % IA64_ALAT_STORE_LOG;
            uint64_t pa = qatomic_read(&cpu->alat_store_log[slot].pa);
            uint32_t size = qatomic_read(&cpu->alat_store_log[slot].size);

            if (size == IA64_ALAT_LOG_ANY) {
                clear = true;
            } else if (pa != IA64_ALAT_LOG_NONE &&
                       env->alat_state.alat_active_count != 0 &&
                       ia64_alat_invalidate_overlap(env, pa, size)) {
                lost = true;
            }
        }
        /*
         * A writer that lapped the log while we read it: replay is unsafe.
         * It fills slot seen/2 for store seen/2 + LOG before it publishes the
         * odd sequence, so even a completed count of LOG may have lost it.
         */
        if (!clear) {
            uint64_t now;

            smp_rmb();          /* the slot reads above, then the recheck */
            now = qatomic_load_acquire(&cpu->alat_write_sequence);
            clear = (now + 1) / 2 - seen / 2 >= IA64_ALAT_STORE_LOG;
        }
        if (clear) {
            if (env->alat_state.alat_active_count != 0) {
                lost = true;
            }
            ia64_alat_clear(env);
        }
        env_archcpu(env)->alat_seen_sequence[cs->cpu_index] = seq & ~1ULL;
    }
    return lost;
}

static bool ia64_alat_sync_memory_writes(CPUIA64State *env)
{
    return ia64_alat_sync_remote(env);
}

void ia64_invalidate_alat_reg_range(CPUIA64State *env, uint32_t first,
                                    uint32_t last, bool fp)
{
    uint32_t occupied = env->alat_state.alat_occupied;

    while (occupied) {
        unsigned i = ctz32(occupied);

        occupied &= occupied - 1;
        if (env->alat_state.alat[i].fp == fp &&
            env->alat_state.alat[i].reg >= first &&
            env->alat_state.alat[i].reg < last) {
            ia64_alat_invalidate_entry(env, &env->alat_state.alat[i]);
        }
    }
}

/* The ALAT compares stores on physical address bits 19:0 (reference manual 5.1). */
#define IA64_ALAT_ADDR_MASK 0xfffffULL

static bool ia64_ranges_overlap(uint64_t start, uint64_t size,
                                uint64_t other_start, uint64_t other_size)
{
    if (size == 0 || other_size == 0) {
        return false;
    }
    if (size > IA64_ALAT_ADDR_MASK || other_size > IA64_ALAT_ADDR_MASK) {
        return true;
    }
    return ((start - other_start) & IA64_ALAT_ADDR_MASK) < other_size ||
           ((other_start - start) & IA64_ALAT_ADDR_MASK) < size;
}

void ia64_invalidate_alat_phys_range(CPUIA64State *env, uint64_t pa,
                                     uint64_t size)
{
    if (env->alat_state.alat_active_count == 0) {
        return;
    }
    if (ia64_alat_sync_memory_writes(env)) {
        return;
    }
    ia64_alat_invalidate_overlap(env, pa, size);
}

void ia64_invalidate_stacked_alat(CPUIA64State *env)
{
    ia64_invalidate_alat_reg_range(env, IA64_STACKED_GR_BASE, IA64_GR_COUNT,
                                   false);
}

void ia64_invalidate_rotating_fp_alat(CPUIA64State *env)
{
    ia64_invalidate_alat_reg_range(env, IA64_ROTATING_FR_BASE, IA64_FR_COUNT,
                                   true);
}

/* ---- Advanced Load Address Table check ---- */

uint64_t ia64_alat_chk_a(CPUIA64State *env, uint64_t va, uint32_t reg)
{
    bool found = false;

    if (env->alat_state.alat_active_count != 0 &&
        !ia64_alat_sync_memory_writes(env)) {
        /* This API intentionally accepts either integer or FP entries. */
        found = reg < IA64_GR_COUNT && (env->alat_state.alat_reg_slot[0][reg] ||
                                        env->alat_state.alat_reg_slot[1][reg]);
    }
    if (found) {
        return 0;
    }
    env->cr_ifa = va;
    CPUState *cs = env_cpu(env);
    cs->exception_index = IA64_EXCP_GENERAL;
    cpu_loop_exit(cs);
    return 1;
}

void ia64_alat_invala(CPUIA64State *env)
{
    ia64_alat_clear(env);
}

uint64_t ia64_alat_load_begin(CPUIA64State *env)
{
    ia64_alat_sync_memory_writes(env);
    return 0;
}

static void ia64_set_alat(CPUIA64State *env, uint32_t reg, uint64_t addr,
                          uint32_t size, bool fp, uint64_t generation,
                          uint64_t pa)
{
    IA64MemorySpeculation spec;
    int i;

    if (reg >= IA64_GR_COUNT) {
        return;
    }
    if (pa == UINT64_MAX &&
        (!ia64_data_address_to_phys_attr(env, addr, &pa, &spec) ||
         !ia64_memory_allows_advanced_load(spec))) {
        return;
    }

    /* Registration always replaces the unique entry for this register. */
    i = env->alat_state.alat_reg_slot[fp][reg] - 1;
    if (i < 0) {
        if (env->alat_state.alat_occupied == UINT32_MAX) {
            /*
             * A full table replaces an entry round robin: an approximation
             * of the manual's advancing pointers (251110-003 5.1.1).
             */
            i = env->alat_state.alat_victim++ % IA64_ALAT_ENTRIES;
            ia64_alat_invalidate_entry(env, &env->alat_state.alat[i]);
        } else {
            i = ctz32(~env->alat_state.alat_occupied);
        }
    }
    if (!env->alat_state.alat[i].valid) {
        env->alat_state.alat_active_count++;
        ia64_store_watch_update(env);
    }
    env->alat_state.alat[i].phys_addr = pa;
    env->alat_state.alat[i].size = size;
    env->alat_state.alat[i].reg = reg;
    env->alat_state.alat[i].fp = fp;
    env->alat_state.alat[i].valid = true;
    env->alat_state.alat_reg_slot[fp][reg] = i + 1;
    env->alat_state.alat_occupied |= UINT32_C(1) << i;

    /* Stores since the load began that overlap the new entry remove it. */
    ia64_alat_sync_remote(env);
}

void ia64_alat_set(CPUIA64State *env, uint32_t reg, uint64_t addr,
                   uint32_t size, uint64_t generation)
{
    ia64_set_alat(env, reg, addr, size, false, generation, UINT64_MAX);
}

void ia64_alat_set_fp(CPUIA64State *env, uint32_t reg, uint64_t addr,
                      uint32_t size, uint64_t generation)
{
    if (reg > 1) {
        ia64_set_alat(env, reg, addr, size, true, generation, UINT64_MAX);
    }
}

/*
 * The caller samples generation before qualification and passes this PA only
 * across the immediately following RAM load, with no intervening instruction.
 * The two sequence checks still protect translation and publication.  A
 * missing qualified PA retains the architectural translation fallback.
 */
void ia64_alat_set_pa(CPUIA64State *env, uint32_t reg, uint64_t addr,
                      uint32_t size, uint64_t generation, uint64_t pa, bool fp)
{
    if (!fp || reg > 1) {
        ia64_set_alat(env, reg, addr, size, fp, generation, pa);
    }
}

static int ia64_find_alat_reg(CPUIA64State *env, uint32_t reg, bool fp,
                              uint64_t *generation, bool close_window)
{
    int found;

    if (reg >= IA64_GR_COUNT || env->alat_state.alat_active_count == 0) {
        return -1;
    }
    if (ia64_alat_sync_memory_writes(env)) {
        return -1;
    }
    *generation = 0;

    found = env->alat_state.alat_reg_slot[fp][reg] - 1;

    if (found >= 0 && close_window) {
        ia64_alat_sync_remote(env);
        found = env->alat_state.alat_reg_slot[fp][reg] - 1;
    }
    return found;
}

static bool ia64_alat_matches_addr(CPUIA64State *env,
                                   const IA64AlatEntry *entry, uint64_t addr,
                                   uint32_t size)
{
    uint64_t pa;

    if (entry->size != size) {
        return false;
    }
    if (!ia64_data_address_to_phys(env, addr, &pa)) {
        return false;
    }
    return entry->phys_addr == pa;
}

static uint64_t ia64_check_load_alat(CPUIA64State *env, uint32_t reg, bool fp,
                                     bool verify_addr, uint64_t addr,
                                     uint32_t size, uint32_t clear)
{
    uint64_t generation;
    int i;

    if (fp && reg <= 1) {
        return 0;
    }

    /* Addressless checks need only the final acquire sample below. */
    i = ia64_find_alat_reg(env, reg, fp, &generation, verify_addr);
    if (i < 0) {
        return 0;
    }
    if (verify_addr &&
        !ia64_alat_matches_addr(env, &env->alat_state.alat[i], addr, size)) {
        if (clear) {
            ia64_alat_invalidate_entry(env, &env->alat_state.alat[i]);
        }
        return 0;
    }
    ia64_alat_sync_remote(env);
    if (!env->alat_state.alat[i].valid) {
        return 0;
    }
    if (clear) {
        ia64_alat_invalidate_entry(env, &env->alat_state.alat[i]);
    }
    return 1;
}

static void ia64_alat_invalidate_register(CPUIA64State *env, uint32_t reg,
                                          bool fp)
{
    int i;

    if (reg >= IA64_GR_COUNT) {
        return;
    }
    i = env->alat_state.alat_reg_slot[fp][reg] - 1;

    if (i >= 0) {
        ia64_alat_invalidate_entry(env, &env->alat_state.alat[i]);
    }
}

void ia64_alat_invalidate_reg(CPUIA64State *env, uint32_t reg)
{
    ia64_alat_invalidate_register(env, reg, false);
}

void ia64_alat_invalidate_fp_reg(CPUIA64State *env, uint32_t reg)
{
    ia64_alat_invalidate_register(env, reg, true);
}

uint64_t ia64_alat_check_load(CPUIA64State *env, uint32_t reg, uint32_t clear)
{
    return ia64_check_load_alat(env, reg, false, false, 0, 0, clear);
}

uint64_t ia64_alat_check_load_addr(CPUIA64State *env, uint32_t reg,
                                   uint64_t addr, uint32_t size, uint32_t clear)
{
    return ia64_check_load_alat(env, reg, false, true, addr, size, clear);
}

uint64_t ia64_alat_check_load_fp(CPUIA64State *env, uint32_t reg,
                                 uint32_t clear)
{
    return ia64_check_load_alat(env, reg, true, false, 0, 0, clear);
}

uint64_t ia64_alat_check_load_fp_addr(CPUIA64State *env, uint32_t reg,
                                      uint64_t addr, uint32_t size,
                                      uint32_t clear)
{
    return ia64_check_load_alat(env, reg, true, true, addr, size, clear);
}

void ia64_alat_notify_store(CPUIA64State *env)
{
    g_assert(!env->alat_state.write_active);
    ia64_alat_invala(env);
}

/* Log the range before publishing the odd sequence, so others know it. */
void ia64_alat_write_begin(CPUIA64State *env, uint64_t addr, uint32_t size)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(env_cpu(env));
    uint64_t sequence;
    uint64_t pa = 0;
    uint32_t logged = IA64_ALAT_LOG_ANY;

    /*
     * An I/O store under icount restarts its instruction in a new block
     * before it completes, so the earlier attempt never reached its end.
     */
    ia64_alat_write_abort(env);
    env->alat_state.write_active = true;
    sequence = qatomic_read(&cpu->alat_write_sequence);
    g_assert(!(sequence & 1));
    /* Only another CPU reads the log, so a lone CPU skips the translation. */
    if (cpu->has_peers && size) {
        CPUTLBEntryFull *full;
        void *host;

        /* A page the TLB cannot map without a fault has an unknown range. */
        if (!(probe_access_full(env, addr, size, MMU_DATA_STORE,
                                cpu_mmu_index(env_cpu(env), false), true,
                                &host, &full, 0) & TLB_INVALID_MASK)) {
            pa = full->phys_addr | (addr & ~TARGET_PAGE_MASK);
            logged = size;
        }
    }
    qatomic_set(&cpu->alat_store_log[(sequence / 2) % IA64_ALAT_STORE_LOG].pa,
                pa);
    qatomic_set(&cpu->alat_store_log[(sequence / 2) % IA64_ALAT_STORE_LOG].size,
                logged);
    /* The log slot is visible before the odd sequence that names it, */
    smp_wmb();
    qatomic_set(&cpu->alat_write_sequence, sequence + 1);
    /* and the odd sequence before the faultable RAM store. */
    smp_wmb();
}

/* Publish completion; a canceled scope changed no memory, so say so. */
static void ia64_alat_write_take_token(CPUIA64State *env, bool changed)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(env_cpu(env));
    uint64_t sequence = qatomic_read(&cpu->alat_write_sequence);

    g_assert(env->alat_state.write_active);
    g_assert(sequence & 1);
    env->alat_state.write_active = false;
    if (!changed) {
        qatomic_set(&cpu->alat_store_log[(sequence / 2) %
                                         IA64_ALAT_STORE_LOG].pa,
                    IA64_ALAT_LOG_NONE);
        qatomic_set(&cpu->alat_store_log[(sequence / 2) %
                                         IA64_ALAT_STORE_LOG].size, 1);
    }
    /* Publish completion only after the RAM store is visible. */
    qatomic_store_release(&cpu->alat_write_sequence, sequence + 1);
}

static void ia64_alat_write_finish(CPUIA64State *env, uint64_t addr,
                                   uint32_t size, bool precise)
{
    ia64_alat_write_take_token(env, true);
    if (env->alat_state.alat_active_count != 0) {
        if (!precise) {
            ia64_alat_invala(env);
            return;
        }
        ia64_alat_sync_remote(env);
        ia64_invalidate_alat_store(env, addr, size);
    }
}

void ia64_alat_write_end(CPUIA64State *env, uint64_t addr, uint32_t size)
{
    ia64_alat_write_finish(env, addr, size, true);
}

void ia64_alat_write_cancel(CPUIA64State *env)
{
    /* No RAM changed, so others keep their entries. */
    ia64_alat_write_take_token(env, false);
}

void ia64_alat_write_abort(CPUIA64State *env)
{
    if (env->alat_state.write_active) {
        /* Faulted stores clear the local ALAT. */
        ia64_alat_write_finish(env, 0, 0, false);
    }
}

void ia64_invalidate_alat_store(CPUIA64State *env, uint64_t addr, uint32_t size)
{
    uint64_t pa;

    if (env->alat_state.alat_active_count == 0) {
        return;
    }
    if (!ia64_data_address_to_phys(env, addr, &pa)) {
        return;
    }

    ia64_invalidate_alat_phys_range(env, pa, size);
}
