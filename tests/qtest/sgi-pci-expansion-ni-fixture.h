/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef SGI_PCI_EXPANSION_NI_FIXTURE_H
#define SGI_PCI_EXPANSION_NI_FIXTURE_H

#include "hw/core/qdev.h"
#include "exec/hwaddr.h"

uint64_t sgi_ni_card_read(DeviceState *card, hwaddr offset, unsigned size);
void sgi_ni_card_write(DeviceState *card, hwaddr offset, uint64_t value,
                       unsigned size);

#endif
