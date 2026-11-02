/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Keeps a scsi-cd's capacity and block length consistent on the SGI host
 * adapters, which upstream scsi-cd does not do once MODE SELECT can change
 * the length. See hw/scsi/sgi-scsi-cd.c.
 *
 * Every SGI host adapter model calls sgi_scsi_cd_command_done() from its
 * SCSIBusInfo.complete callback. A new adapter that misses the call leaves
 * its CD-ROM counting in the wrong unit, so each adapter's qtest drives
 * it.
 */

#ifndef HW_SCSI_SGI_SCSI_CD_H
#define HW_SCSI_SGI_SCSI_CD_H

#include "hw/scsi/scsi.h"

/* Call first in the adapter's completion callback for @req. */
void sgi_scsi_cd_command_done(SCSIRequest *req);

#endif /* HW_SCSI_SGI_SCSI_CD_H */
