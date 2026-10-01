/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA: the InfiniteReality graphics pipe.
 *
 * This sgi-xio-board subclass models widget probes, one-wire identity
 * records, an additional register window, and a textport command port.
 * Values beyond the widget-header and classification fields are synthetic
 * or approximated. Identity serials and several board-record fields are
 * placeholders. The register and one-wire behavior has not been fully
 * characterized against physical hardware.
 */

#ifndef HW_MISC_SGI_KONA_H
#define HW_MISC_SGI_KONA_H

#include "hw/mips/sgi/xio-board.h"
#include "hw/1wire/onewire.h"
#include "hw/misc/sgi-command-port.h"
#include "qemu/units.h"
#include "qom/object.h"

#define TYPE_SGI_KONA "sgi-kona"
OBJECT_DECLARE_SIMPLE_TYPE(SGIKonaState, SGI_KONA)

/* One widget's worth of crosstalk address space; the rest reads unmapped. */
#define SGI_KONA_APERTURE_SIZE (16 * MiB)

/* Widget-header identification, the generic crosstalk layout. */
#define SGI_KONA_WIDGET_ID 0x000004

/* A plain, generic crosstalk register; this model interprets none of it. */
#define SGI_KONA_WIDGET_CONTROL 0x000024

/*
 * The 3-bit subsystem-classification register. This model returns
 * probe_value; the default value selects its configured part-based path.
 */
#define SGI_KONA_PROBE 0x000064

/*
 * The one-wire bit-slot register. Its fields use the encoding shared by the
 * Bridge NIC and Hub one-wire models. A 32-bit write starts a bit slot; this
 * model reports completion synchronously. SGI_KONA_NIC is a second offset
 * that the model aliases to the same state; its physical relationship to
 * this register remains unresolved.
 */
#define SGI_KONA_ONEWIRE 0x000098

#define SGI_KONA_ONEWIRE_LEVEL        (1u << 0)
#define SGI_KONA_ONEWIRE_DONE         (1u << 1)
#define SGI_KONA_ONEWIRE_SAMPLE_SHIFT 2
#define SGI_KONA_ONEWIRE_SAMPLE_MASK  (0xffu << 2)
#define SGI_KONA_ONEWIRE_PULSE_SHIFT  10
#define SGI_KONA_ONEWIRE_PULSE_MASK   (0x3ffu << 10)
#define SGI_KONA_ONEWIRE_WRITABLE \
    (SGI_KONA_ONEWIRE_SAMPLE_MASK | SGI_KONA_ONEWIRE_PULSE_MASK)

/*
 * This model aliases the second register offset to the one-wire chain above.
 * Whether physical hardware exposes one chain at both offsets remains
 * unresolved. The model has no separate state for this offset.
 */
#define SGI_KONA_NIC 0x00009c

/*
 * An additional register window and textport command port. Behavior
 * below describes this model unless a limitation is stated explicitly.
 */

/*
 * The IO6 firmware's one-time low-window write, distinct from the kernel
 * FIFO port hw/misc/sgi-kona-ports.h describes at this same offset --
 * see that header's own note on the ambiguity. Accepted and stored only;
 * the observed value (0x23) has no established meaning.
 */
#define SGI_KONA_LOW_WINDOW_WRITE 0x00007c

/*
 * The credit/occupancy register the PROM polls before every textport
 * push, wired to the textport command port's own credit hook (left
 * unset, so it answers the fixed zero this offset's approximation
 * needs: satisfies both the driver's signed-<=0 read and the firmware's
 * unsigned-==0 read). No write case is established.
 */
#define SGI_KONA_CREDIT 0x800208

/*
 * Plain stored field, written 0 as the first step of the firmware's
 * recovery sequence on a failed sync probe. This model interprets
 * neither the write nor its readback.
 */
#define SGI_KONA_RESET_ARG 0x800220

/*
 * The mailbox interrupt-cause register. Bit MAILBOX is established:
 * write-1-to-clear, and a clear that leaves a further reply word queued
 * re-arms it synchronously -- this model has no separate device-side
 * timing to simulate the real interrupt's latency.
 */
#define SGI_KONA_INT_CAUSE 0x800238
#define SGI_KONA_INT_CAUSE_MAILBOX (1u << 4)

/*
 * The mailbox reply-word register: readable while a queued reply
 * remains (zero once exhausted), any write -- evidence always shows
 * zero -- advances to the next queued word without touching
 * SGI_KONA_INT_CAUSE.
 */
#define SGI_KONA_MAILBOX 0x800240
#define SGI_KONA_MAILBOX_MAX_WORDS 5

/*
 * Plain stored field, reused by both the firmware's recovery sequence
 * (values 3, 2) and its EEPROM-check sequence (values 1, 0). This model
 * interprets neither.
 */
#define SGI_KONA_RESET_STROBE 0x800300

/*
 * Plain stored field: written 1 before the firmware's fallback EEPROM
 * scan. This model interprets neither the write nor its readback.
 */
#define SGI_KONA_EEPROM_ENABLE 0x800308

/*
 * The fallback EEPROM's address and data ports: the address port
 * stores the addressed offset, the data port answers the word at that
 * offset. Only address SGI_KONA_EEPROM_CHECKED_ADDR is established
 * (value SGI_KONA_EEPROM_CHECKED_VALUE); every other address in the
 * firmware's 12-word scan reads zero.
 */
#define SGI_KONA_EEPROM_ADDR 0x8003e8
#define SGI_KONA_EEPROM_DATA 0x8003f0
#define SGI_KONA_EEPROM_CHECKED_ADDR  0x87ff1c
#define SGI_KONA_EEPROM_CHECKED_VALUE 2

/* The PROM textport's own command port. */
#define SGI_KONA_TEXTPORT_PORT 0xc10000

/*
 * The textport's own sync-word-stripped bulk-transfer framing for
 * command 0xa (a six-halfword header, then a packed 1-bpp bitmap
 * payload): hw/misc/sgi-kona-ports.h excludes it from the selector
 * table for exactly this reason, so the owning device frames it in
 * front of the generic command-port framer instead.
 */
#define SGI_KONA_TEXTPORT_SYNC_WORD 1
#define SGI_KONA_BITMAP_SELECTOR 0xa
#define SGI_KONA_BITMAP_HEADER_WORDS 6

typedef enum SGIKonaBitmapState {
    SGI_KONA_BITMAP_IDLE,
    SGI_KONA_BITMAP_HEADER,
    SGI_KONA_BITMAP_PAYLOAD,
} SGIKonaBitmapState;

/* The native framebuffer this increment publishes as a display surface. */
#define SGI_KONA_FB_WIDTH  1280
#define SGI_KONA_FB_HEIGHT 1024

struct SGIKonaState {
    SGIXioBoardState parent_obj;

    /*
     * Set by build_widget(), before this board re-parents: the XIO
     * fabric proxy sgi_xio_crossbow_link_claim_fabric() claims. Required;
     * a board realized without it refuses.
     */
    MemoryRegion *fabric;

    MemoryRegion regs;

    uint32_t manufacturer;
    uint32_t part;
    uint32_t revision;

    /* Plain stored fields; this model interprets neither. */
    uint32_t widget_control;
    uint32_t probe_value;

    uint32_t onewire_reg;

    /* Increment 2: register-window plain storage and the mailbox queue. */
    uint32_t reset_arg;
    uint32_t int_cause;
    uint32_t mailbox_queue[SGI_KONA_MAILBOX_MAX_WORDS];
    uint32_t mailbox_len;
    uint32_t mailbox_pos;
    uint32_t reset_strobe;
    uint32_t eeprom_enable;
    uint32_t eeprom_addr;
    uint32_t low_window_write;

    /* Increment 2: the textport command port and its parser state. */
    SGICommandPort textport;
    bool textport_expect_sync;
    uint32_t draw_color;
    /*
     * SGIKonaBitmapState's values, but stored as a plain uint32_t so
     * VMSTATE_UINT32() can migrate it directly -- matching
     * hw/misc/sgi-mgras.c's own cmd_token_class precedent, which stores
     * its parser state the same way for the same reason.
     */
    uint32_t bitmap_state;
    uint32_t bitmap_header[SGI_KONA_BITMAP_HEADER_WORDS];
    uint32_t bitmap_header_have;
    uint32_t bitmap_width;
    uint32_t bitmap_height;
    uint32_t *bitmap_payload;
    uint32_t bitmap_payload_have;
    uint32_t bitmap_payload_expected;

    /*
     * Host scanout: native storage, converted only on damage. Unlike
     * hw/misc/sgi-mgras.c's own con/display_buffer/dirty precedent,
     * KONA's display surface is created directly over this array (see
     * sgi_kona_display_ensure_surface()), so framebuffer is itself the
     * canonical guest-visible pixel state, not a derived buffer -- it is
     * migrated (fixed-length, like mgras's own front/back planes), and
     * framebuffer_count exists solely to carry that fixed length through
     * VMSTATE_VARRAY_UINT32(). con is host-only and never migrated:
     * finish_realize() recreates it, and post_load rebuilds its surface.
     */
    QemuConsole *con;
    uint32_t *framebuffer;
    uint32_t framebuffer_count;
    bool framebuffer_dirty;

    /*
     * Host-only qtest introspection coordinates for x-test-pixel. Not
     * guest state and not migrated, matching hw/misc/sgi-mgras.c's own
     * test_pixel_x/test_pixel_y precedent.
     */
    uint32_t test_pixel_x;
    uint32_t test_pixel_y;
};

#endif
