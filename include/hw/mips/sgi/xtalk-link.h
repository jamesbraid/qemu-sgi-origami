/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The two SN0 crosstalk link kinds: a Hub cabled straight to one widget
 * (Origin 200), and a Hub or a widget joined to a Crossbow port (Origin
 * 2000). Each owns exactly the behaviour its kind needs; see
 * docs/designs/sn-machine-model-architecture.md, "Ports and links", and
 * the crosstalk port interface (sgi-xtalk-port.h) both link kinds read
 * their endpoints through.
 *
 * Storage is caller-owned, matching this tree's own convention for
 * machine-built glue (compare the SN0Origin2000HubIO/HubOutgoingIO
 * structs this replaces): the machine allocates an array of these and
 * hands each element in by pointer. Every MemoryRegion a link builds is
 * parented to the owner Object the caller supplies, exactly as the code
 * this replaces parented its own regions to the machine.
 */

#ifndef HW_MIPS_SGI_XTALK_LINK_H
#define HW_MIPS_SGI_XTALK_LINK_H

#include "qom/object.h"
#include "system/memory.h"
#include "hw/misc/sgi-xbow.h"

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

/*
 * The crossbow link: a Hub's I/O port or a widget joined to one
 * Crossbow port. Carries packets between the two ports in both
 * directions -- what the SN0Origin2000HubIO/HubOutgoingIO adapters and
 * the machine's own per-node proxy/alias arrays did by hand.
 *
 * sgi_xbow_attach_endpoint() requires its Crossbow still unrealized, so
 * attachment happens in two calls bracketing the Crossbow's own realize:
 * prepare_*() and attach_*() run first (attach_*() needs the outer
 * port's own faces, so a widget's board must already have realized by
 * then; a Hub's node likewise), then finish() runs once the Crossbow has
 * realized, installing its now-real per-widget views as the outer port's
 * own outbound target.
 *
 * A Hub's outer face cannot be its plain cpu_face()/packet_face(): what
 * the Crossbow reads there is the Hub's whole crosstalk container and
 * its engine claim view, neither of which is what a Crossbow packet
 * addressed to this widget should reach. attach_hub() builds the
 * narrower faces those packets need instead, in node's own address
 * space and register state.
 */
typedef struct SGIXtalkCrossbowLink {
    /*
     * The outer endpoint's own outbound proxy -- a Hub's "xtalk-io" or
     * a widget's "fabric" property target -- created and installed
     * before that endpoint realizes, aliased onto the Crossbow's real
     * packet ingress once finish() runs.
     */
    MemoryRegion proxy;
    MemoryRegion proxy_alias;

    bool is_hub;
    SN0NodeState *node;

    /*
     * The Hub-only faces prepare_hub()/attach_hub() build; unused for a
     * widget link. outbound_face is what "xtalk-io" is actually set to:
     * the Hub's big-window mode issues a widget-addressed access (a
     * widget number and an offset within SN0_XTALK_WIDGET_SIZE, the same
     * shape a small/big window decodes), which outbound_face translates
     * into a packet-addressed one on proxy before it ever reaches the
     * Crossbow, matching the packet header a switched fabric expects.
     */
    MemoryRegion outbound_face;
    AddressSpace outbound_packets;

    MemoryRegion cpu_refusal;
    MemoryRegion cpu_alias;
    MemoryRegion inbound_face;
    AddressSpace inbound_target;
    AddressSpace inbound_local_registers;
    MemoryRegion inbound_local_register_view;
    uint64_t inbound_memory_size;
    uint64_t inbound_xtalk_base;
    uint64_t inbound_node_stride;
    uint64_t inbound_modify_offset;
} SGIXtalkCrossbowLink;

/*
 * Create the proxy and return it, for the caller to install as the
 * outer endpoint's own outbound property before that endpoint realizes.
 */
MemoryRegion *sgi_xtalk_crossbow_link_prepare(SGIXtalkCrossbowLink *link,
                                              Object *owner, const char *name);

/* Also wires node's own "xtalk-io" property to the proxy prepare() built. */
bool sgi_xtalk_crossbow_link_prepare_hub(SGIXtalkCrossbowLink *link,
                                         Object *owner, const char *name,
                                         SN0NodeState *node, Error **errp);

bool sgi_xtalk_crossbow_link_attach_hub(SGIXtalkCrossbowLink *link,
                                        Object *owner, const char *name,
                                        SGIXbowState *xbow, unsigned int widget,
                                        Error **errp);
bool sgi_xtalk_crossbow_link_attach_widget(SGIXtalkCrossbowLink *link,
                                           Object *widget_port,
                                           SGIXbowState *xbow,
                                           unsigned int widget, Error **errp);

bool sgi_xtalk_crossbow_link_finish(SGIXtalkCrossbowLink *link, Object *owner,
                                    const char *name, SGIXbowState *xbow,
                                    unsigned int widget, Error **errp);

#endif /* HW_MIPS_SGI_XTALK_LINK_H */
