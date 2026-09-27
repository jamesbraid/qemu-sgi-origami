/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The two SN0 crosstalk link kinds: a Hub cabled straight to one widget
 * (Origin 200), and a Hub or a widget joined to a Crossbow port (Origin
 * 2000). Each owns exactly the behaviour its kind needs, reading its
 * endpoints through the crosstalk port interface (sgi-xio-port.h) both link
 * kinds share.
 *
 * Storage is caller-owned, matching this tree's convention for machine-built
 * glue (compare the SN0Origin2000HubIO/HubOutgoingIO structs this replaces):
 * the machine allocates an array of these and hands each element in by
 * pointer. Every MemoryRegion a link builds is parented to the owner Object
 * the caller supplies, exactly as the code this replaces parented its regions
 * to the machine.
 */

#ifndef HW_MIPS_SGI_XIO_LINK_H
#define HW_MIPS_SGI_XIO_LINK_H

#include "qom/object.h"
#include "system/memory.h"
#include "hw/misc/sgi-xbow.h"

/*
 * SN0NodeState is a private type of hw/mips/sgi/ (sn0.h, not under include/),
 * which this public header cannot reach into. Only a link function's .c
 * file, in that same directory, may. Forward declared here for the
 * pointer-only uses below.
 */
typedef struct SN0NodeState SN0NodeState;

/*
 * The direct link: a Hub's I/O port cabled straight to one crosstalk widget.
 * Installs the widget's cpu_face as an alias at every crosstalk position the
 * Hub decodes to it (the Bridge answers its whole face at every non-zero
 * widget a directly linked Hub sends out its link) and wires the PROM's
 * fixed widget-2 PI-error probe through whichever of the widget's device
 * windows it falls in. Origin 2000 does not use the probe; connect() wires it
 * only when probe_widget is nonzero.
 */

/* Widget positions a crosstalk address space names (sn0.h's count). */
#define SGI_XIO_DIRECT_LINK_MAX_ALIASES 16

typedef struct SGIXioDirectLink {
    MemoryRegion alias[SGI_XIO_DIRECT_LINK_MAX_ALIASES];
    unsigned int alias_count;
} SGIXioDirectLink;

/*
 * hub_port and widget_port must already answer cpu_face(): hub_port once
 * itsing node has realized, widget_port once its build_widget() has run.
 * discovery_widget and widget are the two crosstalk positions the widget is
 * separately reachable at (a repeat is fine: the second alias is simply not
 * installed); decoded_widget_mask names every further widget the Hub decodes
 * to it, in SN0_XIO_WIDGETS bits. probe_widget nonzero requests the PROM
 * PI-error probe, dispatched through node's
 * prom_pi_probe/prom_pi_probe_window fields at that widget's crosstalk base
 * plus probe_offset, resolved against
 * widget_port's "bridge" child.
 */
bool sgi_xio_direct_link_connect(SGIXioDirectLink *link, Object *owner,
                                 SN0NodeState *node, Object *widget_port,
                                 unsigned int discovery_widget,
                                 unsigned int widget,
                                 uint32_t decoded_widget_mask,
                                 unsigned int probe_widget,
                                 uint64_t probe_offset, Error **errp);

/*
 * The crossbow link: a Hub's I/O port or a widget joined to one Crossbow
 * port. Carries packets between the two ports in both directions, as the
 * SN0Origin2000HubIO/HubOutgoingIO adapters and the machine's per-node
 * proxy/alias arrays did by hand.
 *
 * sgi_xbow_attach_endpoint() requires its Crossbow still unrealized, so
 * attachment happens in two calls bracketing the Crossbow's realize:
 * prepare_*() and attach_*() run first (attach_*() needs the outer port's
 * faces, so a widget's board must already have realized by then; a Hub's node
 * likewise), then finish() runs once the Crossbow has realized, installing
 * its now-real per-widget views as the outer port's outbound target.
 *
 * A Hub's outer face cannot be its plain cpu_face()/packet_face(): what the
 * Crossbow reads there is the Hub's whole crosstalk container and its engine
 * claim view, neither of which is what a Crossbow packet addressed to this
 * widget should reach. attach_hub() builds the narrower faces those packets
 * need instead, in node's address space and register state.
 */
typedef struct SGIXioCrossbowLink {
    /*
     * The outer endpoint's outbound proxy (a Hub's "xio-io-space" or a widget's
     * "fabric" property target) is created and installed before that endpoint
     * realizes, aliased onto the Crossbow's real packet ingress once finish()
     * runs.
     */
    MemoryRegion proxy;
    MemoryRegion proxy_alias;

    /*
     * Set once a `-device` board actually takes this link's proxy as its
     * "fabric" property (sgi_xio_crossbow_link_claim_fabric()). finish()
     * aliases the real ingress onto the proxy either way, but that real
     * ingress answers only once its widget is alive; a spare widget nobody
     * claimed stays out of that count on purpose (see sgi_machine_done()), so
     * this flag is what tells the caller whether to mark it alive.
     */
    bool fabric_claimed;

    /*
     * The faces the claiming `-device` board hands claim_fabric() once its
     * widget has built: what sgi_machine_done() registers as the spare
     * widget's inbound destination (sgi_xbow_attach_endpoint_live()), so a
     * processor or peer widget can address the board, not only the board
     * send outbound. NULL until a board claims the proxy.
     */
    MemoryRegion *claimant_cpu_face;
    MemoryRegion *claimant_packet_face;

    bool is_hub;
    SN0NodeState *node;

    /*
     * The Hub-only faces prepare_hub()/attach_hub() build; unused for a
     * widget link. outbound_face is what "xio-io-space" is actually set to: the
     * Hub's big-window mode issues a widget-addressed access (a widget number
     * and an offset within SN0_XIO_WIDGET_SIZE, the same shape a small/big
     * window decodes), which outbound_face translates into a packet-addressed
     * one on proxy before it ever reaches the Crossbow, matching the packet
     * header a switched fabric expects.
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
    uint64_t inbound_io_base;
    uint64_t inbound_node_stride;
    uint64_t inbound_modify_offset;
} SGIXioCrossbowLink;

/*
 * Create the proxy and return it, for the caller to install as the outer
 * endpoint's outbound property before that endpoint realizes.
 */
MemoryRegion *sgi_xio_crossbow_link_prepare(SGIXioCrossbowLink *link,
                                            Object *owner, const char *name);

/*
 * Marks a spare proxy prepare() built as actually taken by a `-device`
 * board's own "fabric" property, so finish() knows to bring the underlying
 * widget up, and records the board's faces so sgi_machine_done() can register
 * the widget as an inbound destination too. fabric is whatever a board's
 * "fabric" link property resolves to, which a real machine always sets to a
 * proxy prepare() built but a qtest fixture standing in for one need not;
 * this quietly does nothing when fabric is not a proxy prepare() returned,
 * rather than assume every caller's fabric is one of ours. Call it once the
 * board's build_widget() has recorded its cpu_face and packet_face, so both
 * are ready to hand over.
 */
void sgi_xio_crossbow_link_claim_fabric(MemoryRegion *fabric,
                                        MemoryRegion *cpu_face,
                                        MemoryRegion *packet_face);

/* Also wires node's "xio-io-space" property to the proxy prepare() built. */
bool sgi_xio_crossbow_link_prepare_hub(SGIXioCrossbowLink *link,
                                         Object *owner, const char *name,
                                         SN0NodeState *node, Error **errp);

bool sgi_xio_crossbow_link_attach_hub(SGIXioCrossbowLink *link,
                                        Object *owner, const char *name,
                                        SGIXbowState *xbow, unsigned int widget,
                                        Error **errp);
bool sgi_xio_crossbow_link_attach_widget(SGIXioCrossbowLink *link,
                                           Object *widget_port,
                                           SGIXbowState *xbow,
                                           unsigned int widget, Error **errp);

bool sgi_xio_crossbow_link_finish(SGIXioCrossbowLink *link, Object *owner,
                                    const char *name, SGIXbowState *xbow,
                                    unsigned int widget, Error **errp);

/*
 * The XBridge link: an SN1 node's Bedrock joined to its XBridge's port 0xa,
 * the host port. Not the Crossbow link above under a new name: that link's
 * shape is the SN0 Hub's packet header and register-offset layout end to end
 * (SN0NodeState, sn0_hub_address_map, the Hub's widget-relative "xio-io-space"
 * property), none of which exists on SN1. The XBridge's crossbar section
 * routes by destination widget exactly as a Crossbow does. This is not a
 * hardware difference. The two chips this link joins attach through their
 * typed property surfaces (the Bedrock's "widget-N" links, the XBridge's
 * "fabric" link), not through an xbow's ingress/packet_ingress address-space
 * routing, so there is no state for a link struct to own and nothing to share
 * with the Crossbow link's code.
 *
 * Unlike the Crossbow link, an XBridge's per-widget views exist from instance
 * init (hw/pci-host/sgi-xbridge.c's chip_init()), so connecting needs no
 * deferred finish() once the chip realizes: this installs the Bedrock's
 * packet-receiving face as the chip's "fabric" property (the path through
 * which its widget-0 conditions and both heads' interrupts reach the host)
 * before the chip realizes. Reading back each chosen widget's own
 * cpu_face() and handing them to the Bedrock's "widget-N" properties is the
 * caller's job, exactly as a widget port's typed acceptance already works
 * elsewhere in this file; that direction needs no link-owned state either.
 */
bool sgi_xio_xbridge_link_connect(Object *bedrock_port, DeviceState *xbridge,
                                  Error **errp);

#endif /* HW_MIPS_SGI_XIO_LINK_H */
