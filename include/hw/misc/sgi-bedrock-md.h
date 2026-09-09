/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_BEDROCK_MD_H
#define HW_MISC_SGI_BEDROCK_MD_H

#include "hw/core/sysbus.h"

#define TYPE_SGI_BEDROCK_MD "sgi-bedrock-md"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockMDState, SGI_BEDROCK_MD)

#define SGI_BEDROCK_MD_SIZE 0x60048
#define SGI_BEDROCK_MD_DIRECTORY_SIZE 0x40000000
#define SGI_BEDROCK_MD_STORAGE 14

struct SGIBedrockMDState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion directory_mmio;
    MemoryRegion *node_mr;
    uint64_t *directory;
    uint64_t *page_counters;
    uint32_t directory_entries;
    uint64_t current_cell;
    uint64_t memory_config;
    uint64_t refresh;
    uint64_t directory_config;
    uint64_t storage[SGI_BEDROCK_MD_STORAGE];
};

void sgi_bedrock_md_soft_reset(SGIBedrockMDState *s);

#endif
