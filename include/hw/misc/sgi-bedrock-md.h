/* SPDX-License-Identifier: GPL-2.0-or-later */
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
/* Fetch-and-op variables: one 64-byte block per variable, over node memory. */
#define SGI_BEDROCK_MD_FETCHOP_SIZE SGI_BEDROCK_MD_MEMORY_SIZE
#define SGI_BEDROCK_MD_FETCHOP_BLOCK 64
#define SGI_BEDROCK_MD_STORAGE 14
/*
 * Doublewords whose backdoor-written ECC byte disagrees with their data.
 * Each is overlaid on the memory window so a read is checked; a data write
 * regenerates the ECC and lifts the overlay.
 */
#define SGI_BEDROCK_MD_ECC_INJECTIONS 128
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
} SGIBedrockMDECCInjection;

/*
 * Memory lines whose directory word, written through the backdoor with
 * directory ECC ignored, is not a codeword. Each is overlaid on the memory
 * window so a read of the line checks the word; the overlay covers the
 * whole 128-byte line.
 */
#define SGI_BEDROCK_MD_DIRECTORY_INJECTIONS 32

typedef struct SGIBedrockMDDirectoryInjection {
    MemoryRegion mr;
    SGIBedrockMDState *md;
    uint64_t address;
    uint64_t offset;
    uint32_t index;
    bool active;
    bool placed;
} SGIBedrockMDDirectoryInjection;

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
    uint64_t memory_error;
    uint64_t directory_error;
    uint64_t bank_alias_base[8];
    uint64_t bank_alias_size[8];
    MemoryRegion *node_mr;
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
uint64_t sgi_bedrock_md_ecc_store_value(hwaddr offset, uint64_t value,
                                        unsigned size);
/* The memory ECC code's check byte for one doubleword of data. */
uint8_t sgi_bedrock_md_ecc_compute(uint64_t data);
/* A directory word with its check field regenerated, in the given flavor. */
uint64_t sgi_bedrock_md_directory_encode(uint64_t word, bool premium);

#endif
