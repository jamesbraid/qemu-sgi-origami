/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_SCSI_AIC7880_H
#define HW_SCSI_AIC7880_H

#include "exec/memattrs.h"
#include "hw/pci/pci_device.h"

/* False means the diagnostic record was not retained. */
typedef bool (*AIC7880DMAReporter)(void *opaque, PCIDevice *requester,
                                   uint32_t pci_addr, bool to_memory,
                                   uint32_t length, MemTxResult result);

void aic7880_set_storage_diagnostic_reporter(PCIDevice *pci,
                                             AIC7880DMAReporter reporter,
                                             void *opaque);

#endif
