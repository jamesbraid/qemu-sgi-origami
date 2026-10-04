#ifndef HW_FLASH_H
#define HW_FLASH_H

/* NOR flash devices */

#include "exec/hwaddr.h"
#include "system/memory.h"
#include "qom/object.h"

/* pflash_cfi01.c */

#define TYPE_PFLASH_CFI01 "cfi.pflash01"
OBJECT_DECLARE_SIMPLE_TYPE(PFlashCFI01, PFLASH_CFI01)


PFlashCFI01 *pflash_cfi01_register(hwaddr base,
                                   const char *name,
                                   hwaddr size,
                                   BlockBackend *blk,
                                   uint32_t sector_len,
                                   int width,
                                   uint16_t id0, uint16_t id1,
                                   uint16_t id2, uint16_t id3,
                                   int be);
BlockBackend *pflash_cfi01_get_blk(PFlashCFI01 *fl);
MemoryRegion *pflash_cfi01_get_memory(PFlashCFI01 *fl);
/*
 * Set before realize; ops/opaque must live as long as the flash device.
 * Adapter callbacks use read/write below to reach the chip without recursion.
 * Array callbacks must be side-effect-free: they serve a short read run
 * before cached ROMD reads resume, avoiding per-word programming remaps.
 */
void pflash_cfi01_set_bus_ops(PFlashCFI01 *fl,
                             const MemoryRegionOps *ops, void *opaque);
MemTxResult pflash_cfi01_read(PFlashCFI01 *fl, hwaddr addr, uint64_t *value,
                             unsigned size, MemTxAttrs attrs);
MemTxResult pflash_cfi01_write(PFlashCFI01 *fl, hwaddr addr, uint64_t value,
                              unsigned size, MemTxAttrs attrs);
void pflash_cfi01_legacy_drive(PFlashCFI01 *dev, DriveInfo *dinfo);

/* pflash_cfi02.c */

#define TYPE_PFLASH_CFI02 "cfi.pflash02"
OBJECT_DECLARE_SIMPLE_TYPE(PFlashCFI02, PFLASH_CFI02)


PFlashCFI02 *pflash_cfi02_register(hwaddr base,
                                   const char *name,
                                   hwaddr size,
                                   BlockBackend *blk,
                                   uint32_t sector_len,
                                   int nb_mappings,
                                   int width,
                                   uint16_t id0, uint16_t id1,
                                   uint16_t id2, uint16_t id3,
                                   uint16_t unlock_addr0,
                                   uint16_t unlock_addr1,
                                   int be);

/* m25p80.c */

#define TYPE_M25P80 "m25p80-generic"

BlockBackend *m25p80_get_blk(DeviceState *dev);

#endif
