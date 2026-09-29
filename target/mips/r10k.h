/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * R10000-family processor state
 *
 * The R10000, R12000 and R14000 add state to the shared MIPS core that no
 * other processor has: writable-bit masks a reset copies from the CPU
 * definition, the secondary and primary cache diagnostic arrays a CACHE
 * instruction reaches, and a handful of CP0 registers the family defines.
 * A CPUMIPSState carries a pointer to one of these, allocated only for an
 * R10000-family CPU (env->r10k) and NULL on every other processor, so the
 * multi-megabyte cache arrays cost nothing on a machine built from other
 * cores.
 *
 */

#ifndef MIPS_R10K_H
#define MIPS_R10K_H

/* External secondary-cache geometry used by R10000-family definitions. */
#define R10K_SCACHE_SIZE       (1024 * 1024)
#define R14K_SCACHE_SIZE       (2 * 1024 * 1024)
#define R10K_SCACHE_4M_SIZE    (4 * 1024 * 1024)
#define R10K_SCACHE_LINE_SIZE  128
#define R10K_SCACHE_WAYS       2
#define R10K_SCACHE_SETS       (R10K_SCACHE_SIZE / R10K_SCACHE_LINE_SIZE / \
                                R10K_SCACHE_WAYS)
#define R10K_SCACHE_TAGS       (R10K_SCACHE_SETS * R10K_SCACHE_WAYS)
#define R10K_SCACHE_QUADWORDS  (R10K_SCACHE_SIZE / 16)
#define R10K_SCACHE_DWORDS     (R10K_SCACHE_SIZE / 8)
#define R14K_SCACHE_SETS       (R14K_SCACHE_SIZE / R10K_SCACHE_LINE_SIZE / \
                                R10K_SCACHE_WAYS)
#define R14K_SCACHE_TAGS       (R14K_SCACHE_SETS * R10K_SCACHE_WAYS)
#define R14K_SCACHE_QUADWORDS  (R14K_SCACHE_SIZE / 16)
#define R14K_SCACHE_DWORDS     (R14K_SCACHE_SIZE / 8)
#define R10K_SCACHE_4M_SETS    (R10K_SCACHE_4M_SIZE / \
                                R10K_SCACHE_LINE_SIZE / R10K_SCACHE_WAYS)
#define R10K_SCACHE_4M_TAGS    (R10K_SCACHE_4M_SETS * R10K_SCACHE_WAYS)
#define R10K_SCACHE_4M_QUADWORDS (R10K_SCACHE_4M_SIZE / 16)
#define R10K_SCACHE_4M_DWORDS  (R10K_SCACHE_4M_SIZE / 8)
#define CP0St_R10K_DE          16
#define R10K_SCACHE_TAGLO_MASK 0xffffcdff
#define R10K_SCACHE_TAGHI_MASK 0x8000000f

/*
 * The R10000's on-chip primary caches, both 32 Kbytes and two-way set
 * associative: the instruction cache in 64-byte blocks holding predecoded
 * 36-bit instructions, the data cache in 32-byte blocks. Their index
 * diagnostic operations use the virtual address — VA[13:6] or VA[13:5]
 * names the set, VA[13:2] a word, and VA[0] the way. The tag masks are the
 * defined TagLo/TagHi bits of each cache's tag format; the
 * least-recently-used bit at TagLo[3] belongs to the set rather than to a
 * way, like the secondary cache's most-recently-used bit, and is kept
 * aside the same way.
 */
#define R10K_PCACHE_SIZE       (32 * 1024)
#define R10K_PCACHE_WAYS       2
#define R10K_PCACHE_WORDS      (R10K_PCACHE_SIZE / 4)
#define R10K_ICACHE_LINE_SIZE  64
#define R10K_ICACHE_SETS       (R10K_PCACHE_SIZE / R10K_ICACHE_LINE_SIZE / \
                                R10K_PCACHE_WAYS)
#define R10K_ICACHE_TAGS       (R10K_ICACHE_SETS * R10K_PCACHE_WAYS)
#define R10K_DCACHE_LINE_SIZE  32
#define R10K_DCACHE_SETS       (R10K_PCACHE_SIZE / R10K_DCACHE_LINE_SIZE / \
                                R10K_PCACHE_WAYS)
#define R10K_DCACHE_TAGS       (R10K_DCACHE_SETS * R10K_PCACHE_WAYS)
#define R10K_PCACHE_TAGLO_LRU  (1 << 3)
#define R10K_ICACHE_TAGLO_MASK 0xffffff4d
#define R10K_ICACHE_TAGHI_MASK 0x0000000f
#define R10K_DCACHE_TAGLO_MASK 0xffffffcf
#define R10K_DCACHE_TAGHI_MASK 0xe000000f

/*
 * The branch prediction table the Diagnostic register operates on: 512
 * two-bit entries, indexed by VA[11:3] of a branch, initialized in lines of
 * eight entries named by the upper six index bits.
 */
#define R10K_BPT_ENTRIES  512
#define R12K_BPT_ENTRIES  2048
#define R10K_BPT_LINE     8

/*
 * What the processor puts on its system interface that the board sees no
 * memory access for: a secondary-cache instruction that may write a block
 * back or drop it, and a prefetch (R10000 User's Manual version 2.0, the
 * processor block read, block write and eliminate protocols).
 */
typedef enum R10KSystemRequest {
    R10K_REQUEST_INDEX_WRITEBACK_INVALIDATE,
    R10K_REQUEST_HIT_WRITEBACK_INVALIDATE,
    R10K_REQUEST_HIT_INVALIDATE,
    R10K_REQUEST_PREFETCH_LOAD,
    R10K_REQUEST_PREFETCH_STORE,
} R10KSystemRequest;

/*
 * The processor dispatches each such request as a read of the board's
 * system-request region, as the generic CACHE model dispatches tag
 * operations to its ITC region. The read is 8 bytes and big-endian, so
 * the region is DEVICE_BIG_ENDIAN with 8-byte valid and impl sizes and
 * its value arrives unswapped. The offset carries the physical
 * address, the request and, for an index request, which names a set rather
 * than a block, the number of secondary-cache set-index bits. The value
 * read back is nonzero when the request found a block the board follows
 * for this processor; the reference's requester_id names the processor.
 */
#define R10K_SYSTEM_REQUEST_PADDR_BITS 48
#define R10K_SYSTEM_REQUEST_KIND_SHIFT 48
#define R10K_SYSTEM_REQUEST_SET_BITS_SHIFT 52
#define R10K_SYSTEM_REQUEST_SIZE (UINT64_C(1) << 58)

typedef struct R10KState {
    /*
     * Writable-bit masks, copied from the CPU definition at reset. Not
     * migrated: a reset re-derives them from the model.
     */
    int32_t CP0_TagLo_rw_bitmask;
    int32_t CP0_ECC_rw_bitmask;
    /* The Config bits a write preserves; it only ever sets the kseg0 field. */
    int32_t CP0_Config0_preserve_bitmask;
    /* The PageMask bits a write can set; the rest read back as zero. */
    int32_t CP0_PageMask_rw_bitmask;
    /*
     * The FrameMask bits this processor implements. r4k_fill_tlb() masks
     * with this as well as helper_mtc0_framemask().
     */
    int32_t CP0_Framemask_rw_bitmask;
    /*
     * Writable EntryLo bits beyond those the physical address mask names.
     * The R10000 family's uncached attribute sits there.
     */
    uint64_t CP0_EntryLo_extra_rw_bitmask;
    /* The last address xkseg maps, derived from the CPU definition at reset. */
    target_ulong xkseg_limit;

    /*
     * The timer's own contribution to Cause.IP7: raised when Count reaches
     * Compare, cleared by a Compare write. Kept apart from Cause because on
     * the R10000 family the performance counters share IP[7], so the line
     * is the OR of this and the counters' overflow condition, and neither
     * source may clear what the other asserted.
     */
    uint32_t cp0_timer_ip7;

    /*
     * CP0 Register 22, the Diagnostic register. Architecturally 64 bits wide,
     * and not all of its fields lie in the low half: the instruction TLB miss
     * counter sits at bits 51:48. The model never moves that counter — there
     * is no instruction TLB behind it — but the register is stored at its
     * architectural width so a doubleword read is a read of the whole register
     * rather than a sign extension of half of it.
     */
    uint64_t CP0_Diagnostic;
    /*
     * The branch prediction table the Diagnostic register's operation fields
     * read and write. The model predicts no branches, so the entries hold
     * what the operations put there.
     */
    uint8_t bpt[R12K_BPT_ENTRIES];

    /*
     * A Watch exception the R10000 postponed because EXL or ERL was set when
     * the watched doubleword was referenced. The latch is architecturally
     * invisible; it drains into a Watch exception when both bits clear, and
     * a system reset or an MTC0 to WatchLo empties it.
     */
    uint32_t CP0_R10K_DelayedWatch;

    /* CP0 CacheErr is copied from the reporting cache on exception delivery. */
    int32_t CP0_CacheErr;
    uint32_t scache_local_cacheerr;

    /* R10000 external secondary-cache diagnostic state. */
    uint32_t scache_taglo[R10K_SCACHE_4M_TAGS];
    uint8_t scache_taghi[R10K_SCACHE_4M_TAGS];
    uint8_t scache_mru[R10K_SCACHE_4M_SETS];
    uint64_t scache_data[R10K_SCACHE_4M_DWORDS];
    uint16_t scache_ecc[R10K_SCACHE_4M_QUADWORDS];

    /*
     * R10000 primary-cache diagnostic state. The tag arrays keep each tag's
     * defined TagLo bits except the set-wide least-recently-used bit, which
     * lives in its own per-set array; the data arrays keep a predecoded
     * 36-bit instruction or a 32-bit word per entry with the parity bits
     * software staged through the ECC register beside them.
     */
    uint32_t icache_taglo[R10K_ICACHE_TAGS];
    uint8_t icache_taghi[R10K_ICACHE_TAGS];
    uint8_t icache_lru[R10K_ICACHE_SETS];
    uint64_t icache_data[R10K_PCACHE_WORDS];
    uint8_t icache_parity[R10K_PCACHE_WORDS];
    uint32_t dcache_taglo[R10K_DCACHE_TAGS];
    uint32_t dcache_taghi[R10K_DCACHE_TAGS];
    uint8_t dcache_lru[R10K_DCACHE_SETS];
    uint32_t dcache_data[R10K_PCACHE_WORDS];
    uint8_t dcache_parity[R10K_PCACHE_WORDS];

    /*
     * Set by the board and kept across reset, so they stay last: reset
     * clears the state above them only. Not migrated.
     */
    MemoryRegion *system_requests;
    /*
     * The identity the board gives this processor's requests, carried in
     * each reference's requester_id so a memory unit can tell processors
     * apart. The board keeps it current.
     */
    uint16_t requester_id;
} R10KState;

#endif /* MIPS_R10K_H */
