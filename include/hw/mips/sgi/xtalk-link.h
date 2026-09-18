/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SN0 crosstalk link kinds. Each owns exactly the behaviour its
 * kind needs; see docs/designs/sn-machine-model-architecture.md, "Ports
 * and links", and the crosstalk port interface (sgi-xtalk-port.h) each
 * link kind reads its endpoints through.
 *
 * Storage is caller-owned, matching this tree's own convention for
 * machine-built glue (compare the SN0Origin2000HubIO/HubOutgoingIO
 * structs a later commit here replaces): the machine allocates an array
 * of these and hands each element in by pointer. Every MemoryRegion a
 * link builds is parented to the owner Object the caller supplies,
 * exactly as the code it replaces parented its own regions to the
 * machine.
 */

#ifndef HW_MIPS_SGI_XTALK_LINK_H
#define HW_MIPS_SGI_XTALK_LINK_H

#include "qom/object.h"
#include "system/memory.h"

/*
 * SN0NodeState is a private type of hw/mips/sgi/ (sn0.h, not under
 * include/), which this public header cannot reach into -- only a link
 * function's own .c file, in that same directory, may. Forward declared
 * here for the pointer-only uses below.
 */
typedef struct SN0NodeState SN0NodeState;

/*
 * The direct link: a Hub's I/O port cabled straight to one crosstalk
 * widget. Installs the widget's own cpu_face as an alias at every
 * crosstalk position the Hub decodes to it (docs/datasheets/bridge.md:
 * the Bridge answers its whole face at every non-zero widget a directly
 * linked Hub sends out its link) and wires the PROM's fixed widget-2
 * PI-error probe through whichever of the widget's own device windows it
 * falls in. Origin 2000 does not use the probe; connect() wires it only
 * when probe_widget is nonzero.
 */

/* Widget positions a crosstalk address space names (sn0.h's own count). */
#define SGI_XTALK_DIRECT_LINK_MAX_ALIASES 16

typedef struct SGIXtalkDirectLink {
    MemoryRegion alias[SGI_XTALK_DIRECT_LINK_MAX_ALIASES];
    unsigned int alias_count;
} SGIXtalkDirectLink;

/*
 * hub_port and widget_port must already answer cpu_face(): hub_port
 * once its owning node has realized, widget_port once its own
 * build_widget() has run. discovery_widget and widget are the two
 * crosstalk positions the widget is separately reachable at (a repeat
 * is fine: the second alias is simply not installed); decoded_widget_mask
 * names every further widget the Hub decodes to it, in SN0_XTALK_WIDGETS
 * bits. probe_widget nonzero requests the PROM PI-error probe, dispatched
 * through node's own prom_pi_probe/prom_pi_probe_window fields at that
 * widget's crosstalk base plus probe_offset, resolved against widget_port's
 * own "bridge" child.
 */
bool sgi_xtalk_direct_link_connect(SGIXtalkDirectLink *link, Object *owner,
                                    SN0NodeState *node, Object *widget_port,
                                    unsigned int discovery_widget,
                                    unsigned int widget,
                                    uint32_t decoded_widget_mask,
                                    unsigned int probe_widget,
                                    uint64_t probe_offset, Error **errp);

#endif /* HW_MIPS_SGI_XTALK_LINK_H */
