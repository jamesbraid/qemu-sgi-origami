/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The InfiniteReality pipe's flash EEPROM image, shared by the sgi-kona
 * device and the tool that creates a new machine's stores. The layout is
 * the Origami project's InfiniteReality command grammar datasheet's, in
 * its flash EEPROM section: 512 KiB, the boot image from offset 0, the
 * video combination at SGI_KONA_FLASH_COMBO behind its marker and word
 * count, and a twelve-word header at SGI_KONA_FLASH_HEADER whose word 7
 * is the state the PROM and kernel accept only as 2.
 */

#ifndef HW_MISC_SGI_KONA_FLASH_H
#define HW_MISC_SGI_KONA_FLASH_H

#define SGI_KONA_FLASH_SIZE         0x80000
#define SGI_KONA_FLASH_COMBO        0x20000
#define SGI_KONA_FLASH_COMBO_MARKER 0xfacefeed
#define SGI_KONA_FLASH_HEADER       0x7ff00
#define SGI_KONA_FLASH_STATE        (SGI_KONA_FLASH_HEADER + 7 * 4)
#define SGI_KONA_FLASH_STATE_GOOD   2

/*
 * Fill the SGI_KONA_FLASH_SIZE bytes at @image with the flash a new pipe
 * starts with: zero, the model's approximation since no factory image was
 * ever captured, apart from header state 2.
 */
void sgi_kona_flash_fresh_image(uint8_t *image);

#endif /* HW_MISC_SGI_KONA_FLASH_H */
