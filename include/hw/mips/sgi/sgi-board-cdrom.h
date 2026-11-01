/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The SCSI CD-ROM behaviour SGI's boot PROMs need from a scsi-cd.
 */

#ifndef HW_MIPS_SGI_SGI_BOARD_CDROM_H
#define HW_MIPS_SGI_SGI_BOARD_CDROM_H

#include "hw/core/boards.h"

/*
 * Make every scsi-cd on machines of class @mc answer MODE SENSE with a
 * block descriptor and take a new logical block length from MODE SELECT.
 */
void sgi_board_cdrom_class_init(MachineClass *mc);

#endif /* HW_MIPS_SGI_SGI_BOARD_CDROM_H */
