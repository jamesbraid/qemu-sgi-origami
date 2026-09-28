/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bedrock memory/directory register, ECC and bank state. */
#ifndef HW_MISC_SGI_BEDROCK_MD_H
#define HW_MISC_SGI_BEDROCK_MD_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"

#define TYPE_SGI_BEDROCK_MD "sgi-bedrock-md"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockMDState, SGI_BEDROCK_MD)

#define SGI_BEDROCK_MD_SIZE 0x60048
#define SGI_BEDROCK_MD_DIRECTORY_SIZE 0x40000000
/* One eighth of the 8 GiB M-mode node space, like the directory carrier. */
#define SGI_BEDROCK_MD_ECC_SIZE 0x40000000
#define SGI_BEDROCK_MD_MEMORY_SIZE 0x200000000ULL
/*
 * Fetch-and-op variables: one 64-byte block per variable, over node memory.
 */
#define SGI_BEDROCK_MD_FETCHOP_SIZE SGI_BEDROCK_MD_MEMORY_SIZE
#define SGI_BEDROCK_MD_FETCHOP_BLOCK 64
#define SGI_BEDROCK_MD_STORAGE 14
/*
 * Doublewords whose backdoor-written ECC byte disagrees with their data. Each
 * is overlaid on the memory window so a read is checked; a data write
 * regenerates the ECC and lifts the overlay. The stock PROM's memory test
 * holds 259 at once on a two-bank node, so the bound leaves an eight-bank
 * node room to spare.
*/
#define SGI_BEDROCK_MD_ECC_INJECTIONS 4096
#define SGI_BEDROCK_MD_PROTECTION_ENTRIES (1 << 21)
#define SGI_BEDROCK_MD_DIRECTORY_COMPAT_ENTRIES 74

/* Opaque here: only the owning sgi-bedrock.c dereferences it. */
typedef struct SGIBedrockState SGIBedrockState;

typedef struct SGIBedrockMDECCInjection {
    MemoryRegion mr;
    SGIBedrockMDState *md;
    uint64_t address;
    uint64_t raw;
    bool active;
    bool placed;
    /* When a placed slot's mark was lifted; its overlay idles from here. */
    int64_t retired_ns;
} SGIBedrockMDECCInjection;

/*
 * Memory lines whose directory word, written through the backdoor with
 * directory ECC ignored, is not a codeword. Each is overlaid on the memory
 * window so a read of the line checks the word and a write rewrites it; the
 * overlay covers the whole 128-byte line. The stock PROM's directory walk
 * holds 45 at once on a two-bank node.
*/
#define SGI_BEDROCK_MD_DIRECTORY_INJECTIONS 1024

typedef struct SGIBedrockMDDirectoryInjection {
    MemoryRegion mr;
    SGIBedrockMDState *md;
    uint64_t address;
    uint64_t offset;
    uint32_t index;
    bool active;
    bool placed;
    int64_t retired_ns;
} SGIBedrockMDDirectoryInjection;

/*
 * Pages whose lines' directory words the unit rewrites as it serves
 * processor requests. The hardware does this for every line; the model
 * does it for the pages whose words software most recently read through
 * the directory backdoor, the one place the state shows, for a
 * line a processor has named in a secondary-cache instruction. Each is
 * overlaid on the memory window so the unit sees the processors' cached
 * loads and stores to it. A page takes one overlay, a memory map change,
 * when it is admitted, into a free slot or the least recently used one.
 * Losing the overlay is the model's bookkeeping only: the processors'
 * copies of a line are kept apart from the slot, and are there again when
 * its page is watched again (docs/datasheets/bedrock-memory-directory.md).
 *
 * Sixteen slots: the one reader of transitions, the Origin 300's directory
 * diagnostic, checks one line at a time per node, and every slot adds to
 * each flat-view rebuild.
 */
#define SGI_BEDROCK_MD_WATCHED_PAGES 16
/* Every processor of a four-node machine. */
#define SGI_BEDROCK_MD_LINE_HOLDERS 16

/* Whether a processor with directory pointer @pointer is fitted. */
typedef bool (*SGIBedrockMDPointerPresentFn)(void *opaque, uint16_t pointer);

/* A processor's copy of a watched line, as its secondary cache would hold. */
typedef struct SGIBedrockMDHolder {
    uint16_t pointer;
    uint8_t state;
} SGIBedrockMDHolder;

/* The processors' copies of one line, kept whether or not it is watched. */
typedef struct SGIBedrockMDLineState {
    uint32_t index;
    uint8_t holders;
    SGIBedrockMDHolder holder[SGI_BEDROCK_MD_LINE_HOLDERS];
} SGIBedrockMDLineState;

typedef struct SGIBedrockMDWatchedPage {
    MemoryRegion mr;
    SGIBedrockMDState *md;
    uint64_t address;
    uint32_t index;
    bool active;
    bool placed;
    /* When the page was last used, on the unit's use counter. */
    uint64_t last_use;
} SGIBedrockMDWatchedPage;

struct SGIBedrockMDState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    /*
     * The owning node, set directly by bedrock_realize() the way the local
     * block's own back-reference is (sgi-bedrock-lb.h), so a memory error
     * can raise its interrupt on the node's processor interfaces.
     */
    SGIBedrockState *bedrock;
    MemoryRegion directory_mmio;
    MemoryRegion ecc_mmio;
    MemoryRegion ecc_scrub_mmio;
    MemoryRegion memory_mr;
    MemoryRegion memory_mmio;
    MemoryRegion memory_alias[8];
    MemoryRegion fetchop_mmio;
    MemoryRegion ecc_overlay;
    SGIBedrockMDECCInjection ecc_injection[SGI_BEDROCK_MD_ECC_INJECTIONS];
    SGIBedrockMDDirectoryInjection
        directory_injection[SGI_BEDROCK_MD_DIRECTORY_INJECTIONS];
    /* The active slots above by doubleword and by line, for lookup. */
    GHashTable *ecc_injection_by_raw;
    GHashTable *directory_injection_by_index;
    /*
     * Slots whose mark was lifted but whose overlay is still placed, by
     * doubleword and by line, and the timer that removes the idle ones.
     * Host-side only: nothing here changes what the guest sees.
     */
    GHashTable *ecc_retired_by_raw;
    GHashTable *directory_retired_by_index;
    QEMUTimer *retired_timer;
    SGIBedrockMDWatchedPage watched[SGI_BEDROCK_MD_WATCHED_PAGES];
    /* SGIBedrockMDLineState by line index, for every line ever followed. */
    GHashTable *line_states;
    /* The table flattened for migration; only valid around a save or load. */
    SGIBedrockMDLineState *line_state_array;
    int32_t line_state_count;
    /*
     * One bit per line a processor has named in a cache instruction, set
     * without the lock: cache-flush loops name lines by the thousand.
     */
    unsigned long *named_lines;
    /* directory_entries, as the migration bitmap's length wants it. */
    int32_t named_line_bits;
    /* Counts backdoor accesses and processor requests to watched pages. */
    uint64_t use_clock;
    /* Active watched pages; read without the lock by the board's filter. */
    uint32_t watched_count;
    /* Set by the board: whether a directory pointer names a processor. */
    SGIBedrockMDPointerPresentFn pointer_present;
    void *requester_opaque;
    uint64_t memory_error;
    uint64_t directory_error;
    uint64_t bank_alias_base[8];
    uint64_t bank_alias_size[8];
    MemoryRegion *node_mr;
    /*
     * node_as's root: the node's memory from offset zero wherever node_mr
     * itself is mapped, since an address space renders its root at the root's
     * own address in its container.
    */
    MemoryRegion node_root;
    AddressSpace node_as;
    bool node_as_initialized;
    bool memory_alias_initialized;
    uint64_t *directory;
    uint64_t *page_counters;
    uint64_t *protection;
    uint8_t *ecc;
    uint32_t directory_entries;
    uint32_t protection_entries;
    uint32_t ecc_entries;
    uint8_t installed_memory_bank_mask;
    uint16_t installed_memory_bank_mib[8];
    uint64_t directory_compat_offsets[
        SGI_BEDROCK_MD_DIRECTORY_COMPAT_ENTRIES];
    uint64_t directory_compat_values[
        SGI_BEDROCK_MD_DIRECTORY_COMPAT_ENTRIES];
    uint8_t directory_compat_count;
    uint16_t ecc_admitted_halfword_low;
    uint16_t ecc_admitted_halfword;
    uint64_t current_cell;
    uint64_t memory_config;
    uint64_t refresh;
    uint64_t directory_config;
    uint64_t storage[SGI_BEDROCK_MD_STORAGE];
    QEMUTimer *selftest_timer;
    bool selftest_running;
    uint8_t selftest_selector;
    bool selftest_phase;
    bool selftest_result;
    uint64_t selftest_error_address;
    uint8_t selftest_status;
};

void sgi_bedrock_md_soft_reset(SGIBedrockMDState *s);

/*
 * Processor requests the board passes on because no memory access carries
 * them: a secondary-cache instruction and a prefetch.
 */
typedef enum SGIBedrockMDProcessorRequest {
    SGI_BEDROCK_MD_INDEX_WRITEBACK_INVALIDATE,
    SGI_BEDROCK_MD_HIT_WRITEBACK_INVALIDATE,
    SGI_BEDROCK_MD_HIT_INVALIDATE,
    SGI_BEDROCK_MD_PREFETCH_LOAD,
    SGI_BEDROCK_MD_PREFETCH_STORE,
} SGIBedrockMDProcessorRequest;

/*
 * A processor named node memory address @address in a secondary-cache
 * instruction, which lets a later read of the line's directory word watch
 * its page. Safe to call without the lock.
 */
void sgi_bedrock_md_name_line(SGIBedrockMDState *s, uint64_t address);
/* Whether any line is watched; safe to call without the lock. */
bool sgi_bedrock_md_watching(SGIBedrockMDState *s);
/*
 * Serve @request from the processor whose directory pointer is @pointer, at
 * node memory address @address. An index request names every line whose
 * address agrees with @address in the bits @index_mask selects. Returns
 * whether the processor held a watched line the request named. Called with
 * the lock held.
 */
bool sgi_bedrock_md_processor_request(SGIBedrockMDState *s, uint16_t pointer,
                                      SGIBedrockMDProcessorRequest request,
                                      uint64_t address, uint64_t index_mask);
uint64_t sgi_bedrock_md_ecc_store_value(hwaddr offset, uint64_t value,
                                        unsigned size);
/* The memory ECC code's check byte for one doubleword of data. */
uint8_t sgi_bedrock_md_ecc_compute(uint64_t data);
/* A directory word with its check field regenerated, in the given flavor. */
uint64_t sgi_bedrock_md_directory_encode(uint64_t word, bool premium);

#endif
