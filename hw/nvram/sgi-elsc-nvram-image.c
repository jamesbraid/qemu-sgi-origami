/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The image a new SGI module system controller configuration NVRAM starts
 * with. The layout is the Origami project's PCF8584 and ELSC datasheet's, in
 * its configuration NVRAM section.
 */

#include "qemu/osdep.h"
#include "hw/nvram/sgi-elsc-nvram.h"

void sgi_elsc_nvram_fresh_image(uint8_t *image, uint8_t config,
                                uint8_t module, uint8_t clock)
{
    /*
     * Factory contents are unknown; zero is the stated approximation. A
     * controller's first power-up then writes bytes 0-11 of its block in one
     * transaction, and only these are nonzero.
     */
    memset(image, 0, SGI_ELSC_NVRAM_BYTES);
    image[SGI_ELSC_NVRAM_MARKER] = SGI_ELSC_NVRAM_MARKER_VALUE;
    memcpy(&image[SGI_ELSC_NVRAM_PASSWORD], "none", 4);
    image[SGI_ELSC_NVRAM_CONFIG] = config;
    image[SGI_ELSC_NVRAM_MODULE] = module;
    image[SGI_ELSC_NVRAM_CLOCK] = clock;
}
