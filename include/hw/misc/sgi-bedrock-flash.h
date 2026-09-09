/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_BEDROCK_FLASH_H
#define HW_MISC_SGI_BEDROCK_FLASH_H

#include "qemu/units.h"
#include "hw/core/sysbus.h"

#define TYPE_SGI_BEDROCK_FLASH "sgi-bedrock-flash"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockFlashState, SGI_BEDROCK_FLASH)

/* Published logical capacity of the boot flash, and its erase granularity. */
#define SGI_BEDROCK_FLASH_SIZE (16 * MiB)
#define SGI_BEDROCK_FLASH_SECTOR_SIZE (64 * KiB)

/* The primary and the alternate software selection. */
#define SGI_BEDROCK_FLASH_SELECTIONS 2

typedef struct SGIBedrockFlashView {
    MemoryRegion mmio;
    SGIBedrockFlashState *adapter;
    unsigned index;
} SGIBedrockFlashView;

struct SGIBedrockFlashState {
    SysBusDevice parent_obj;

    /* The CFI device holding every byte, command and mode of the flash. */
    DeviceState *flash;
    MemoryRegion *flash_mmio;

    SGIBedrockFlashView view[SGI_BEDROCK_FLASH_SELECTIONS];
};

#endif
