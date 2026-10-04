/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The BASEIO PROM flash model uses a 1 MB, 512K x16 top-boot part
 * answering the AMD command set with manufacturer 0x01 and device
 * 0x22da, one of the three sixteen-bit identities the IP27 PROM's loader
 * accepts, and the one Linux's JEDEC table names AM29LV800BT. Which of
 * the three SGI fitted is not established on either board that carries
 * the part; all three answer the loader the same way. The port's unlock
 * addresses are the x16 word addresses.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/baseio-flash.h"
#include "hw/mips/sgi/initial-flash.h"
#include "hw/mips/sgi/prom-image.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "block/block-global-state.h"
#include "system/block-backend.h"

#define BASEIO_FLASH_MANUFACTURER 0x01
#define BASEIO_FLASH_DEVICE       0x22da
#define BASEIO_FLASH_UNLOCK0      0x555
#define BASEIO_FLASH_UNLOCK1      0x2aa

/*
 * Read and check an original I/O PROM for a part with no drive. The part
 * stores the file as supplied, so the whole file, not only its decoded
 * payload, has to fit.
 */
static SGIPromImage *sgi_baseio_flash_load(const char *firmware, Error **errp)
{
    g_autofree char *path = qemu_find_file(QEMU_FILE_TYPE_BIOS, firmware);
    g_autoptr(SGIPromImage) image = NULL;

    if (!path) {
        error_setg(errp, "cannot find I/O PROM image '%s'", firmware);
        return NULL;
    }
    image = sgi_prom_image_load(path, SGI_IO6_FLASH_SIZE, errp);
    if (!image) {
        return NULL;
    }
    if (g_bytes_get_size(image->file) > SGI_IO6_FLASH_SIZE) {
        error_setg(errp, "I/O PROM image '%s' exceeds flash capacity "
                   "(%d bytes)", firmware, SGI_IO6_FLASH_SIZE);
        return NULL;
    }
    return g_steal_pointer(&image);
}

/*
 * The image must be the part's own 1 MB. A drive's contents take
 * precedence and firmware is then never read. Without a drive the part
 * holds firmware's file followed by erased bytes, or with no firmware
 * either is fitted blank and reads as zeros, not the erased 0xff a real
 * unprogrammed chip would show; the firmware's loader rejects either and
 * falls back to its internal copy.
 */
PFlashCFI02 *sgi_baseio_flash_new(Object *parent, const char *name,
                                  const char *drive_id, const char *firmware,
                                  Error **errp)
{
    g_autoptr(SGIPromImage) image = NULL;
    DeviceState *flash;

    if (!drive_id && firmware) {
        image = sgi_baseio_flash_load(firmware, errp);
        if (!image) {
            return NULL;
        }
    }

    flash = qdev_new(TYPE_PFLASH_CFI02);
    object_property_add_child(parent, "flash", OBJECT(flash));
    object_unref(OBJECT(flash));
    /* A -drive id or, from the machine's io<N>-flash, a block node. */
    if (drive_id) {
        if (!blk_by_name(drive_id) && !bdrv_find_node(drive_id)) {
            error_setg(errp, "no drive or block node named '%s' for the "
                       "BASEIO PROM flash", drive_id);
            return NULL;
        }
        if (!object_property_set_str(OBJECT(flash), "drive", drive_id,
                                     errp)) {
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
    if (image) {
        MemoryRegion *array = sysbus_mmio_get_region(SYS_BUS_DEVICE(flash), 0);
        size_t size;
        const uint8_t *file = g_bytes_get_data(image->file, &size);

        g_assert(memory_region_size(array) == SGI_IO6_FLASH_SIZE);
        sgi_io6_initial_flash(memory_region_get_ram_ptr(array),
                              SGI_IO6_FLASH_SIZE, file, size);
        memory_region_flush_rom_device(array, 0, SGI_IO6_FLASH_SIZE);
    }
    return PFLASH_CFI02(flash);
}
