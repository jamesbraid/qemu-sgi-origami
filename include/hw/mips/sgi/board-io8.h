/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-io8's own public type and widget-view accessor. Every other
 * sgi-xtalk-board class names its QOM type entirely inside its own file
 * (hw/mips/sgi/sn-machine.c's own header comment on the four onboard/
 * plugged board classes): a bare `cpu_face`/`packet_face` pair, the base
 * class's own contract, is all any caller outside the class has ever
 * needed. Origin 300's Bedrock is the exception: it has no outer Crossbow
 * of its own, so its crossbar decode routes every widget position it
 * knows about (its own local view at widget zero, plus one per XBridge
 * head) straight into this one fused chip, and needs a distinct view per
 * widget the way sgi_xbridge_widget_view() already hands out -- more than
 * the one-view-per-board contract carries. This header exists only to
 * expose that lookup to sn1-node.c, which does the wiring.
 */
#ifndef HW_MIPS_SGI_BOARD_IO8_H
#define HW_MIPS_SGI_BOARD_IO8_H

#include "hw/mips/sgi/xtalk-board.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SGI_IO8 "sgi-io8"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIo8State, SGI_IO8)

/*
 * One of the fused chip's three machine-visible widget views: widget
 * zero (the crossbar section's own local view), and 0xe/0xf (its two PCI
 * heads). Matches sgi_xbridge_widget_view()'s own contract, which this
 * forwards to.
 */
MemoryRegion *sgi_io8_widget_view(SGIIo8State *s, unsigned int widget);

#endif
