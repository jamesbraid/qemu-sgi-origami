/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_MGRAS_H
#define HW_MISC_SGI_MGRAS_H

#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"
#include "qemu/units.h"
#include "ui/console.h"

#define TYPE_SGI_MGRAS "sgi-mgras"
OBJECT_DECLARE_SIMPLE_TYPE(SGIMgrasState, SGI_MGRAS)

#define SGI_MGRAS_MMIO_SIZE (2 * MiB)
/* The selected direct-command FIFO depth (design's own approximation). */
#define SGI_MGRAS_FIFO_DEPTH 256
#define SGI_MGRAS_WIDTH 1280
#define SGI_MGRAS_HEIGHT 1024

/*
 * The widget's own crosstalk interrupt-destination pair, at the offset the
 * Crossbow datasheet gives for "any widget's" copy of that register
 * (docs/datasheets/xbow.md, offset 0x0030/0x0038 relative to the widget's
 * register base). SI Viz's exact register table was never captured, so this
 * reuses the one interrupt-destination encoding the project holds evidence
 * for rather than inventing a SI-specific one. One destination serves all
 * four IP27 interrupt roles, which this device recomputes and delivers as a
 * single level whenever any unmasked pending role is set. The target widget
 * field, vector and address are guest-programmed and carried in migration.
 */
#define SGI_MGRAS_INT_DEST_UPPER 0x000034
#define SGI_MGRAS_INT_DEST_LOWER 0x00003c

/*
 * The widget's own generic control register, at the offset every widget's
 * copy shares (docs/datasheets/xbow.md documents Crossbow's own copy of the
 * same offset, "Widget-zero status, control and interrupt destination").
 * SI Viz's own control bit assignments were never captured, so -- following
 * that datasheet's own instruction not to invent semantics for an
 * unevidenced widget-zero control field -- this stores whatever the guest
 * writes and returns it unchanged, rather than either inventing bit meanings
 * or treating a plain generic-header register as an SI-specific unsupported
 * access.
 */
#define SGI_MGRAS_WIDGET_CONTROL 0x000024

/*
 * Two more registers the kernel driver's own reset routine writes once it
 * observes the readiness field at 0x020100 settled, live-traced through a
 * boot that actually reaches this code path: 0x011008 (a second word next
 * to the existing startup-configuration register at 0x011000) and 0x072408
 * (in the display/cursor register block). Neither field's bit meaning was
 * captured -- only that this driver writes them once during reset and
 * expects the access to succeed -- so both store opaque state, the same
 * treatment already given to the generic widget control register above.
 */
#define SGI_MGRAS_RESET_ARG 0x011008
#define SGI_MGRAS_RESET_CONTROL 0x072408

struct SGIMgrasState {
    SysBusDevice parent_obj;
    MemoryRegion regs;
    MemoryRegion *fabric;
    uint64_t fabric_base;
    uint32_t fabric_widget_shift;
    AddressSpace fabric_as;
    uint32_t int_dest_upper;
    uint32_t int_dest_lower;
    uint32_t widget_control;
    uint32_t reset_arg;
    uint32_t reset_control;
    bool irq_asserted;
    /* Host-only qtest introspection; not guest state, not migrated. */
    uint32_t test_pixel_x;
    uint32_t test_pixel_y;
    uint64_t test_last_interrupt_addr;
    uint64_t test_last_interrupt_value;
    QEMUTimer *retrace_timer;
    /*
     * A QEMU display surface created directly over front[]'s own storage
     * (PIXMAN_x8r8g8b8, matching the low-24-bit colour word this device
     * already stores verbatim), so the host's screendump command reads
     * whatever front[] holds with no separate copy.
     */
    QemuConsole *con;
    uint32_t *front;
    uint32_t *back;
    uint32_t front_count;
    uint32_t back_count;
    uint8_t *wid;
    uint32_t hq[5120];
    uint32_t *ge;
    uint32_t ge_addr;
    uint32_t ge_words;
    uint32_t hq_words;
    uint32_t hq_hash;
    uint32_t ge_hash;
    uint32_t pending;
    uint32_t irq_mask;
    uint32_t error;
    uint32_t error_addr;
    uint32_t error_width;
    uint32_t context[63];
    uint32_t context_words;
    uint32_t entry;
    uint32_t protocol;
    uint64_t prefix;
    bool prefix_written;
    bool entry_selected;
    bool first_ack;
    bool second_ack;
    bool ready;
    bool display_enabled;
    bool cursor_enabled;
    uint8_t cursor[256];
    uint32_t cursor_control;
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t cmap_index;
    uint32_t cmap[256];
    uint16_t fifo_count;
    uint32_t command[0x200];
    uint8_t command_valid[0x200];
    uint64_t retraces;
    uint32_t xmap_mode;
    uint32_t vc3[64];
    uint32_t revision;
    uint32_t part;
    uint32_t manufacturer;
};

#endif
