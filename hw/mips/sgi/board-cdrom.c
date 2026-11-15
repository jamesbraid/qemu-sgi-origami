/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The SCSI CD-ROM drive on the SGI machines that can carry one.
 *
 * The drive powers on at 2048-byte logical blocks, reports its current
 * length in a block descriptor, takes a new length from MODE SELECT, and
 * returns to its power-on length when reset.
 * docs/system/mips/sgi/hardware/scsi-cdrom.rst owns the contract and its
 * evidence.
 *
 * The stock PROMs read an install CD in 512-byte blocks and select that
 * length themselves. OpenBSD trusts READ CAPACITY and lays its install CD
 * out in 2048-byte sectors, so it needs the drive back at 2048 after its
 * own bus reset; a drive that powers on at 512 leaves it unable to mount
 * the CD. The 512-byte drives SGI sold for the Indy era can be had with
 * physical_block_size=512, which a scsi-cd's own property keeps.
 *
 * scsi-cd follows MMC by default: no block descriptor and a MODE SELECT
 * block length ignored. A PROM then issues 512-byte LBAs to a device
 * counting 2048-byte blocks and fails to load sash with "execute format
 * error". The quirk properties below give scsi-cd the SCSI-2 behaviour
 * instead, as the q800 machine does for the Macintosh.
 *
 * quirk_mode_page_vendor_specific_apple is named for the Macintosh, but on
 * a CD-ROM all it changes is which MODE SELECT headers scsi-cd accepts: PF=0
 * and SP=1 as well as PF=1, SP=0. A SCSI-2 drive takes a PF=0 parameter
 * list, whose header and block descriptor are laid out as with PF=1;
 * without the quirk scsi-cd refuses one with INVALID FIELD IN CDB. Pages in
 * a PF=0 list are checked as SCSI-2 pages, and SP=1 is accepted with
 * nothing saved.
 */

#include "qemu/osdep.h"
#include "hw/core/boards.h"
#include "hw/mips/sgi/sgi-board-cdrom.h"

static GlobalProperty sgi_board_cdrom_props[] = {
    { "scsi-cd", "quirk_mode_sense_rom_use_dbd", "on" },
    { "scsi-cd", "quirk_mode_page_set_block_size", "on" },
    { "scsi-cd", "quirk_mode_page_vendor_specific_apple", "on" },
};

void sgi_board_cdrom_class_init(MachineClass *mc)
{
    compat_props_add(mc->compat_props, sgi_board_cdrom_props,
                     G_N_ELEMENTS(sgi_board_cdrom_props));
}
