/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_PCI_HOST_SGI_PIC_H
#define HW_PCI_HOST_SGI_PIC_H

#include "hw/pci/pci_host.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SGI_PIC "sgi-pic"
OBJECT_DECLARE_SIMPLE_TYPE(SGIPICState, SGI_PIC)

/* One PIC occupies one XIO widget and exposes two independent PCI heads. */
MemoryRegion *sgi_pic_widget(SGIPICState *s);
PCIBus *sgi_pic_pci_bus(SGIPICState *s, unsigned head);
/* Board code wires each head's pci-intx outputs to its eight pin inputs. */
DeviceState *sgi_pic_head_device(SGIPICState *s, unsigned head);

#endif
