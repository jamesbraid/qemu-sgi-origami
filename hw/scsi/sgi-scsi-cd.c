/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * CD-ROM block length consistency on the SGI host adapters.
 *
 * The SGI machines turn on scsi-cd's quirk_mode_page_set_block_size
 * (hw/mips/sgi/board-cdrom.c), so a guest can switch the drive between
 * 2048- and 512-byte logical blocks with MODE SELECT, as SGI's boot PROMs
 * do. Upstream scsi-cd does not keep the rest of the target consistent
 * with that change, so the host adapters do it here instead:
 *
 * - MODE SELECT changes the block length but not the last LBA, which keeps
 *   counting blocks of the old length until a READ CAPACITY or a reset
 *   recounts it. A guest that sets 512 and reads on without READ CAPACITY
 *   is refused every block beyond a quarter of the medium.
 * - A reset keeps the selected length. A drive returns to its power-on
 *   length when reset, because the length is a mode parameter.
 *
 * This is a workaround for scsi-cd, kept out of hw/scsi/scsi-disk.c. It
 * uses only public SCSIDevice fields and the quirk's qdev property, and
 * acts only on a scsi-cd with the quirk on: passthrough devices report the
 * real drive's geometry. It sends the target no commands, so it consumes
 * no unit attention; scsi-cd's own reset raises the reset unit attention.
 *
 * Known gaps: a selected length is not migrated, as in upstream scsi-cd,
 * so a restored guest finds the power-on length; and scsi-cd's acceptance
 * of lengths other than 512 and 2048 is left as it is.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev.h"
#include "hw/scsi/scsi.h"
#include "hw/scsi/sgi-scsi-cd.h"
#include "scsi/constants.h"
#include "system/block-backend.h"

/* scsi-disk.c does not export the type name. */
#define SGI_SCSI_CD_TYPE "scsi-cd"

/*
 * Each selectable CD-ROM's block length at realize, which is its power-on
 * length. It cannot be recomputed later: scsi-cd's realize uses
 * physical_block_size only when it was set, and the block layer fills that
 * field in during realize.
 */
static GHashTable *sgi_scsi_cd_power_on;

static bool sgi_scsi_cd_selectable(DeviceState *dev)
{
    return object_dynamic_cast(OBJECT(dev), SGI_SCSI_CD_TYPE) &&
           object_property_get_bool(OBJECT(dev),
                                    "quirk_mode_page_set_block_size",
                                    &error_abort);
}

static void sgi_scsi_cd_realized(DeviceListener *listener, DeviceState *dev)
{
    if (sgi_scsi_cd_selectable(dev)) {
        g_hash_table_insert(sgi_scsi_cd_power_on, dev,
                            GINT_TO_POINTER(SCSI_DEVICE(dev)->blocksize));
    }
}

static void sgi_scsi_cd_unrealized(DeviceListener *listener,
                                   DeviceState *dev)
{
    g_hash_table_remove(sgi_scsi_cd_power_on, dev);
}

static DeviceListener sgi_scsi_cd_listener = {
    .realize = sgi_scsi_cd_realized,
    .unrealize = sgi_scsi_cd_unrealized,
};

/*
 * Registered on first use. Machine creation resets the machine before the
 * guest runs, so the first use is an adapter's reset hold phase, and the
 * registration's walk of the realized devices finds every cold-plugged
 * CD-ROM still at its power-on length. Later hot-plugged ones are recorded
 * as they are realized. The walk also replays realize to every other
 * registered listener, as any device_listener_register() after machine
 * creation does.
 */
static void sgi_scsi_cd_listen(void)
{
    if (!sgi_scsi_cd_power_on) {
        sgi_scsi_cd_power_on = g_hash_table_new(NULL, NULL);
        device_listener_register(&sgi_scsi_cd_listener);
    }
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

void sgi_scsi_cd_device_reset(SCSIDevice *dev)
{
    gpointer length;

    sgi_scsi_cd_listen();
    if (g_hash_table_lookup_extended(sgi_scsi_cd_power_on, dev, NULL,
                                     &length)) {
        dev->blocksize = GPOINTER_TO_INT(length);
        sgi_scsi_cd_recount(dev);
    }
}

void sgi_scsi_cd_bus_reset(SCSIBus *bus)
{
    BusChild *kid;

    QTAILQ_FOREACH(kid, &bus->qbus.children, sibling) {
        sgi_scsi_cd_device_reset(SCSI_DEVICE(kid->child));
    }
}

void sgi_scsi_cd_adapter_reset(DeviceState *hba)
{
    BusState *bus;

    QLIST_FOREACH(bus, &hba->child_bus, sibling) {
        SCSIBus *scsi = (SCSIBus *)object_dynamic_cast(OBJECT(bus),
                                                       TYPE_SCSI_BUS);

        if (scsi) {
            sgi_scsi_cd_bus_reset(scsi);
        }
    }
}
