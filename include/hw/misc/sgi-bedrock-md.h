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
 * Lines whose directory word the unit rewrites as it serves processor
 * requests. The hardware does this for every line; the model does it only
 * for the few whose word software has lately read or written through the
 * directory backdoor, the one place the state shows. Each is overlaid on
 * the memory window so the unit sees the processors' cached loads and
 * stores to it, and the processors' cache instructions and prefetches that
 * name it are passed on by the board. A line takes one overlay, a memory
 * map change, when it is admitted; a slot is only reused once it has been
 * idle a while, which bounds that cost when software walks the whole
 * directory, and a line idle that long is released, so no overlay outlives
 * the software that read its word (docs/datasheets/bedrock-memory-
 * directory.md).
 */
#define SGI_BEDROCK_MD_WATCHED_LINES 16
/*
 * Lines a processor lately named in a secondary-cache instruction. A
 * directory word read soon after such a flush is being checked against the
 * protocol, so its line is followed even when every slot is busy.
 */
#define SGI_BEDROCK_MD_NAMED_LINES 8
/* Every processor of a four-node machine. */
#define SGI_BEDROCK_MD_LINE_HOLDERS 16

/* The requesting processor's directory pointer, if the board knows it. */
typedef bool (*SGIBedrockMDRequesterFn)(void *opaque, CPUState *cpu,
                                        uint16_t *pointer);
/* Whether a processor with directory pointer @pointer is fitted. */
typedef bool (*SGIBedrockMDPointerPresentFn)(void *opaque, uint16_t pointer);

/* A processor's copy of a watched line, as its secondary cache would hold. */
typedef struct SGIBedrockMDHolder {
    uint16_t pointer;
    uint8_t state;
} SGIBedrockMDHolder;

typedef struct SGIBedrockMDWatchedLine {
    MemoryRegion mr;
    SGIBedrockMDState *md;
    uint64_t address;
    uint32_t index;
    bool active;
    bool placed;
    /* Last backdoor access or processor request, in virtual time. */
    int64_t used_ns;
    uint8_t holders;
    SGIBedrockMDHolder holder[SGI_BEDROCK_MD_LINE_HOLDERS];
} SGIBedrockMDWatchedLine;

struct SGIBedrockMDState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    /*
     * The owning node, set directly by bedrock_realize() the way the local
     * block's own back-reference is (sgi-bedrock-lb.h). Reads of performance
     * counter 0, which the stock PROM uses as a software lock word, are
     * handed to sgi_bedrock_perf_counter_access() for the second rendezvous's
     * fairness yield; see sgi-bedrock.h.
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
    SGIBedrockMDWatchedLine watched[SGI_BEDROCK_MD_WATCHED_LINES];
    /* Releases the watched lines left idle. Host-side: not migrated. */
    QEMUTimer *watch_timer;
    /* Active watched lines; read without the lock by the board's filter. */
    uint32_t watched_count;
    /* Written without the lock by the processors' cache instructions. */
    uint64_t named[SGI_BEDROCK_MD_NAMED_LINES];
    uint32_t named_next;
    /* Set by the board: who issued a processor access. */
    SGIBedrockMDRequesterFn requester;
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
 * instruction; safe to call without the lock.
 */
void sgi_bedrock_md_line_named(SGIBedrockMDState *s, uint64_t address);
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
