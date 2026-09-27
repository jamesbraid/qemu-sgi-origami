/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The flash part that holds the BASEIO PROM on a Bridge's external-flash
 * port, where the IP27 PROM's loader reads it: the Origin 2000 BaseIO
 * card's own part and the Origin 200 GIGAchannel cabinet's. The loader
 * runs the same identify and copy sequence at either, so both boards fit
 * the one part this helper builds (see hw/mips/sgi/baseio-flash.c for
 * which part and why).
 */
#ifndef HW_MIPS_SGI_BASEIO_FLASH_H
#define HW_MIPS_SGI_BASEIO_FLASH_H

#include "hw/block/flash.h"
#include "qom/object.h"

/*
 * Create the part as parent's "flash" child and realize it. drive_id
 * names a -drive whose image fills the part, or is NULL for a part
 * fitted blank. name labels the part's memory region. The caller links
 * the part's MMIO region 0 to its Bridge's "flash" property.
 */
PFlashCFI02 *sgi_baseio_flash_new(Object *parent, const char *name,
                                  const char *drive_id, Error **errp);

#endif
