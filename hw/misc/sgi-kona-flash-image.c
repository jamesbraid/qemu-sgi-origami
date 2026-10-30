/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The image a new InfiniteReality pipe's flash EEPROM starts with; see
 * include/hw/misc/sgi-kona-flash.h.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/misc/sgi-kona-flash.h"

void sgi_kona_flash_fresh_image(uint8_t *image)
{
    memset(image, 0, SGI_KONA_FLASH_SIZE);
    stl_be_p(&image[SGI_KONA_FLASH_STATE], SGI_KONA_FLASH_STATE_GOOD);
}
