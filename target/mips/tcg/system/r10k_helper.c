/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The R10000 family's system helpers: the CACHE instruction's operations
 * over the cache arrays the CPU state carries, the physical
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
#include "qemu/bswap.h"
#include "qemu/log.h"
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
    return env->r10k->scache_size;
}

/* Realize works the geometry out once; see r10k_scache_geometry_init(). */
static unsigned int r10k_scache_line_size(const CPUMIPSState *env)
{
    return env->r10k->scache_line_size;
}

static unsigned int r10k_scache_sets(const CPUMIPSState *env)
{
    return env->r10k->scache_sets;
}

static unsigned int r10k_scache_quad_sets(const CPUMIPSState *env)
{
    return env->r10k->scache_quad_sets;
}

static bool r10k_scache_geometry_valid(const CPUMIPSState *env)
{
    return env->r10k->scache_geometry_valid;
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
 * zero. The opt-in data-cache view moves dirty data to secondary before a
 * writeback operation calls this invalidation.
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

static void r10k_scache_invalidate(CPUMIPSState *env, unsigned int set,
                                   unsigned int way, uint64_t paddr,
                                   target_ulong vaddr);
static bool r10k_scache_flush_primaries(CPUMIPSState *env, hwaddr pa);

static MemTxAttrs r10k_cache_backing_attrs(CPUMIPSState *env)
{
    MemTxAttrs attrs = { .mips_cca = 3 };

    attrs.requester_id = env->r10k->requester_id;
    return attrs;
}

static unsigned int r10k_scache_word_index(CPUMIPSState *env, hwaddr pa,
                                            unsigned int way)
{
    unsigned int quad_set = (pa / 16) & (r10k_scache_quad_sets(env) - 1);

    return (quad_set * R10K_SCACHE_WAYS + way) * 2 + ((pa >> 3) & 1);
}

static unsigned int r10k_dcache_word_index(hwaddr pa, unsigned int way)
{
    /* The opt-in view accepts direct xkphys C=3, where VA[13:2] equals PA. */
    return ((pa >> 2) & 0xfff) * R10K_PCACHE_WAYS + way;
}

static int r10k_scache_find(CPUMIPSState *env, hwaddr pa)
{
    unsigned int set = (pa / r10k_scache_line_size(env)) &
                       (r10k_scache_sets(env) - 1);

    for (unsigned int way = 0; way < R10K_SCACHE_WAYS; way++) {
        unsigned int tag = set * R10K_SCACHE_WAYS + way;

        if ((env->r10k->scache_taglo[tag] & (3 << 10)) &&
            r10k_scache_tag_pa(env, tag) == (pa >> 18)) {
            env->r10k->scache_mru[set] = way;
            return way;
        }
    }
    return -1;
}

static bool r10k_scache_writeback(CPUMIPSState *env, hwaddr pa,
                                  unsigned int way)
{
    R10KState *r10k = env->r10k;
    unsigned int line_size = r10k_scache_line_size(env);
    unsigned int set = (pa / line_size) & (r10k_scache_sets(env) - 1);
    unsigned int tag = set * R10K_SCACHE_WAYS + way;
    uint8_t bytes[R10K_SCACHE_LINE_SIZE];

    if ((r10k->scache_taglo[tag] & (3 << 10)) != (3 << 10)) {
        return true;
    }
    pa &= ~(hwaddr)(line_size - 1);
    for (unsigned int i = 0; i < line_size; i += 8) {
        stq_be_p(bytes + i,
                 r10k->scache_data[r10k_scache_word_index(env, pa + i, way)]);
    }
    if (address_space_write(&MIPS_CPU(env_cpu(env))->cache_backing_as, pa,
                            r10k_cache_backing_attrs(env), bytes,
                            line_size) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "R10000 cache writeback failed at 0x%" HWADDR_PRIx
                      "\n", pa);
        return false;
    }
    r10k->scache_taglo[tag] = (r10k->scache_taglo[tag] & ~(3 << 10)) |
                               (1 << 10);
    return true;
}

static int r10k_scache_fill(CPUMIPSState *env, hwaddr pa)
{
    R10KState *r10k = env->r10k;
    unsigned int line_size = r10k_scache_line_size(env);
    unsigned int set = (pa / line_size) & (r10k_scache_sets(env) - 1);
    unsigned int way;
    unsigned int tag;
    uint8_t bytes[R10K_SCACHE_LINE_SIZE];
    int found = r10k_scache_find(env, pa);

    if (found >= 0) {
        return found;
    }
    way = !(r10k->scache_taglo[set * R10K_SCACHE_WAYS] & (3 << 10)) ? 0 :
          !(r10k->scache_taglo[set * R10K_SCACHE_WAYS + 1] & (3 << 10)) ? 1 :
          r10k->scache_mru[set] ^ 1;
    tag = set * R10K_SCACHE_WAYS + way;
    if (r10k->scache_taglo[tag] & (3 << 10)) {
        hwaddr old = (r10k_scache_tag_pa(env, tag) << 18) |
                     (pa & (0x3ffff & ~(hwaddr)(line_size - 1)));

        if (!r10k_scache_flush_primaries(env, old) ||
            !r10k_scache_writeback(env, old, way)) {
            return -1;
        }
        r10k_scache_invalidate(env, set, way, old, old);
    }
    pa &= ~(hwaddr)(line_size - 1);
    if (address_space_read(&MIPS_CPU(env_cpu(env))->cache_backing_as, pa,
                           r10k_cache_backing_attrs(env), bytes,
                           line_size) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "R10000 cache fill failed at 0x%" HWADDR_PRIx "\n",
                      pa);
        return -1;
    }
    for (unsigned int i = 0; i < line_size; i += 8) {
        r10k->scache_data[r10k_scache_word_index(env, pa + i, way)] =
            ldq_be_p(bytes + i);
    }
    r10k->scache_taglo[tag] = (uint32_t)(extract64(pa, 18, 18) << 14) |
                               ((uint32_t)extract64(pa, 12, 2) << 7) |
                               (1 << 10);
    r10k->scache_taghi[tag] = extract64(pa, 36, 4);
    r10k->scache_mru[set] = way;
    return way;
}

static bool r10k_dcache_writeback(CPUMIPSState *env, hwaddr pa,
                                  unsigned int way)
{
    R10KState *r10k = env->r10k;
    unsigned int set = (pa >> 5) & (R10K_DCACHE_SETS - 1);
    unsigned int tag = set * R10K_PCACHE_WAYS + way;
    int sway;
    unsigned int stag;

    if ((r10k->dcache_taglo[tag] & (3 << 6)) != (3 << 6)) {
        return true;
    }
    pa &= ~(hwaddr)(R10K_DCACHE_LINE_SIZE - 1);
    sway = r10k_scache_fill(env, pa);
    if (sway < 0) {
        return false;
    }
    stag = ((pa / r10k_scache_line_size(env)) &
            (r10k_scache_sets(env) - 1)) * R10K_SCACHE_WAYS + sway;
    for (unsigned int i = 0; i < R10K_DCACHE_LINE_SIZE; i += 4) {
        hwaddr word_pa = pa + i;
        unsigned int index = r10k_scache_word_index(env, word_pa, sway);
        uint64_t word = r10k->scache_data[index];
        uint32_t data = r10k->dcache_data[
            r10k_dcache_word_index(word_pa, way)];

        r10k->scache_data[index] = (word_pa & 4) ?
            (word & UINT64_C(0xffffffff00000000)) | data :
            (word & UINT64_C(0x00000000ffffffff)) | ((uint64_t)data << 32);
    }
    r10k->scache_taglo[stag] = (r10k->scache_taglo[stag] & ~(3 << 10)) |
                               (3 << 10);
    r10k->dcache_taglo[tag] = (r10k->dcache_taglo[tag] & ~(3 << 6)) |
                               (1 << 6);
    return true;
}

static int r10k_dcache_fill(CPUMIPSState *env, hwaddr pa)
{
    R10KState *r10k = env->r10k;
    unsigned int set = (pa >> 5) & (R10K_DCACHE_SETS - 1);
    unsigned int way;
    unsigned int tag;
    int sway;

    for (way = 0; way < R10K_PCACHE_WAYS; way++) {
        tag = set * R10K_PCACHE_WAYS + way;
        if ((r10k->dcache_taglo[tag] & (3 << 6)) &&
            r10k_dcache_tag_pa(env, tag) == (pa >> 12)) {
            r10k->dcache_lru[set] = way;
            return way;
        }
    }
    way = !(r10k->dcache_taglo[set * R10K_PCACHE_WAYS] & (3 << 6)) ? 0 :
          !(r10k->dcache_taglo[set * R10K_PCACHE_WAYS + 1] & (3 << 6)) ? 1 :
          r10k->dcache_lru[set] ^ 1;
    tag = set * R10K_PCACHE_WAYS + way;
    if (r10k->dcache_taglo[tag] & (3 << 6)) {
        hwaddr old = (r10k_dcache_tag_pa(env, tag) << 12) |
                     ((set & 0x7f) << 5);

        if (!r10k_dcache_writeback(env, old, way)) {
            return -1;
        }
    }
    pa &= ~(hwaddr)(R10K_DCACHE_LINE_SIZE - 1);
    sway = r10k_scache_fill(env, pa);
    if (sway < 0) {
        return -1;
    }
    for (unsigned int i = 0; i < R10K_DCACHE_LINE_SIZE; i += 4) {
        hwaddr word_pa = pa + i;
        uint64_t dword = r10k->scache_data[
            r10k_scache_word_index(env, word_pa, sway)];

        r10k->dcache_data[r10k_dcache_word_index(word_pa, way)] =
            (word_pa & 4) ? dword : dword >> 32;
    }
    r10k->dcache_taglo[tag] = (uint32_t)(extract64(pa, 12, 24) << 8) |
                               (1 << 6);
    r10k->dcache_taghi[tag] = extract64(pa, 36, 4);
    r10k->dcache_lru[set] = way;
    return way;
}

static bool r10k_scache_flush_primaries(CPUMIPSState *env, hwaddr pa)
{
    hwaddr block = pa & ~(hwaddr)(r10k_scache_line_size(env) - 1);

    for (unsigned int set = 0; set < R10K_DCACHE_SETS; set++) {
        for (unsigned int way = 0; way < R10K_PCACHE_WAYS; way++) {
            unsigned int tag = set * R10K_PCACHE_WAYS + way;
            hwaddr line = (r10k_dcache_tag_pa(env, tag) << 12) |
                          ((set & 0x7f) << 5);

            if ((env->r10k->dcache_taglo[tag] & (3 << 6)) == (3 << 6) &&
                (line & ~(hwaddr)(r10k_scache_line_size(env) - 1)) ==
                block) {
                if (!r10k_dcache_writeback(env, line, way)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool r10k_cache_backing_is_ram(CPUMIPSState *env, hwaddr pa,
                                       MemTxAttrs attrs)
{
    hwaddr line_size = r10k_scache_line_size(env);
    hwaddr line = pa & ~(line_size - 1);
    hwaddr len = line_size;
    hwaddr xlat;
    MemoryRegion *mr;

    RCU_READ_LOCK_GUARD();
    mr = address_space_translate(&MIPS_CPU(env_cpu(env))->cache_backing_as,
                                 line, &xlat, &len, false, attrs);
    return mr && memory_region_is_ram(mr) &&
           !memory_region_is_ram_device(mr) && len == line_size;
}

static MemTxResult r10k_cache_read(void *opaque, hwaddr pa, uint64_t *data,
                                    unsigned size, MemTxAttrs attrs)
{
    MIPSCPU *cpu = opaque;
    CPUMIPSState *env = &cpu->env;
    uint8_t bytes[8] = { };
    MemTxResult result;

    if (attrs.mips_cca != 3 ||
        !r10k_cache_backing_is_ram(env, pa, attrs)) {
        result = address_space_read(&cpu->cache_backing_as, pa, attrs,
                                    bytes, size);
        if (result != MEMTX_OK) {
            return result;
        }
    } else {
        for (unsigned int i = 0; i < size; i++) {
            hwaddr byte_pa = pa + i;
            int way = r10k_dcache_fill(env, byte_pa);
            uint32_t word;

            if (way < 0) {
                return MEMTX_ERROR;
            }
            word = env->r10k->dcache_data[
                r10k_dcache_word_index(byte_pa, way)];

            bytes[i] = word >> ((3 - (byte_pa & 3)) * 8);
        }
    }
    *data = 0;
    for (unsigned int i = 0; i < size; i++) {
        *data = (*data << 8) | bytes[i];
    }
    return MEMTX_OK;
}

static MemTxResult r10k_cache_write(void *opaque, hwaddr pa, uint64_t data,
                                     unsigned size, MemTxAttrs attrs)
{
    MIPSCPU *cpu = opaque;
    CPUMIPSState *env = &cpu->env;
    uint8_t bytes[8];

    for (unsigned int i = 0; i < size; i++) {
        bytes[i] = data >> ((size - 1 - i) * 8);
    }
    if (attrs.mips_cca != 3 ||
        !r10k_cache_backing_is_ram(env, pa, attrs)) {
        return address_space_write(&cpu->cache_backing_as, pa, attrs,
                                   bytes, size);
    }
    for (unsigned int i = 0; i < size; i++) {
        hwaddr byte_pa = pa + i;
        int way = r10k_dcache_fill(env, byte_pa);
        unsigned int index;
        unsigned int shift;
        unsigned int set;
        unsigned int tag;

        if (way < 0) {
            return MEMTX_ERROR;
        }
        index = r10k_dcache_word_index(byte_pa, way);
        shift = (3 - (byte_pa & 3)) * 8;
        set = (byte_pa >> 5) & (R10K_DCACHE_SETS - 1);
        tag = set * R10K_PCACHE_WAYS + way;

        env->r10k->dcache_data[index] =
            (env->r10k->dcache_data[index] & ~(0xffu << shift)) |
            ((uint32_t)bytes[i] << shift);
        env->r10k->dcache_taglo[tag] =
            (env->r10k->dcache_taglo[tag] & ~(3 << 6)) | (3 << 6);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps r10k_cache_ops = {
    .read_with_attrs = r10k_cache_read,
    .write_with_attrs = r10k_cache_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

void r10k_cache_memory_init(MIPSCPU *cpu)
{
    address_space_init(&cpu->cache_backing_as, CPU(cpu)->memory,
                       "r10k-cache-backing");
    memory_region_init_io(&cpu->cache_memory, OBJECT(cpu), &r10k_cache_ops,
                          cpu, "r10k-data-cache", UINT64_MAX);
    cpu->cache_memory_initialized = true;
}

void r10k_cache_memory_destroy(MIPSCPU *cpu)
{
    if (cpu->cache_memory_initialized) {
        address_space_destroy(&cpu->cache_backing_as);
        cpu->cache_memory_initialized = false;
    }
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

    for (i = 0; i < r10k_scache_line_size(env) / R10K_ICACHE_LINE_SIZE; i++) {
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
    for (i = 0; i < r10k_scache_line_size(env) / R10K_DCACHE_LINE_SIZE; i++) {
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
 * invalidated way. The opt-in data-cache view writes a dirty block to the
 * backing address space before calling this invalidation.
 */
static void r10k_scache_invalidate(CPUMIPSState *env, unsigned int set,
                                   unsigned int way, uint64_t paddr,
                                   target_ulong vaddr)
{
    unsigned int tag = set * R10K_SCACHE_WAYS + way;
    unsigned int line_mask = r10k_scache_line_size(env) - 1;

    if (set >= r10k_scache_sets(env) || way >= R10K_SCACHE_WAYS ||
        tag >= r10k_scache_size(env) / r10k_scache_line_size(env) ||
        tag >= R10K_SCACHE_MAX_TAGS) {
        return;
    }

    r10k_scache_invalidate_subsets(env,
                                   (r10k_scache_tag_pa(env, tag) << 18) |
                                   (paddr & (0x3ffff & ~line_mask)),
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
 * follows a block the request found. A board that wants none links no
 * region. @set_bits is nonzero only for an index request.
 */
static bool r10k_system_request(CPUMIPSState *env, R10KSystemRequest request,
                                hwaddr paddr, unsigned int set_bits)
{
    R10KState *r10k = env->r10k;

    if (!r10k || !r10k->bus_request) {
        return false;
    }
    return r10k->bus_request(r10k->bus_request_opaque, r10k->requester_id,
                             request, paddr, set_bits);
}

/*
 * A prefetch with a load hint asks for the block to read, with a store hint
 * for it exclusive (R10000 User's Manual version 2.0, Table 1-1); the other
 * hints are undefined and do nothing. A prefetch takes no memory-management
 * exception, so an address that does not translate, or translates
 * uncached, is simply dropped.
 */
void helper_r10k_prefetch(CPUMIPSState *env, target_ulong addr, uint32_t hint)
{
    MMUAccessType access = hint & 1 ? MMU_DATA_STORE : MMU_DATA_LOAD;
    MemTxAttrs attrs = {};
    hwaddr paddr;
    int prot;

    /* Translation emits no call for an undefined hint or without a board. */
    if (get_physical_address(env, &paddr, &prot, addr, access,
                             mips_env_mmu_index(env), &attrs) !=
        TLBRET_MATCH || (attrs.mips_cca != 4 && attrs.mips_cca != 5)) {
        /* Uncached and noncoherent algorithms make no coherent request. */
        return;
    }
    /*
     * Under coherent exclusive (4) a load miss reads exclusive, and so does
     * a load-hint prefetch; under exclusive on write (5) it reads shared
     * with the prefetch cause (Table 4-2).
     */
    r10k_system_request(env, (hint & 1) || attrs.mips_cca == 4 ?
                             R10K_REQUEST_PREFETCH_STORE :
                             R10K_REQUEST_PREFETCH_LOAD,
                        paddr, 0);
}

/*
 * The R10000 family's CACHE instruction, dispatched from helper_cache()
 * before the generic model: defined operations work the cache arrays, and
 * undefined combinations stay inert. @ra is the helper's own
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
    if (cache_type == 3 && !r10k_scache_geometry_valid(env)) {
        return;
    }

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
        unsigned int way = addr & 1;
        unsigned int tag = set * R10K_PCACHE_WAYS + way;

        if (MIPS_CPU(env_cpu(env))->cache_data_visibility &&
            (env->r10k->dcache_taglo[tag] & (3 << 6)) == (3 << 6)) {
            hwaddr line = (r10k_dcache_tag_pa(env, tag) << 12) |
                          ((set & 0x7f) << 5);

            if (!r10k_dcache_writeback(env, line, way)) {
                return;
            }
        }
        r10k_dcache_invalidate(env, tag);
        return;
    }
    if ((cache_type == 0 && cache_operation == 0b100) ||
        (cache_type == 1 && (cache_operation == 0b100 ||
                             cache_operation == 0b101))) {
        /*
         * Hit Invalidate (I)/(D) and Hit WriteBack Invalidate (D): both
         * ways at the virtual index are compared against the physical
         * address, and a valid matching way is invalidated. The write
         * back distinguishing the last of the three moves data only in
         * the opt-in data-cache view.
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
                    if (MIPS_CPU(env_cpu(env))->cache_data_visibility &&
                        cache_operation == 0b101 &&
                        !r10k_dcache_writeback(env, paddr, w)) {
                        return;
                    }
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
        unsigned int set = (paddr / r10k_scache_line_size(env)) &
                           (r10k_scache_sets(env) - 1);

        /*
         * A block the board follows is written back if dirty, and a
         * block write or nothing goes out on the interface either way
         * (the processor block write and eliminate protocols).
         */
        unsigned int tag = set * R10K_SCACHE_WAYS + way;

        if (set >= r10k_scache_sets(env) || way >= R10K_SCACHE_WAYS ||
            tag >= r10k_scache_size(env) / r10k_scache_line_size(env) ||
            tag >= R10K_SCACHE_MAX_TAGS) {
            return;
        }
        r10k_system_request(env, R10K_REQUEST_INDEX_WRITEBACK_INVALIDATE,
                            paddr, ctz32(r10k_scache_sets(env)));
        if (env->r10k->scache_taglo[tag] & (3 << 10)) {
            if (MIPS_CPU(env_cpu(env))->cache_data_visibility) {
                hwaddr line = (r10k_scache_tag_pa(env, tag) << 18) |
                              (paddr & 0x3ffff);

                line &= ~(hwaddr)(r10k_scache_line_size(env) - 1);

                if (!r10k_scache_flush_primaries(env, line) ||
                    !r10k_scache_writeback(env, line, way)) {
                    return;
                }
            }
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
        unsigned int set = (paddr / r10k_scache_line_size(env)) &
                           (r10k_scache_sets(env) - 1);
        unsigned int w;
        bool hit = r10k_system_request(env, cache_operation == 0b101 ?
                                       R10K_REQUEST_HIT_WRITEBACK_INVALIDATE :
                                       R10K_REQUEST_HIT_INVALIDATE,
                                       paddr, 0);

        if (set >= r10k_scache_sets(env)) {
            return;
        }
        for (w = 0; w < R10K_SCACHE_WAYS; w++) {
            unsigned int tag = set * R10K_SCACHE_WAYS + w;

            if (tag >= r10k_scache_size(env) / r10k_scache_line_size(env) ||
                tag >= R10K_SCACHE_MAX_TAGS) {
                break;
            }
            if ((env->r10k->scache_taglo[tag] & (3 << 10)) &&
                r10k_scache_tag_pa(env, tag) == extract64(paddr, 18, 22)) {
                if (MIPS_CPU(env_cpu(env))->cache_data_visibility &&
                    cache_operation == 0b101) {
                    if (!r10k_scache_flush_primaries(env, paddr) ||
                        !r10k_scache_writeback(env, paddr, w)) {
                        return;
                    }
                }
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
     * The capacity is the one realize sized the arrays to, from the CPU
     * definition and any board reset mode. Index diagnostic operations use
     * the translated physical address and address bit zero selects the way.
     */
    if (cache_type == 3 && (cache_operation == 0b001 ||
                           cache_operation == 0b010 ||
                           cache_operation == 0b110 ||
                           cache_operation == 0b111)) {
        unsigned int way = paddr & 1;

        if (cache_operation == 0b001 || cache_operation == 0b010) {
            unsigned int set = (paddr / r10k_scache_line_size(env)) &
                               (r10k_scache_sets(env) - 1);
            unsigned int tag = set * R10K_SCACHE_WAYS + way;

            if (set >= r10k_scache_sets(env) || way >= R10K_SCACHE_WAYS ||
                tag >= r10k_scache_size(env) / r10k_scache_line_size(env) ||
                tag >= R10K_SCACHE_MAX_TAGS ||
                set >= R10K_SCACHE_MAX_SETS) {
                return;
            }
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

            if (quad >= r10k_scache_size(env) / 16 ||
                quad >= R10K_SCACHE_MAX_QUADWORDS ||
                dword >= r10k_scache_size(env) / 8 ||
                dword >= R10K_SCACHE_MAX_DWORDS) {
                return;
            }
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
    uint64_t scan_mask;

    /* 1k pages are not supported. */
    mask = env->CP0_PageMask | ~(TARGET_PAGE_MASK << 1);
    VPN = env->CP0_EntryHi & (TARGET_PAGE_MASK << 1) & env->SEGMask;
    ASID = env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask;
    G = env->CP0_EntryLo0 & env->CP0_EntryLo1 & 1;

    scan_mask = env->tlb_active_mask;
    if (idx < 64) {
        scan_mask &= ~(1ULL << idx);
    }

    while (scan_mask) {
        int i = ctz64(scan_mask);
        scan_mask &= scan_mask - 1;
        r4k_tlb_t *tlb = &env->tlb->mmu.r4k.tlb[i];
        target_ulong both = mask | tlb->PageMask;

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
        env->tlb_active_mask &= ~(1ULL << i);
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

    if (!env->cfg.watch_is_physical) {
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

    idx = extract64(env->r10k->CP0_Diagnostic, CP0DI_BPIdx,
                    env->cfg.bpt_entries == R12K_BPT_ENTRIES ? 11 : 9);
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
    const r4k_tlb_t *old = &env->tlb->mmu.r4k.tlb[r];

    /*
     * The replaced entry may live on as a shadow so QEMU's own TLB keeps
     * its pages, and lookups still match shadows. A shadow with an
     * invalid half would answer a reference to that half with TLB
     * Invalid, where the processor, which no longer holds the entry,
     * takes TLB Refill. So only an entry whose halves both translate is
     * kept; any other is discarded outright.
     */
    r4k_invalidate_tlb(env, r, old->V0 && old->V1);
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

    /* The read replaces EntryHi, ASID included, as a write to it would. */
    if ((env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask) != ASID) {
        r10k_asid_changed(env);
    }
}

/*
 * ASID changes without flushing QEMU's whole TLB.
 *
 * QEMU's own TLB caches each translation under the mmu index of the mode
 * that made the reference, not under the ASID that selected the guest TLB
 * entry. The shared MIPS code therefore discards the whole cache whenever
 * EntryHi.ASID changes, and whenever a Status segment enable (UX, SX, KX)
 * clears. Most of what that throws away is still right: references through
 * kseg0, kseg1 and xkphys, and through global entries, translate the same
 * way under every ASID, and only a few pages depend on an enable that just
 * cleared. IRIX changes ASID at every context switch, and also briefly
 * around TLB maintenance on another process's behalf, so on a multiprocessor
 * Origin the full flushes come about once a millisecond and each of them
 * also empties the translated-code jump cache.
 *
 * This family discards only the translations that can have changed. Each
 * fill that the guest TLB answered from a non-global entry is listed with
 * its page and mmu index (r10k_note_fill()); an ASID change flushes exactly
 * those pages (r10k_asid_changed()), or every index that holds one if the
 * list overflowed. The guest TLB entries themselves are untouched, and no
 * translation survives that the processor would not make under the new
 * ASID: one from a non-global entry is always listed, and one from a global
 * entry or an unmapped segment does not depend on the ASID. Changes to the
 * guest TLB keep their own flushes in the shared helpers, which still find
 * only current-ASID and global pages in QEMU's TLB.
 *
 * Status segment enables are handled the same way at index granularity: a
 * fill records which enable made its address reachable, and clearing an
 * enable flushes only the indexes that hold such a page
 * (r10k_segments_disabled()).
 */

/*
 * The Status enable that makes @address reachable, as get_physical_address()
 * checks it, or zero for the 32-bit compatibility segments that need none.
 * xkphys follows the access mode SegCtl gives it, which on a processor
 * without segment control is always the kernel mode KX selects; any other
 * mode is answered with all three enables, the conservative choice.
 */
static uint32_t r10k_segment_enables(CPUMIPSState *env, target_ulong address)
{
#if defined(TARGET_MIPS64)
    if (address <= USEG_LIMIT) {
        return 0;
    } else if (address < 0x4000000000000000ULL) {
        return 1 << CP0St_UX;
    } else if (address < 0x8000000000000000ULL) {
        return 1 << CP0St_SX;
    } else if (address < 0xC000000000000000ULL) {
        unsigned int xr = (env->CP0_SegCtl2 & CP0SC2_XR_MASK) >> CP0SC2_XR;

        if (xr & (1 << ((address >> 59) & 0x7))) {
            return (1 << CP0St_UX) | (1 << CP0St_SX) | (1 << CP0St_KX);
        }
        return 1 << CP0St_KX;
    } else if (address < 0xFFFFFFFF80000000ULL) {
        return 1 << CP0St_KX;
    }
#endif
    return 0;
}

/*
 * The family's TLB lookup: the shared R4000 one, noting for the fill that
 * follows whether the matching entry was global.
 */
static int r10k_map_address(CPUMIPSState *env, hwaddr *physical, int *prot,
                            target_ulong address, MMUAccessType access_type,
                            MemTxAttrs *attrs)
{
    int ret = r4k_map_address(env, physical, prot, address, access_type,
                              attrs);

    if (ret == TLBRET_MATCH &&
        !env->tlb->mmu.r4k.tlb[env->tlb_last_match].G) {
        env->r10k->fill_asid_dependent = true;
    }
    return ret;
}

/*
 * Record a translation about to enter QEMU's TLB at @address under
 * @mmu_idx. A lookup outside a fill (a debugger read, a prefetch) can leave
 * fill_asid_dependent set for a later fill through an unmapped segment;
 * that only lists a page that did not need listing.
 */
void r10k_note_fill(CPUMIPSState *env, vaddr address, int mmu_idx)
{
    R10KState *r10k = env->r10k;
    uint64_t page = address & TARGET_PAGE_MASK;
    uint32_t enables = r10k_segment_enables(env, address);
    uint32_t i;

    if (enables & (1 << CP0St_UX)) {
        r10k->ux_idxmap |= BIT(mmu_idx);
    }
    if (enables & (1 << CP0St_SX)) {
        r10k->sx_idxmap |= BIT(mmu_idx);
    }
    if (enables & (1 << CP0St_KX)) {
        r10k->kx_idxmap |= BIT(mmu_idx);
    }

    if (!r10k->fill_asid_dependent) {
        return;
    }
    r10k->fill_asid_dependent = false;
    r10k->asid_idxmap |= BIT(mmu_idx);
    if (r10k->asid_pages_overflow) {
        return;
    }
    for (i = 0; i < r10k->asid_npages; i++) {
        if ((r10k->asid_pages[i] & TARGET_PAGE_MASK) == page) {
            r10k->asid_pages[i] |= BIT(mmu_idx);
            return;
        }
    }
    if (r10k->asid_npages == R10K_ASID_PAGES) {
        r10k->asid_pages_overflow = true;
        return;
    }
    r10k->asid_pages[r10k->asid_npages++] = page | BIT(mmu_idx);
}

/* Forget what a flush of the indexes in @idxmap has just removed. */
static void r10k_forget_idxmap(R10KState *r10k, uint32_t idxmap)
{
    r10k->ux_idxmap &= ~idxmap;
    r10k->sx_idxmap &= ~idxmap;
    r10k->kx_idxmap &= ~idxmap;
}

/* QEMU's whole TLB has been flushed: nothing listed is left in it. */
void r10k_tlb_flushed(CPUMIPSState *env)
{
    R10KState *r10k = env->r10k;

    r10k->asid_npages = 0;
    r10k->asid_pages_overflow = false;
    r10k->asid_idxmap = 0;
    r10k_forget_idxmap(r10k, UINT32_MAX);
}

/* EntryHi.ASID has changed: discard every translation that depended on it. */
void r10k_asid_changed(CPUMIPSState *env)
{
    R10KState *r10k = env->r10k;
    CPUState *cs = env_cpu(env);
    uint32_t i;

    if (r10k->asid_pages_overflow) {
        tlb_flush_by_mmuidx(cs, r10k->asid_idxmap);
        r10k_forget_idxmap(r10k, r10k->asid_idxmap);
    } else {
        for (i = 0; i < r10k->asid_npages; i++) {
            uint64_t entry = r10k->asid_pages[i];

            tlb_flush_page_by_mmuidx(cs, entry & TARGET_PAGE_MASK,
                                     entry & ~TARGET_PAGE_MASK);
        }
    }
    r10k->asid_npages = 0;
    r10k->asid_pages_overflow = false;
    r10k->asid_idxmap = 0;
}

/*
 * Status UX, SX or KX, named in @cleared, has just gone from one to zero:
 * the segments it enabled now raise address errors, so discard the indexes
 * holding pages in them.
 */
void r10k_segments_disabled(CPUMIPSState *env, uint32_t cleared)
{
    R10KState *r10k = env->r10k;
    uint32_t idxmap = 0;

    if (cleared & (1 << CP0St_UX)) {
        idxmap |= r10k->ux_idxmap;
    }
    if (cleared & (1 << CP0St_SX)) {
        idxmap |= r10k->sx_idxmap;
    }
    if (cleared & (1 << CP0St_KX)) {
        idxmap |= r10k->kx_idxmap;
    }
    if (idxmap) {
        tlb_flush_by_mmuidx(env_cpu(env), idxmap);
        r10k_forget_idxmap(r10k, idxmap);
    }
}

/*
 * The R4000-style MMU with this family's own TLB writes and reads. The
 * lookup and the fill stay shared: their differences are a few lines apiece
 * inside long functions, and a private copy of either would drift. Wire the
 * shared R4000 helpers first, then replace the three the family redefines
 * and wrap the lookup to feed the fill bookkeeping above.
 */
void r10k_mmu_init(CPUMIPSState *env, const mips_def_t *def)
{
    r4k_mmu_init(env, def);
    env->tlb->map_address = r10k_map_address;
    env->tlb->helper_tlbwi = r10k_helper_tlbwi;
    env->tlb->helper_tlbwr = r10k_helper_tlbwr;
    env->tlb->helper_tlbr = r10k_helper_tlbr;
}
