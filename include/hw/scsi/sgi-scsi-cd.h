/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Keeps a scsi-cd's capacity and block length consistent on the SGI host
 * adapters, which upstream scsi-cd does not do once MODE SELECT can change
 * the length. See hw/scsi/sgi-scsi-cd.c.
 *
 * Every SGI host adapter model calls sgi_scsi_cd_command_done() from its
 * SCSIBusInfo.complete callback, and one of the reset helpers after each
 * reset that reaches its targets: a target reset, a bus reset, its own soft
 * reset and its reset hold phase. A new adapter that misses a call leaves
 * its CD-ROM counting in the wrong unit. The qtests check the completion
 * hook on every adapter. Of the resets, they drive the bus reset on the
 * ISP1040, the ISP12160 and the aic7880, the target reset on the ISP12160
 * and the ISP2200, the soft reset on the ISP1040 (BUS_CTRL) and the ISP2200
 * (CSR), and the hold phase through a system reset on the ISP1040. The
 * ISP12160's soft reset shares the ISP1040's code, and the aic7880 has no
 * hold-phase test of its own (tests/qtest/sgi-scsi-cd-test.h).
 */

#ifndef HW_SCSI_SGI_SCSI_CD_H
#define HW_SCSI_SGI_SCSI_CD_H

#include "hw/scsi/scsi.h"

/* Call first in the adapter's completion callback for @req. */
void sgi_scsi_cd_command_done(SCSIRequest *req);

/* Call after @dev has been reset, for a target or device reset. */
void sgi_scsi_cd_device_reset(SCSIDevice *dev);

/* Call after @bus has been reset. */
void sgi_scsi_cd_bus_reset(SCSIBus *bus);

/*
 * Call after every SCSI bus of @hba has been reset: from the adapter's own
 * soft reset, and from its reset hold phase, which runs after its buses'
 * devices have run theirs.
 */
void sgi_scsi_cd_adapter_reset(DeviceState *hba);

#endif /* HW_SCSI_SGI_SCSI_CD_H */
