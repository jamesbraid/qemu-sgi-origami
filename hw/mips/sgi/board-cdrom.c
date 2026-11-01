/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The SCSI CD-ROM drive on the SGI machines that can carry one.
 *
 * SGI's boot PROMs read an SGI install CD in 512-byte logical blocks. The
 * stock Octane PROM takes the drive's current length from the block
 * descriptor MODE SENSE returns and, when it shows 2048, sends MODE
 * SELECT(6) for 512 before its first READ(10); with no descriptor it
 * assumes 512 and sends nothing. The model's drive is a standard SCSI-2
 * CD-ROM: it powers on at 2048-byte blocks, reports them in a block
 * descriptor, and takes a new length from MODE SELECT. That is a model
 * choice for every SGI machine with a CD, not a measured drive.
 *
 * scsi-cd follows MMC by default: no block descriptor, and a MODE SELECT
 * block length ignored. A PROM then issues 512-byte LBAs to a device
 * counting 2048-byte blocks and fails to load sash with "execute format
 * error". The quirk properties below give scsi-cd the SCSI-2 behaviour
 * instead, as the q800 machine does for the Macintosh.
 */

#include "qemu/osdep.h"
#include "hw/core/boards.h"
#include "hw/mips/sgi/sgi-board-cdrom.h"

static GlobalProperty sgi_board_cdrom_props[] = {
    { "scsi-cd", "quirk_mode_sense_rom_use_dbd", "on" },
    { "scsi-cd", "quirk_mode_page_set_block_size", "on" },
};

void sgi_board_cdrom_class_init(MachineClass *mc)
{
    compat_props_add(mc->compat_props, sgi_board_cdrom_props,
                     G_N_ELEMENTS(sgi_board_cdrom_props));
}
