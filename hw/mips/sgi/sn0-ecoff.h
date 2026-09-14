/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * load_ecoff: load a big-endian MIPS ECOFF OMAGIC executable, the format
 * of SGI's pre-ELF toolchain. Sections load at their linked addresses plus
 * @addr_offset; bss loads zeroed. Returns the total bytes placed, or one
 * of the negative codes below (load_ecoff_strerror names them).
 */
#ifndef HW_MIPS_SN0_ECOFF_H
#define HW_MIPS_SN0_ECOFF_H

#include "exec/hwaddr.h"

#define ECOFF_LOAD_FAILED     (-1)
#define ECOFF_LOAD_NOT_ECOFF  (-2)
#define ECOFF_LOAD_BAD_HEADER (-3)
#define ECOFF_LOAD_NOT_OMAGIC (-4)
ssize_t load_ecoff(const char *filename, uint64_t *pentry, uint64_t *pgp,
                   uint64_t *lowaddr, uint64_t *highaddr, hwaddr addr_offset);
const char *load_ecoff_strerror(ssize_t error);

#endif
