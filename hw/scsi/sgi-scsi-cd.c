/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * CD-ROM block length consistency on the SGI host adapters.
 *
 * The SGI machines turn on scsi-cd's quirk_mode_page_set_block_size
 * (hw/mips/sgi/board-cdrom.c), so a guest can switch the drive between
 * 2048- and 512-byte logical blocks with MODE SELECT, as SGI's boot PROMs
 * do. Upstream scsi-cd does not keep the rest of the target consistent
 * with that change, so the host adapters do it here instead. MODE SELECT
 * changes the block length but not the last LBA, which keeps counting
 * blocks of the old length until a READ CAPACITY or a reset recounts it.
 * A guest that sets 512 and reads on without READ CAPACITY is refused
 * every block beyond a quarter of the medium.
 *
 * This is a workaround for scsi-cd, kept out of hw/scsi/scsi-disk.c. It
 * uses only public SCSIDevice fields and the quirk's qdev property, and
 * acts only on a scsi-cd with the quirk on: passthrough devices report the
 * real drive's geometry. It sends the target no commands, so it consumes
 * no unit attention.
 *
 * Known gaps: a selected length is not migrated, as in upstream scsi-cd,
 * so a restored guest finds the power-on length; and scsi-cd's acceptance
 * of lengths other than 512 and 2048 is left as it is.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/scsi/scsi.h"
#include "hw/scsi/sgi-scsi-cd.h"
#include "scsi/constants.h"
#include "system/block-backend.h"

/* scsi-disk.c does not export the type name. */
#define SGI_SCSI_CD_TYPE "scsi-cd"

static bool sgi_scsi_cd_selectable(DeviceState *dev)
{
    return object_dynamic_cast(OBJECT(dev), SGI_SCSI_CD_TYPE) &&
           object_property_get_bool(OBJECT(dev),
                                    "quirk_mode_page_set_block_size",
                                    &error_abort);
}

/* scsi-disk.c's own count, as its reset and READ CAPACITY make it. */
static void sgi_scsi_cd_recount(SCSIDevice *dev)
{
    uint64_t nb_sectors;

    blk_get_geometry(dev->conf.blk, &nb_sectors);
    nb_sectors /= dev->blocksize / BDRV_SECTOR_SIZE;
    dev->max_lba = nb_sectors ? nb_sectors - 1 : 0;
}

/*
 * After any MODE SELECT, not only a successful one: scsi-disk.c applies the
 * block descriptor before it validates the mode pages that follow, so a
 * select it refuses can still have changed the length. Recounting an
 * unchanged length is a no-op.
 */
void sgi_scsi_cd_command_done(SCSIRequest *req)
{
    if ((req->cmd.buf[0] == MODE_SELECT || req->cmd.buf[0] == MODE_SELECT_10)
        && sgi_scsi_cd_selectable(DEVICE(req->dev))) {
        sgi_scsi_cd_recount(req->dev);
    }
}
