/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_PCI_HOST_SGI_PIC_H
#define HW_PCI_HOST_SGI_PIC_H

#include "hw/pci/pci_host.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-bedrock-pi.h"
#include "hw/misc/sgi-xbow.h"
#include "qapi/error.h"
#include "qom/object.h"

#define TYPE_SGI_PIC "sgi-pic"
#define TYPE_SGI_PIC_V5_TEST "sgi-pic-v5-test"
#define SGI_PIC_BYTE_LANE_SELF_ORDERED "byte-lane-self-ordered"
#define SGI_PIC_LEVEL_SOURCES 16
OBJECT_DECLARE_SIMPLE_TYPE(SGIPICState, SGI_PIC)

/* One PIC occupies one XIO widget and exposes two independent PCI heads. */
MemoryRegion *sgi_pic_widget(SGIPICState *s);
PCIBus *sgi_pic_pci_bus(SGIPICState *s, unsigned head);
/* Board code wires each head's pci-intx outputs to its eight pin inputs. */
DeviceState *sgi_pic_head_device(SGIPICState *s, unsigned head);
void sgi_pic_set_bedrock_pi(SGIPICState *s, unsigned pi,
                            SGIBedrockPIState *bedrock_pi,
                            unsigned source_base);

/* Select the reviewed IP53 functional composition before node construction. */
bool sgi_pic_enable_pxbow_experiment(SGIPICState *s, uint64_t span,
                                     Error **errp);
bool sgi_pic_attach_pxbow_packet_endpoint(SGIPICState *s,
                                          MemoryRegion *packet_face,
                                          Error **errp);
MemoryRegion *sgi_pic_pxbow_widget_zero(SGIPICState *s);
SGIXbowState *sgi_pic_pxbow(SGIPICState *s);
MemoryRegion *sgi_pic_routed_widget(SGIPICState *s, unsigned int widget,
                                    Error **errp);
uint8_t sgi_pic_pxbow_revision(SGIPICState *s);

/*
 * Build a middle PIC under pic-rev3-enclosure-experimental as an IX-brick
 * or PX-brick crossbar, before realization: its integrated PXBOW takes
 * the host on port 10, the outer PICs on 12 and 13 and its own heads on
 * 15. The node's view of each widget number through the host port is
 * sgi_pic_enclosure_widget_view(); widget 0 is the crossbar's registers.
 */
bool sgi_pic_enable_enclosure_crossbar(SGIPICState *s, uint64_t span,
                                       Error **errp);
MemoryRegion *sgi_pic_enclosure_widget_view(SGIPICState *s,
                                            unsigned int widget,
                                            Error **errp);

#endif
