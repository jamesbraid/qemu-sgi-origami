/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * QLogic ISP SCSI adapter family: what a board needs from it.
 */

#ifndef HW_SCSI_QLOGIC_ISP_H
#define HW_SCSI_QLOGIC_ISP_H

#include "hw/core/qdev.h"

#define TYPE_QLOGIC_ISP      "qlogic-isp"
#define TYPE_QLOGIC_ISP1040  "qlogic-isp1040"
#define TYPE_QLOGIC_ISP12160 "qlogic-isp12160"

/*
 * Offer this adapter the command line's own drives -- the ones named with
 * if=scsi and this adapter's bus number. The adapter passes its own bus
 * rather than the board looking one up, because an id-less device's bus is
 * named from a counter shared by every SCSI bus in the machine: the second
 * adapter's bus is not called "scsi.0", and a board that assumes it is gets
 * a null bus. Same shape as lsi53c8xx_handle_legacy_cmdline(). A
 * multi-channel adapter offers its first channel, the one a board wires.
 */
void qlogic_isp_handle_legacy_cmdline(DeviceState *dev);

#endif
