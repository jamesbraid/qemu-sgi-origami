/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Crossbow, the XIO crossbar.
 *
 * Copyright (c) 2026 Origami contributors
 *
 * Nine internal ports: eight external link controllers carrying decoded
 * widget identities 8 through 15, and a ninth that terminates at logical
 * widget zero, where the chip's own registers answer. Only the eight have an
 * XIO view of their own here. Widget zero's outbound traffic is the
 * interrupt it raises, which carries a 48-bit destination address of the
 * target's own rather than an XIO offset, so it goes into the target's
 * space directly instead of through an addressed route.
 */

#ifndef HW_MISC_SGI_XBOW_H
#define HW_MISC_SGI_XBOW_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "system/memory.h"
#include "qom/object.h"

/*
 * Eight external links decode widgets 8-15 and have crosstalk views. The
 * ninth port, widget 0, answers Crossbow registers. Its interrupt carries a
 * target-local 48-bit address and writes directly to the target's space.
 */

#define TYPE_SGI_XBOW "sgi-xbow"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXbowState, SGI_XBOW)

#define SGI_XBOW_WIDGETS        16
#define SGI_XBOW_LINKS          8
#define SGI_XBOW_FIRST_WIDGET   8
#define SGI_XBOW_PACKET_ADDRESS_BITS 48
#define SGI_XBOW_PACKET_SIZE (1ULL << SGI_XBOW_PACKET_ADDRESS_BITS)

typedef struct SGIXbowLink {
    SGIXbowState *xbow;
    unsigned int link;
} SGIXbowLink;

/*
 * Two numbering schemes meet in this device, so each is kept to its own side.
 * The properties and the generated machine descriptor mask widget numbers,
 * where bit 8 is the widget at decoded identity 8. Everything inside masks
 * and indexes links, 0 through 7. The property accessors are the only place
 * the two are converted.
 */
struct SGIXbowState {
    SysBusDevice parent_obj;

    /*
     * One XIO view per port, because a packet's route depends on where
     * it entered as well as where it is addressed. A port's view is disabled
     * while its own link is down, and each destination alias inside it is
     * disabled while that destination's link is down. No port carries a route
     * back to itself: a link does not send to itself, so that widget slot
     * decodes like an empty one.
     */
    MemoryRegion ingress[SGI_XBOW_LINKS];
    MemoryRegion route[SGI_XBOW_LINKS][SGI_XBOW_LINKS];
    MemoryRegion packet_ingress[SGI_XBOW_LINKS];
    MemoryRegion packet_route[SGI_XBOW_LINKS][SGI_XBOW_LINKS];

    /*
     * The widget numbers between the register block and the first link, which
     * name no destination on any switch. They decode so that a packet
     * carrying one is reported on the port that received it.
     */
    MemoryRegion illegal[SGI_XBOW_LINKS];

    /* The register block, and the view of it each link port is given. */
    MemoryRegion regs;
    MemoryRegion regs_view[SGI_XBOW_LINKS];
    MemoryRegion packet_regs_view[SGI_XBOW_LINKS];

    /*
     * What a packet addressed to each widget reaches, from the machine. The
     * interrupt widget zero raises carries a 48-bit destination address of
     * the target's own rather than an XIO offset, so it is written into
     * the target's space instead of through the routes an XIO address
     * selects.
     */
    MemoryRegion *face[SGI_XBOW_LINKS];
    MemoryRegion *packet_face[SGI_XBOW_LINKS];
    AddressSpace *face_as[SGI_XBOW_LINKS];
    bool regions_initialized;
    bool quiesced;
    bool stop_on_link_reset;
    bool complete_posted_error_address_writes;

    /* How much XIO space one widget owns, which the machine defines. */
    uint64_t span;

    /* What widget 0 answers with: revision, part number, manufacturer. */
    uint32_t revision;
    uint32_t part;
    uint32_t manufacturer;

    /*
     * Widget masks, as the properties and the descriptor spell them. These
     * are realize-time configuration inputs only: realize copies them into
     * the link masks below, and a widget attached and brought up after
     * realize (sgi_xbow_attach_endpoint_live(), sgi_xbow_mark_widget_alive())
     * changes the link masks without being reflected here.
     */
    uint16_t links_present_prop;
    uint16_t startup_links_alive_prop;
    uint16_t link_reset_restore_alive_prop;

    /*
     * Link masks. Attachment is board wiring. The startup mask is the
     * emulator's configured health, reapplied whenever the machine resets.
     * The live mask is what software and the link registers change, and it is
     * what migration carries.
     */
    uint8_t link_present;
    uint8_t link_startup;
    uint8_t link_reset_restore_alive_mask;
    uint8_t link_alive;

    uint8_t link_initializing;
    uint8_t link_came_up;
    uint8_t link_eight_bit;
    uint8_t link_reset_asserted;
    uint8_t link_reset_restore_alive;
    bool interrupt_asserted;

    uint32_t status;
    uint32_t err_upper;
    uint32_t err_lower;
    uint32_t control;
    uint32_t packet_timeout;
    uint32_t int_upper;
    uint32_t int_lower;
    uint32_t err_cmd;
    uint32_t llp_control;
    uint32_t arb_reload;
    uint32_t perf_a;
    uint32_t perf_b;
    uint32_t nic;
    uint32_t link_control[SGI_XBOW_LINKS];
    uint32_t link_status[SGI_XBOW_LINKS];
    uint32_t link_arb_upper[SGI_XBOW_LINKS];
    uint32_t link_arb_lower[SGI_XBOW_LINKS];
    uint32_t link_aux_status[SGI_XBOW_LINKS];

    /*
     * What has happened on a link since its status was last acknowledged, in
     * the bit positions its status register uses. A request stands while one
     * of these is still enabled, so a condition that has been acknowledged
     * does not raise a second one, and a port that is simply up or simply
     * failed does not hold a request nothing can clear.
     */
    uint32_t link_event[SGI_XBOW_LINKS];

    qemu_irq link_reset[SGI_XBOW_LINKS];
    QEMUTimer *link_reset_timer[SGI_XBOW_LINKS];
    int64_t link_reset_remaining[SGI_XBOW_LINKS];
    SGIXbowLink link_context[SGI_XBOW_LINKS];
};

/*
 * The XIO view a widget attached at this number sends into. The machine
 * maps it where that widget's outbound packets appear: inside a Hub's
 * XIO container, or as an I/O bridge's fabric link. Each view is also
 * published as a sysbus region, in link order, for a test that maps the ports
 * itself.
 */
MemoryRegion *sgi_xbow_ingress(SGIXbowState *s, unsigned int widget);

/* Terminal host teardown. Hold the BQL and keep endpoints connected. */
void sgi_xbow_quiesce(SGIXbowState *s);

/* Full packet address, with destination widget in bits 51:48. */
MemoryRegion *sgi_xbow_packet_ingress(SGIXbowState *s, unsigned int widget);

/* Both initialized endpoint faces must be supplied before realization. */
bool sgi_xbow_attach_endpoint(SGIXbowState *s, unsigned int widget,
                              MemoryRegion *cpu_face, MemoryRegion *packet_face,
                              Error **errp);

/* Legacy endpoints use the same face in both views. Set before realize. */
bool sgi_xbow_attach_widget(SGIXbowState *s, unsigned int widget,
                            MemoryRegion *face, Error **errp);

/*
 * The widgets with an endpoint attached, as a widget mask in the same form as
 * "links-present" (bit N for widget N). A machine compares the two to keep
 * what it reports attached and what it actually attached from drifting apart;
 * the device itself allows a face on an absent port.
 */
uint16_t sgi_xbow_endpoint_widgets(const SGIXbowState *s);

/*
 * Marks a present-but-not-yet-alive widget's own link alive after realize,
 * the same operational transition "links-alive" drives from the register
 * side, without requiring an attached endpoint: a widget with no face can
 * still send, since sgi_xbow_routed() only reads face[] for a destination,
 * never for a source. With @restore_after_reset the widget also joins the
 * "link-reset-restore-alive" set, so a later link reset returns it to the
 * health it had before the reset, as it does for a link configured there.
 * Fails if the widget is already alive, not present, or the crossbar is not
 * yet realized or is quiesced.
 */
bool sgi_xbow_mark_widget_alive(SGIXbowState *s, unsigned int widget,
                                bool restore_after_reset, Error **errp);

/*
 * Attach a widget's endpoint faces after the Crossbow has realized and build
 * the routes into it, the post-realize counterpart of
 * sgi_xbow_attach_endpoint(): it makes the widget an inbound destination
 * other ports and processors can address, where attach_endpoint() refuses
 * once the Crossbow is realized. For a board that reaches its Crossbow only
 * after machine init (a `-device` expansion board plugged into the spare
 * fabric proxy), the Crossbow is already realized. The port
 * reports a widget present from here on, whatever "links-present" said at
 * realize. The caller brings the widget's own link alive with
 * sgi_xbow_mark_widget_alive(). Fails if the widget already has a face, is
 * out of range, or the crossbar is not realized or is quiesced.
 */
bool sgi_xbow_attach_endpoint_live(SGIXbowState *s, unsigned int widget,
                                   MemoryRegion *cpu_face,
                                   MemoryRegion *packet_face, Error **errp);

#endif /* HW_MISC_SGI_XBOW_H */
