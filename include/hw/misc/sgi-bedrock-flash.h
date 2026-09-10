/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_BEDROCK_FLASH_H
#define HW_MISC_SGI_BEDROCK_FLASH_H

#include "qemu/units.h"
#include "hw/core/sysbus.h"

#define TYPE_SGI_BEDROCK_FLASH "sgi-bedrock-flash"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockFlashState, SGI_BEDROCK_FLASH)

/* Published logical capacity of the boot flash, across both PROM positions. */
#define SGI_BEDROCK_FLASH_SIZE (16 * MiB)

/*
 * Two PROM positions, selected a stride apart inside each boot space. Position
 * zero holds the lower half of the backing and position one the upper half.
 */
#define SGI_BEDROCK_FLASH_POSITIONS 2
#define SGI_BEDROCK_FLASH_PART_SIZE \
    (SGI_BEDROCK_FLASH_SIZE / SGI_BEDROCK_FLASH_POSITIONS)
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
 * A store at 4n from its position's base carries that part's word at byte n,
 * so each 8 MiB part occupies the first 32 MiB above its base and this is the
 * first offset within a position with no backing word behind it.
 */
#define SGI_BEDROCK_FLASH_WRITE_LIMIT (4 * SGI_BEDROCK_FLASH_PART_SIZE)

/* The local and the remote boot space. */
#define SGI_BEDROCK_FLASH_WINDOWS 2

typedef struct SGIBedrockFlashWindow {
    MemoryRegion mmio;
    SGIBedrockFlashState *adapter;
    unsigned index;
} SGIBedrockFlashWindow;

struct SGIBedrockFlashState {
    SysBusDevice parent_obj;

    /*
     * One CFI device per PROM position, each holding that part's bytes and
     * its own command state. Both boot spaces reach the same pair, so a
     * sequence begun through one is completable through the other.
     */
    DeviceState *flash[SGI_BEDROCK_FLASH_POSITIONS];
    MemoryRegion *flash_mmio[SGI_BEDROCK_FLASH_POSITIONS];

    SGIBedrockFlashWindow window[SGI_BEDROCK_FLASH_WINDOWS];
};

#endif
