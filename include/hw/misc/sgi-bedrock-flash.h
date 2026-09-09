/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_BEDROCK_FLASH_H
#define HW_MISC_SGI_BEDROCK_FLASH_H

#include "qemu/units.h"
#include "hw/core/sysbus.h"

#define TYPE_SGI_BEDROCK_FLASH "sgi-bedrock-flash"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockFlashState, SGI_BEDROCK_FLASH)

/* Published logical capacity of the boot flash. */
#define SGI_BEDROCK_FLASH_SIZE (16 * MiB)
/*
 * The granularity the firmware's own rewrite erases and reprograms in. That
 * is a property of what the firmware does, not a published property of
 * whatever part is fitted.
 */
#define SGI_BEDROCK_FLASH_SECTOR_SIZE (64 * KiB)

/* Each boot space, and the second software read address one stride into it. */
#define SGI_BEDROCK_FLASH_WINDOW_SIZE (128 * MiB)
#define SGI_BEDROCK_FLASH_SECOND_READ (32 * MiB)

/*
 * A store at window offset 4n carries the word at backing byte n, so the
 * 16 MiB backing occupies the first 64 MiB of the window and this is the
 * first offset with no backing word behind it.
 */
#define SGI_BEDROCK_FLASH_WRITE_LIMIT (4 * SGI_BEDROCK_FLASH_SIZE)

/* The local and the remote boot space. */
#define SGI_BEDROCK_FLASH_WINDOWS 2

typedef struct SGIBedrockFlashWindow {
    MemoryRegion mmio;
    SGIBedrockFlashState *adapter;
    unsigned index;
} SGIBedrockFlashWindow;

struct SGIBedrockFlashState {
    SysBusDevice parent_obj;

    /* The CFI device holding every byte, command and mode of the flash. */
    DeviceState *flash;
    MemoryRegion *flash_mmio;

    SGIBedrockFlashWindow window[SGI_BEDROCK_FLASH_WINDOWS];
};

#endif
