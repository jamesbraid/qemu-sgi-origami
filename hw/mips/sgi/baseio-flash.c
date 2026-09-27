/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The BASEIO PROM flash part (docs/datasheets/baseio.md, "The flash
 * part", in the Origami repository): a 1 MB, 512K x16 top-boot part
 * answering the AMD command set with manufacturer 0x01 and device
 * 0x22da, one of the three sixteen-bit identities the IP27 PROM's loader
 * accepts, and the one Linux's JEDEC table names AM29LV800BT. Which of
 * the three SGI fitted is not established on either board that carries
 * the part; all three answer the loader the same way. The port's unlock
 * addresses are the x16 word addresses.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/baseio-flash.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qemu/units.h"
#include "system/block-backend.h"

#define BASEIO_FLASH_MANUFACTURER 0x01
#define BASEIO_FLASH_DEVICE       0x22da
#define BASEIO_FLASH_UNLOCK0      0x555
#define BASEIO_FLASH_UNLOCK1      0x2aa

/*
 * The image must be the part's own 1 MB. Without a drive the part is
 * fitted blank and reads as zeros, not the erased 0xff a real
 * unprogrammed chip would show; the firmware's loader rejects either and
 * falls back to its internal copy.
 */
PFlashCFI02 *sgi_baseio_flash_new(Object *parent, const char *name,
                                  const char *drive_id, Error **errp)
{
    DeviceState *flash = qdev_new(TYPE_PFLASH_CFI02);

    object_property_add_child(parent, "flash", OBJECT(flash));
    object_unref(OBJECT(flash));
    if (drive_id) {
        BlockBackend *blk = blk_by_name(drive_id);

        if (!blk) {
            error_setg(errp, "no drive named '%s' for the BASEIO PROM flash",
                       drive_id);
            return NULL;
        }
        if (!qdev_prop_set_drive_err(flash, "drive", blk, errp)) {
            return NULL;
        }
    }
    /* Top boot: fifteen 64 KB sectors, then 32 KB, two 8 KB and 16 KB. */
    qdev_prop_set_uint32(flash, "num-blocks0", 15);
    qdev_prop_set_uint32(flash, "sector-length0", 64 * KiB);
    qdev_prop_set_uint32(flash, "num-blocks1", 1);
    qdev_prop_set_uint32(flash, "sector-length1", 32 * KiB);
    qdev_prop_set_uint32(flash, "num-blocks2", 2);
    qdev_prop_set_uint32(flash, "sector-length2", 8 * KiB);
    qdev_prop_set_uint32(flash, "num-blocks3", 1);
    qdev_prop_set_uint32(flash, "sector-length3", 16 * KiB);
    qdev_prop_set_uint8(flash, "width", 2);
    qdev_prop_set_uint8(flash, "big-endian", 1);
    qdev_prop_set_uint16(flash, "id0", BASEIO_FLASH_MANUFACTURER);
    qdev_prop_set_uint16(flash, "id1", BASEIO_FLASH_DEVICE);
    qdev_prop_set_uint16(flash, "unlock-addr0", BASEIO_FLASH_UNLOCK0);
    qdev_prop_set_uint16(flash, "unlock-addr1", BASEIO_FLASH_UNLOCK1);
    qdev_prop_set_string(flash, "name", name);
    if (!sysbus_realize(SYS_BUS_DEVICE(flash), errp)) {
        return NULL;
    }
    return PFLASH_CFI02(flash);
}
