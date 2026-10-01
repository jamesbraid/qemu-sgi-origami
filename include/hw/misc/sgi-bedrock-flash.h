/* SPDX-License-Identifier: BSD-3-Clause */
/* Origin 300 boot-flash adapter and two CFI PROM positions. */
#ifndef HW_MISC_SGI_BEDROCK_FLASH_H
#define HW_MISC_SGI_BEDROCK_FLASH_H

#include "qemu/units.h"
#include "hw/core/sysbus.h"

#define TYPE_SGI_BEDROCK_FLASH "sgi-bedrock-flash"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockFlashState, SGI_BEDROCK_FLASH)

/*
 * Published logical capacity of the boot flash, across both PROM positions.
 */
#define SGI_BEDROCK_FLASH_SIZE (16 * MiB)

/*
 * Two PROM positions, selected a stride apart inside each boot space.
 * Position zero holds the lower half of the backing and position one the
 * upper half.
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

/*
 * Each boot space, and the second software read address one stride into it.
 */
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

    /*
     * Set by the machine once it has attached a cacheable execution view over
     * a position's storage, so every store this adapter forwards to that
     * position (a command byte or a program) can refresh the view's copy
     * and invalidate any translation cached from the bytes it had before.
     * NULL when nothing reads a position outside this adapter's own MMIO
     * dispatch.
    */
    void (*programmed)(void *opaque, unsigned position);
    void *programmed_opaque;
    /*
     * Which node's flash this is: node 0's positions keep the RAM names their
     * snapshots carry, a later node's carry its index as well.
    */
    uint32_t node;
};

#endif
