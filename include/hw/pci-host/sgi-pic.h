/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_PCI_HOST_SGI_PIC_H
#define HW_PCI_HOST_SGI_PIC_H

#include "hw/pci/pci_host.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qom/object.h"

#define TYPE_SGI_PIC "sgi-pic"
#define TYPE_SGI_PIC_V5_TEST "sgi-pic-v5-test"
OBJECT_DECLARE_SIMPLE_TYPE(SGIPICState, SGI_PIC)

/* One PIC occupies one XIO widget and exposes two independent PCI heads. */
MemoryRegion *sgi_pic_widget(SGIPICState *s);
PCIBus *sgi_pic_pci_bus(SGIPICState *s, unsigned head);
/* Board code wires each head's pci-intx outputs to its eight pin inputs. */
DeviceState *sgi_pic_head_device(SGIPICState *s, unsigned head);

/* Select the reviewed IP53 functional composition before node construction. */
bool sgi_pic_enable_pxbow_experiment(SGIPICState *s, uint64_t span,
                                     Error **errp);
bool sgi_pic_attach_pxbow_packet_endpoint(SGIPICState *s,
                                          MemoryRegion *packet_face,
                                          Error **errp);
MemoryRegion *sgi_pic_pxbow_widget_zero(SGIPICState *s);
MemoryRegion *sgi_pic_routed_widget(SGIPICState *s);
uint8_t sgi_pic_pxbow_revision(SGIPICState *s);

#endif
