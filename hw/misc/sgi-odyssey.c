/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI Odyssey (VPro) graphics board, as an XIO board carrying one crosstalk
 * widget. Crosstalk part 0xc013, manufacturer 0x023.
 *
 * The first increment models what a stock IP35 PROM and the IRIX IP35
 * graphics driver have been observed to need before they will claim the
 * board: the crosstalk configuration block, the board-identity part on its
 * one-wire access register, the memory-configuration register, the software
 * sync register and the interrupt flag, flag-clear and enable registers.
 *
 * The second increment adds the display back end's indexed register file --
 * the 64-bit indirect write port at 0x400000 and its read window at
 * 0x220000 -- for the two entries the IP35 driver's I2C engine touches: the
 * main and opt port command/status pairs
 * (docs/research/graphics/odyssey-i2c-engine-20260923.md). Every other
 * index in that file stays unimplemented. The I2C engines model no
 * downstream device: a non-zero command word always transfers and always
 * acknowledges, so ACK always reads acknowledged and DATA and the 13:10
 * engine-state field always read zero. That clears the driver's
 * attach-time polling loop without asserting what a real PLL, DDC EEPROM
 * or EDID ROM would answer.
 *
 * Not implemented: the rest of the read-back window at 0x200000 and the
 * rendering-pipeline stages (Buzz Graphics Engine, Buzz X-Form, Buzz
 * Raster, Pixel Blaster & Jammer, the gamma ramp) that OpenBSD's
 * odyssey_setup() initialises, except where a later increment below
 * models one. Unmodeled offsets read zero, discard writes and report
 * themselves once through LOG_UNIMP; every access is traced.
 *
 * The third increment adds a modeled framebuffer and a display scanout,
 * the foundation the fourth increment's command interpreter renders into.
 * The colour framebuffer is two RGBA8 planes, colour buffers 0 and 1
 * (ODSY_REG_DRAW_MASK), over the shared graphics foundation's board memory,
 * presented through the shared SGIScanout (hw/display/sgi/scanout.h): the
 * scanout owns the console, the presented frame, the dirty-line set and the
 * vertical-retrace timer, and resolves each window's displayed buffer a line
 * at a time through the head's resolve_row hook. The
 * framebuffer's size, pixel format and byte packing are approximations
 * documented on SGIOdysseyState's colour-plane field: the real board's
 * SDRAM framebuffer base, stride and channel packing were not located
 * (docs/research/graphics/odyssey-display-backend-survey-20260922.md).
 *
 * The fourth increment adds a command-FIFO interpreter (0x110000) for the
 * three operations a console driver needs: solid rectangle fill,
 * screen-to-screen copy (console scrolling) and monochrome glyph blit
 * (console text). The grammar is recovered from OpenBSD's public
 * odyssey(4) driver -- odyssey_fillrect(), odyssey_copyrect() and
 * odyssey_putchar() -- and agrees with the command stream the IRIX X
 * server sends
 * (docs/research/graphics/odyssey-openbsd-public-contract-20260922.md,
 * odyssey-xsgi-2d-command-grammar-20260922.md). The FIFO is
 * hardware-sequenced, not address-counted
 * (docs/datasheets/odyssey.md qtest checklist item 21): every 32-bit or
 * 64-bit store is the next word of a command stream that a small state
 * machine below parses one word at a time. Every recognized word, opcode
 * and register is cited at its handler; an unrecognized command word is
 * reported once through LOG_UNIMP and treated as taking no operands, the
 * same do-not-invent-an-arity stance odyssey_reg() already takes for
 * unestablished register widths. This increment's approximations -- the
 * glyph raster-position template's exact word grouping, the per-pixel
 * color word's bit layout, and which 32-bit half of an 8-byte FIFO store
 * is logically first -- are documented at their handlers below.
 *
 * The fifth increment turns those interrupt registers into an actual
 * delivery. XIO interrupts on this platform are crosstalk packets,
 * not a sysbus line: each edge of (intr_flags & intr_enable) sends an
 * interrupt-modify write to the destination int_dest_upper/int_dest_lower
 * program, over a machine-supplied fabric link to the host's inbound
 * window. The destination decode -- widget in bits 19:16, vector in bits
 * 31:24 of int_dest_upper -- reuses hw/misc/sgi-mgras.c's own reviewed
 * widget-error delivery rather than re-deriving it independently.
 *
 * The sixth increment turns the 0x400000 write port into the counted
 * word-stream parser it really is (docs/analysis/odyssey-colormap-format.md)
 * and gives the back end its colour lookup RAM. The X server loads the
 * PseudoColor palette here as counted records, so an index-mode fill now
 * resolves its colour through the LUT instead of the earlier provisional
 * grey. The two I2C command indices are the count == 1 case of the same
 * record and keep their existing dispatch; the display-mode registers at
 * 0x2900 + DID are captured so a drawable's colormap group can be resolved.
 *
 * The seventh increment renders the greeter's text. The X server draws every
 * label as a one-bit pattern quad, not the per-row pixel runs the fourth
 * increment approximated: it loads a 32x32-bit stipple through the 0x3c4e and
 * 0x3c4f register bursts, selects a shape with 0x043f, turns pattern mode on
 * with 0x0404, then draws the string's bounding box as a QUADS fill that
 * paints the current colour only where a pattern bit is set
 * (docs/analysis/odyssey-glyph-raster-format.md). That replaces the earlier
 * glyph-setup/row-marker/pixel-run handling, which modeled the wrong path and
 * produced stray fragments.
 *
 * The eighth increment paints the separate BEGIN-0xa image raster path: the
 * greeter's account icons (the pixmaps above root/EZsetup/demos/guest) and the
 * text-field caret. An image opens with BEGIN primitive 0xa, takes its
 * destination box from the 0xcf80a92f floats, and fills it in raster order
 * from the pixel bursts -- fmt 0x11 as direct 0x00RRGGBB colour (the icons),
 * fmt 0x10 as four colormap indices per word (docs/analysis/odyssey-glyph-
 * raster-format.md). One approximation stands: the caret's index-0 erase
 * (a 6x20 field of colour index 0) would stamp opaque black over the mauve
 * input field, since the write mode that makes the erase-then-redraw sequence
 * correct is not established. Index-0 pixels of a colour-index image are left
 * unpainted rather than drawn black, which preserves the field; it is flagged
 * at the painter below.
 *
 * The ninth increment adds the page-table pixel/texture DMA engine on the
 * shared graphics foundation's SGIPixelDMA (hw/display/sgi/pixel-dma.h). The
 * kernel driver pins an arbitrary (including user) buffer, builds a 16 KiB-page
 * scatter-gather translation table over it, and starts the board fetching it by
 * pushing a single descriptor -- two 64-bit doublewords carrying an opcode plus
 * the table's physical base -- through the command FIFO at 0x110000
 * (docs/analysis/odyssey-dma-submit-swap-path.md). Only large pixel/image and
 * texture blits take this path; geometry is FIFO-pushed word by word and does
 * not, so the login greeter (which paints through the PIO command interpreter
 * above) never exercises this engine. The engine walks the table's 8-byte
 * entries and paints the header's rectangle into the buffer its word 0 names,
 * the screen or an off-screen board-memory object (ODSY_OBJ_POOL_BYTES), or
 * for a board-to-host header writes that rectangle into the entries
 * (ODSY_DMA_TO_HOST), then
 * raises the same completion fence the
 * standalone 0x0a5a host-write command already used -- the halfword to the guest
 * fence slot plus the softsync crosstalk interrupt -- from the head's publish()
 * hook, so there is one fence implementation. The descriptor's exact field
 * layout, the translation-table entry's bit layout and the value the board
 * writes into the fence slot are all unestablished in any allowed source
 * (docs/analysis/odyssey-dma-submit-swap-path.md, "What a hardware capture would
 * still need to settle"); this increment models a documented approximation of
 * each, flagged at the constants and the engine below, and settles nothing about
 * where in guest memory the board's own pixel pool lives (the objects are a
 * model-owned store, the same device-backed choice and for the same reason
 * as the colour plane's).
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/atomic.h"
#include "qemu/log.h"
#include "qemu/rcu.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "hw/1wire/ds2505.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/sgi/board-memory.h"
#include "hw/display/sgi/gl-context.h"
#include "hw/display/sgi/head-ops.h"
#include "hw/display/sgi/pixel-dma.h"
#include "hw/display/sgi/rasterizer.h"
#include "hw/display/sgi/scanout.h"
#include "hw/misc/sgi-odyssey.h"
#include "hw/mips/sgi/pi-gfx.h"
#include "hw/mips/sgi/xio-widget.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "migration/vmstate.h"
#include "qemu/host-utils.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "system/address-spaces.h"
#include "system/memory_cached.h"
#include "ui/console.h"
#include "trace.h"

/*
 * The whole aperture a driver maps. IRIX maps 0x401000 bytes and OpenBSD
 * 0x410000; the difference is unreconciled, so the region covers the
 * larger one.
 */
#define ODSY_APERTURE 0x410000

#define ODSY_PART 0xc013
#define ODSY_MANUFACTURER 0x023

/*
 * The one modeled scanout mode's retrace period, from the compiled 60 Hz
 * 1280x1024 mode-description file (docs/datasheets/odyssey.md, display back
 * end): 1688 total pixels per line x 1066 total lines per frame, blanking
 * included, counted at a 107,964,476 Hz pixel clock -- 59.99998 Hz. There is
 * no VFO or timing register to read this from; on the real board retrace is
 * interrupt-driven by the kernel driver's vertical-retrace handler with
 * software vblank counters, so the period is a modeled parameter, not
 * guest state. SGIScanout counts it on QEMU_CLOCK_VIRTUAL to arm the
 * retrace() hook.
 */
#define ODSY_FRAME_CLOCKS (1688u * 1066u)
#define ODSY_PIXEL_CLOCK_HZ 107964476u

/*
 * The crosstalk configuration block: eight 64-bit registers on an 8-byte
 * stride, whose low halves are the 0x04, 0x0c, ... spellings every SGI
 * driver and this tree's other widgets use.
 */
#define ODSY_BLOCK_END 0x60
#define ODSY_IDENTITY 0x00
#define ODSY_STATUS 0x08
#define ODSY_ERROR_ADDR_UPPER 0x10
#define ODSY_ERROR_ADDR_LOWER 0x18
#define ODSY_CONTROL 0x20
#define ODSY_XT_REQ_TIMEOUT 0x28
#define ODSY_INT_DEST_UPPER 0x30
#define ODSY_INT_DEST_LOWER 0x38
#define ODSY_ERROR_COMMAND 0x40

/*
 * int_dest_upper/int_dest_lower's own decode: widget in bits 19:16, the
 * 48-bit destination address split 15:0 of int_dest_upper (high) and all
 * of int_dest_lower (low), and the delivered vector in bits 31:24 of
 * int_dest_upper. Data bit 8 of the interrupt-modify write itself selects
 * assertion versus deassertion. Carried over from hw/misc/sgi-mgras.c's
 * own reviewed decode of the same destination-register shape, not
 * independently re-derived from this board's own datasheet.
 */
#define ODSY_INT_DEST_WIDGET_SHIFT 16
#define ODSY_INT_DEST_WIDGET_BITS 4
#define ODSY_INT_DEST_VECTOR_SHIFT 24

/*
 * The driver's six-write memory-reset sequence (docs/datasheets/odyssey.md,
 * "0x1004"), the first before any identity read: established as driver
 * behaviour from the shipped kernel, its effect on hardware is unknown. The
 * model accepts and logs each write rather than modeling a reset.
 */
#define ODSY_RESET_CONTROL 0x1004

/*
 * The flag interrupt's registers, as the shipped IRIX 6.5.30 kernel uses them
 * (docs/reviews/odyssey-datasheet-fold-8eaf559842.md, claim 9):
 *
 *   ODSY_INTR_FLAGS (0x1014): the pending flags. The kernel driver's
 *     flag-interrupt handler reads them, reads ODSY_INTR_ENABLE_SET, and
 *     dispatches on the and. The driver also writes here -- the kernel
 *     driver's FIFO high-water handler stores 0x200 when the FIFO is
 *     already below low water, and the kernel driver's diagnostic DMA path
 *     clears bit 11 by read-modify-write -- but whether a write sets,
 *     replaces or clears flags is unknown. Such a write is accepted and
 *     reported, and changes nothing.
 *   ODSY_INTR_CLEAR (0x101c): write one to clear flags. The kernel driver's
 *     flag-interrupt handler writes the mask it handled; the kernel driver's
 *     DMA submission writes 0x800 before a transfer. Established as the
 *     accesses, inferred as the function.
 *   ODSY_INTR_ENABLE_SET (0x1034) / ODSY_INTR_ENABLE_CLEAR (0x103c): the set
 *     and clear halves of one enable mask. The driver's copy of the mask
 *     gains a bit as it writes that bit to 0x1034 (the kernel driver's FIFO
 *     high-water handler and interrupt setup) and loses it as it writes it
 *     to 0x103c (the kernel driver's FIFO low-water handler and interrupt
 *     shutdown). Inferred from that pairing; a read of 0x1034 returns the
 *     whole mask, since the flag handler ands it with the flags.
 */
#define ODSY_INTR_FLAGS 0x1014
#define ODSY_INTR_CLEAR 0x101c
#define ODSY_INTR_ENABLE_SET 0x1034
#define ODSY_INTR_ENABLE_CLEAR 0x103c
/*
 * The routed-interrupt destination and per-source vector table: a 48-bit
 * destination shared with a widget field (bits 28:25 of ROUTE_DEST_UPPER)
 * distinct from int_dest_upper/int_dest_lower's own widget field above, and
 * four packed 8-bit vectors in ROUTE_VECTORS, one per routed source. Write
 * only -- the driver never reads any of the three back (docs/datasheets/
 * odyssey.md, "interrupt routing"). The softsync, retrace and DMA-done
 * sources raise through this table (ODSY_ROUTE_VECTOR_*_SHIFT below).
 */
#define ODSY_ROUTE_DEST_UPPER 0x1044
#define ODSY_ROUTE_DEST_LOWER 0x104c
#define ODSY_ROUTE_VECTORS 0x1054
#define ODSY_STATUS0 0x1064
#define ODSY_MEMORY_CONFIG 0x106c
#define ODSY_FLCTL_CONFIG 0x1070
#define ODSY_SW_SYNC 0x107c
#define ODSY_NIC_PROM 0x10d8
#define ODSY_NIC_KERNEL 0x10dc

/*
 * The DMA-sync command's (ODSY_REG_HOST_WRITE, below) host-write base: a
 * 48-bit guest physical address split the same way as int_dest_upper/lower,
 * bits 15:0 of the upper half plus all of the lower half. Write only, in
 * the host-address block docs/datasheets/odyssey.md leaves otherwise
 * unmodeled (offsets 0x4000-0x405f).
 */
#define ODSY_HOST_WRITE_ADDR_UPPER 0x4034
#define ODSY_HOST_WRITE_ADDR_LOWER 0x403c

/*
 * The context-shadow address: a 48-bit guest physical address split the
 * same way as the host-write pair above, bits 15:0 of the upper half plus
 * all of the lower half. The kernel driver's context switch clears the
 * outgoing context's completion word, stores the switch word naming the
 * incoming context to ODSY_CTX_SWITCH_PORT below, polls the outgoing
 * context's shadow plus ODSY_CTX_SWITCH_DONE_OFFSET, and only then writes
 * the incoming context's shadow address to this pair, each store followed
 * by a barrier (docs/reviews/odyssey-datasheet-fold-8eaf559842.md,
 * claim 13). So at a switch the pair already names the shadow of the
 * context being switched out, which is where the switch completes.
 * Write only.
 */
#define ODSY_CTX_SHADOW_ADDR_UPPER 0x4004
#define ODSY_CTX_SHADOW_ADDR_LOWER 0x400c

/*
 * bits 28:25 of ROUTE_DEST_UPPER, and two of the four source bytes within
 * ROUTE_VECTORS. Distinct positions from int_dest_upper's own widget and
 * vector fields above -- two separate destination registers, not aliases.
 * The kernel driver's interrupt setup packs the vectors of its routed
 * interrupts #4..#1 into bytes 31:24..7:0; the top byte is bound to the
 * softsync ISR and byte 23:16 to the kernel driver's vertical-retrace
 * handler (the binding the guest driver programs), and byte 15:8 to the
 * kernel driver's DMA-done interrupt handler. Byte 7:0 (the flag source)
 * is stored but nothing here raises through it.
 */
#define ODSY_ROUTE_DEST_WIDGET_SHIFT 25
#define ODSY_ROUTE_VECTOR_SOFTSYNC_SHIFT 24
#define ODSY_ROUTE_VECTOR_RETRACE_SHIFT 16
#define ODSY_ROUTE_VECTOR_DMA_SHIFT 8

/*
 * The "Buzz registers" the stock IP35 BaseIO PROM's graphics diagnostic
 * tests, adjoining but distinct from the kernel driver's own register
 * scan (docs/research/graphics/odyssey-baseio-prom-graphics-20260925.md).
 * No name or reset value is established for any of the six; each is
 * plain read/write storage like the widget-control and software-sync
 * registers, which is enough for a walking-ones/walking-zero-complement
 * self-test to pass without inventing a bit layout that no available
 * source establishes.
 */
#define ODSY_BUZZ_1 0x1078
#define ODSY_BUZZ_2 0x1088
#define ODSY_BUZZ_3 0x1098
#define ODSY_BUZZ_4 0x10a8
#define ODSY_BUZZ_5 0x10b8
#define ODSY_BUZZ_6 0x10c0

/*
 * The swap-control register bank
 * (docs/analysis/odyssey-swap-wake-mechanism.md), as the guest driver's
 * buffer swap and vertical-retrace handler use it, interleaved with the
 * Buzz registers above but a distinct mechanism. Every mask has one bit
 * per window, 0..31.
 *
 *   ODSY_SWAP_ARM (0x10b4): windows whose displayed buffer flips at the next
 *     retrace. Established: the kernel driver's buffer swap writes
 *     (shadow ^ wanted) & members.
 *   ODSY_SWAP_PENDING (0x10bc): swap-group pending mask; stored, inert.
 *   ODSY_SWAP_CONTROL / _COMPANION (0x10c4/0x10cc): windows set to show
 *     buffer 1 / buffer 0 at once, the driver's immediate path. Inferred.
 *   ODSY_DISPLAYED_BUFFER (0x10d4): which buffer each window shows, compared
 *     by the retrace ISR against the buffer it asked for. The driver's
 *     expectation is established; the board's encoding is inferred from it.
 */
#define ODSY_SWAP_ARM 0x10b4
#define ODSY_SWAP_PENDING 0x10bc
#define ODSY_SWAP_CONTROL 0x10c4
#define ODSY_SWAP_CONTROL_COMPANION 0x10cc
#define ODSY_DISPLAYED_BUFFER 0x10d4

/*
 * Front and back storage (docs/datasheets/odyssey.md, "Buffer-selection
 * software contract" and "Buffer swap and retrace"). The model keeps two
 * colour buffers and names them by the displayed-buffer bits' sense:
 * buffer 0 is what a window with its ODSY_DISPLAYED_BUFFER bit clear shows,
 * buffer 1 what it shows with the bit set. This is an internal abstraction,
 * not the board's storage: where the board keeps each buffer and how it
 * packs them are unknown, so each is one model plane of the shape the single
 * plane always had.
 *
 * Drawing. ESTABLISHED as software writes: on its stereo/double-buffer-
 * capable drawable branch, the retained IP30 64-bit GL library puts
 * front-left, front-right, back-left and back-right in bits 0 to 3 of a
 * virtual drawing mask (this does not establish every IP35 n32 selection
 * path), and the kernel sends that mask through register 0x0424 unchanged
 * while the drawable's buffer state is zero, and with its low and high bit
 * pairs exchanged while it is not. A zero mask stays zero. INFERRED: the
 * pair exchange is the kernel naming buffers rather than roles, so after it
 * bits 1:0 name buffer 0 and bits 3:2 buffer 1; a drawable's front is the
 * buffer its window shows, which alternates at each swap, and the exchange
 * tracks it. The model draws every colour write into the buffers the last
 * 0x0424 mask names and into neither for a zero mask.
 *
 * APPROXIMATIONS. Stereo is not modeled: a right-eye bit selects the same
 * buffer as its left-eye bit and is reported once. The mask's reset value
 * is unknown; ODSY_DRAW_MASK_RESET draws buffer 0, the buffer every window
 * shows after the driver's reset write of 0x10cc, so drawing that comes
 * before any 0x0424 -- the PROM's and the kernel console's -- is seen.
 * The kernel's separate nonzero-to-0xc branch for software mode-mask 0x3c00
 * reaches the model as the mask 0xc it emits; that branch's visual format is
 * unknown. The 0x1017 destination-mode changes the GL library makes around
 * its draw selection are not given a meaning. One drawing mask is kept
 * across context switches, as every other register is under the
 * context-save policy; whether the kernel re-pushes 0x0424 at switch-in is
 * not in the cleared contract. If it does not, a client's mask outlives its
 * context and the X server's 2D writes follow it. A guest run of a
 * double-buffered client beside X tells the two apart.
 *
 * Reading. ESTABLISHED: 0x608 carries the bound drawing drawable's word and
 * 0x609 the bound reading drawable's, independently, bit 31 set for software
 * buffer state zero. UNKNOWN: the address decoding, the physical polarity of
 * bit 31, and which engine consumes each word. The model keeps both words
 * raw (draw_word, read_word) for the pixel-readback path; no model path
 * reads a colour buffer by them. The screen-to-screen copy's source buffer is
 * part of that unknown, so a copy runs within each buffer the draw mask
 * selects (odyssey_copy_rect()).
 */
#define ODSY_REG_DRAW_MASK 0x0424
#define ODSY_BUFFERS 2
#define ODSY_DRAW_MASK_BITS 0xf
#define ODSY_DRAW_MASK_BUFFER0 0x3       /* left and right eye of buffer 0 */
#define ODSY_DRAW_MASK_BUFFER1 0xc       /* left and right eye of buffer 1 */
#define ODSY_DRAW_MASK_RIGHT 0xa         /* the right-eye bits */
#define ODSY_DRAW_MASK_RESET 0x1         /* buffer 0, left: an approximation */

/*
 * Reset values a real Tezro V12's firmware diagnostics printed for a
 * healthy board (odyssey-baseio-prom-graphics-20260925.md, cited through
 * odyssey-fuel-identity-spike-20260925.md): widget request timeout and
 * flow-control configuration. The Buzz registers above carry no such
 * reference value, so they reset to zero.
 */
#define ODSY_XT_REQ_TIMEOUT_RESET 0x000fffff
#define ODSY_FLCTL_CONFIG_RESET 0x0001c000

/*
 * Status register 0x1064 (odsy status0). This model's command FIFO drains
 * synchronously in the write handler, so it is always enabled, always empty
 * and always below the low-water mark, and never fills. The kernel's periodic
 * gfx-credits check reads this register and treats a clear low-water or
 * enabled bit as a graphics error, so a board that never answered it (the
 * default no-register path, reading zero) drew an "odsy board 0: Graphics
 * error" alert on every check and then took its board-reset recovery path.
 * The bit names are established in the datasheet's status table; the real
 * board's timing-dependent FIFO occupancy is not modeled (see the datasheet's
 * approximation ledger).
 */
#define ODSY_STATUS0_FIFO_ENABLED 0x10000000
#define ODSY_STATUS0_FIFO_LOW_WATER 0x00020000
#define ODSY_STATUS0_FIFO_EMPTY 0x00010000
#define ODSY_STATUS0_READY \
    (ODSY_STATUS0_FIFO_ENABLED | ODSY_STATUS0_FIFO_LOW_WATER | \
     ODSY_STATUS0_FIFO_EMPTY)

/*
 * ODSY_INTR_FLAGS' low-water bit, the command-FIFO low-water interrupt. The
 * driver enables this source and waits on it to replenish its graphics command
 * credits: on real hardware it fires when the FIFO drains to its low-water
 * mark. This model drains the FIFO synchronously in the write handler, so every
 * command submission leaves the FIFO empty and below low water. Raise it on
 * each submission (the drain edge). Without it "gfx credits below bias" never
 * clears, the kernel logs "odsy board 0: Graphics error", and it gives up
 * starting Xsgi.
 * The real board's timing-dependent occupancy is not modeled (see the
 * datasheet's approximation ledger).
 */
#define ODSY_INTR_FIFO_LOW_WATER 0x00000200

/*
 * SGI's MicroLAN control register, the layout the Bridge, the XBridge and
 * the Hub all carry: pulse width in bits 19:10, sample offset in 9:2,
 * completion in bit 1 and the sampled wire level in bit 0.
 */
#define ODSY_NIC_LEVEL (1u << 0)
#define ODSY_NIC_DONE (1u << 1)
#define ODSY_NIC_SAMPLE_SHIFT 2
#define ODSY_NIC_SAMPLE_MASK (0xffu << ODSY_NIC_SAMPLE_SHIFT)
#define ODSY_NIC_PULSE_SHIFT 10
#define ODSY_NIC_PULSE_MASK (0x3ffu << ODSY_NIC_PULSE_SHIFT)

/* Board-identity part family 0x0b, one 32-byte page, 48-bit serial. */
#define ODSY_NIC_FAMILY 0x0b
#define ODSY_NIC_PAGE 32
#define ODSY_NIC_ROM_SIZE 160
#define ODSY_NIC_SERIAL_MASK MAKE_64BIT_MASK(0, 48)
#define ODSY_NIC_RECORD_DEFAULT "Name:ODY128VERSIONB"

/*
 * The display back end's indexed register file is a 32-bit word FIFO, not a
 * selector-plus-data register (docs/analysis/odyssey-colormap-format.md).
 * Each 8-byte store to ODSY_BACKEND_SELECT is two words, upper half first; a
 * 4-byte store is one word. The stream is a run of counted records: a header
 * word carries tag 0b11 in bits 29:28, an entry index in bits 27:14 and a
 * count in bits 13:0, and the next `count` words are data for consecutive
 * entries starting at that index. count == 0 is a pad, a no-op. The I2C
 * engine's fixed 0x3xx00001 selector -- index in bits 27:14, count 1 -- is
 * just the count == 1 case of this, so routing its one data word to the I2C
 * command registers is preserved unchanged. Each register is separately
 * readable at ODSY_BACKEND_READ_BASE + (index << 3).
 */
#define ODSY_BACKEND_SELECT 0x400000
#define ODSY_BACKEND_READ_BASE 0x220000
#define ODSY_BACKEND_SELECTOR_INDEX_SHIFT 14
/* The header's index (bits 27:14) and count (bits 13:0) fields. */
#define ODSY_BACKEND_SELECTOR_INDEX_MASK 0x3fff
#define ODSY_BACKEND_COUNT_MASK 0x3fff
/*
 * A header word's tag, bits 29:28, always 0b11 in every observed record.
 * Bits 31:30 vary (the LUT ioctl paths set bit 30, init and the I2C engine
 * clear it) and carry no meaning this model needs, so they are ignored.
 */
#define ODSY_BACKEND_TAG_SHIFT 28
#define ODSY_BACKEND_TAG_MASK 0x3
#define ODSY_BACKEND_TAG_HEADER 0x3

/*
 * The colour lookup RAM: entry indices below ODSY_LUT_SIZE hold one 30-bit
 * colour each, ten bits per channel, red in bits 29:20, green in 19:10, blue
 * in 9:0 (docs/analysis/odyssey-colormap-format.md). An 8-bit indexed pixel
 * resolves through entry 256*group + pixel; the top eight bits of each
 * channel are the colour to show (the X server has already applied its
 * software gamma to colormap entries). Entries 0x1a00 to 0x1fff are the
 * 1,536-entry gamma table RGB pixels pass through (odyssey_resolve_row()).
 */
#define ODSY_LUT_SIZE 0x2000
#define ODSY_LUT_ENTRY_MASK 0x3fffffff
#define ODSY_LUT_RED_SHIFT 22
#define ODSY_LUT_GREEN_SHIFT 12
#define ODSY_LUT_BLUE_SHIFT 2

/*
 * The per-drawable display-mode registers at ODSY_DISPMODE_BASE + DID
 * (drawable id 0..31). Bits 8:4 of the word name the colormap group the
 * drawable's indexed pixels resolve through
 * (docs/analysis/odyssey-colormap-format.md).
 */
#define ODSY_DISPMODE_BASE 0x2900
#define ODSY_DISPMODE_COUNT 32
#define ODSY_DISPMODE_GROUP_SHIFT 4
#define ODSY_DISPMODE_GROUP_MASK 0x1f

/*
 * Colour resolution at scanout (docs/analysis/odyssey-colormap-scanout.md).
 * The plane keeps what the guest drew; the display back end resolves each
 * pixel through its drawable id's display mode and the lookup RAM as they
 * stand when the frame is shown. ESTABLISHED: the X server changes a
 * window's colormap by rewriting its display-mode register, never by
 * repainting; its screen blank zeroes the gamma table and every colormap and
 * its unblank reloads them; the RGB visuals' display modes name group 26,
 * base 0x1a00, the gamma table. INFERRED: that the board reads the id plane,
 * display mode and lookup RAM per pixel at scanout, which is the only
 * reading under which the blank and the colormap install work.
 */
#define ODSY_GAMMA_GROUP 26        /* base 0x1a00: groups >= 26 are RGB */
#define ODSY_GAMMA_BASE (ODSY_GAMMA_GROUP * 256)
#define ODSY_CMAP_UNVERIFIED_GROUP 25 /* base 0x1900, no software gamma */
#define ODSY_DID_MAIN_SHIFT 3
#define ODSY_DID_OVERLAY_MASK 0x7
/*
 * APPROXIMATION: a pixel whose main drawable id is 0 resolves through
 * drawable 1's display mode. The X server writes the root window's id
 * planes to 0 (mask 0xff) once its session tiles the root, and paints the
 * root with indices allocated in the default colormap at 0x1100, group 17,
 * which drawable 1's mode names; drawable 0's own mode, 0x004b5f00, names
 * group 16, which the server never loads. So a board that resolved main id
 * 0 through 0x2900 as written would show the root black, which IRIX does
 * not. How the board really treats main id 0 is UNKNOWN; this keeps the
 * root's intended colours until a capture or register map settles it.
 */
#define ODSY_DID_ROOT_MODE 1
/* The colour plane word of an index pixel: the index, low 12 bits (model). */
#define ODSY_INDEX_MASK 0xfff

#define ODSY_I2C_MAIN_CMD_INDEX 0x2780
#define ODSY_I2C_OPT_CMD_INDEX 0x2740

#define ODSY_I2C_MAIN_CMD_READ 0x233c00
#define ODSY_I2C_MAIN_STATUS_READ 0x233c08
#define ODSY_I2C_OPT_CMD_READ 0x233a00
#define ODSY_I2C_OPT_STATUS_READ 0x233a08

/*
 * gfxinfo's displayed back-end version, bits 2:0: established as read, the
 * driver effect otherwise unknown. The model answers 0 for every profile;
 * a measured 128 MB second-generation board is believed to answer 1
 * (docs/reviews/odyssey-family-behaviour-evidence-20261001.md). Pinned
 * explicitly as an approximation rather than left to the unimplemented
 * fallback.
 */
#define ODSY_BACKEND_VERSION 0x23380c

/*
 * Status bits this model computes at read time: DONE set whenever the last
 * command word written was non-zero, READY set unconditionally, since no
 * transfer takes simulated time and the engine is always idle when the
 * guest looks. ACK, DATA and the 13:10 engine-state field are not stored:
 * no downstream device is modeled, so a transfer always acknowledges and
 * always answers zero, and the reset poll for state 0 or 0xa never blocks.
 */
#define ODSY_I2C_STATUS_DONE (1u << 9)
#define ODSY_I2C_STATUS_READY (1u << 22)

/*
 * The command FIFO: the guest's only path to painting the framebuffer.
 * Hardware-sequenced, not address-counted (docs/datasheets/odyssey.md
 * qtest checklist item 21), so every store here -- 32-bit or 64-bit -- is
 * the next word of a command stream, never a register at an offset.
 * Write-only: nothing reads it back.
 */
#define ODSY_CMD_FIFO 0x110000

/*
 * The per-context USER command FIFO. The X server DDX maps the board into each
 * client process and pushes its entire drawing-command stream here (board
 * aperture offset 0x100000; docs/datasheets/odyssey.md user-mappings table),
 * while the kernel driver uses ODSY_CMD_FIFO (0x110000). It carries the same
 * command grammar this interpreter already decodes: a 4-byte store is one FIFO
 * word, an 8-byte store is two. Until this was routed, every user-side drawing
 * word fell through to odyssey_unimplemented() and was dropped, so Xsgi ran and
 * enumerated the board but nothing ever reached the framebuffer and the screen
 * stayed black (docs/analysis/odyssey-screeninit-stall.md). Unlike the kernel
 * port, the user port never carries the command-FIFO self-test preamble, so it
 * does not feed odyssey_selftest_track().
 */
#define ODSY_USER_CMD_FIFO 0x100000

/*
 * The per-context command port: a single 8-byte store that is the
 * kernel driver's context switch in full, not a command-FIFO word. Close
 * in the aperture to ODSY_CMD_FIFO above but a distinct mechanism --
 * mapped into user processes at its own page (docs/datasheets/odyssey.md,
 * user-mappings table). The word is (7 << ODSY_CTX_SWITCH_OPCODE_SHIFT) |
 * (new_ctx_id << ODSY_CTX_SWITCH_ID_SHIFT) | ODSY_CTX_SWITCH_VALID
 * (a boot trace read back 0x70000010001 switching to context 1). The
 * driver polls guest RAM at the context shadow address
 * (ODSY_CTX_SHADOW_ADDR_UPPER/_LOWER) plus ODSY_CTX_SWITCH_DONE_OFFSET for
 * a nonzero 32-bit word, with a 15 s timeout and no interrupt -- the
 * kernel driver's context-switch wait reports "CTXSW timeout" on a stall.
 */
#define ODSY_CTX_SWITCH_PORT 0x118000
#define ODSY_CTX_SWITCH_OPCODE_SHIFT 40
#define ODSY_CTX_SWITCH_OPCODE 0x7
#define ODSY_CTX_SWITCH_VALID (1ULL << 0)
#define ODSY_CTX_SWITCH_ID_SHIFT 16
#define ODSY_CTX_SWITCH_DONE_OFFSET 0x8fc

/*
 * The command-FIFO self-test read-back. The kernel driver's command-FIFO
 * initialisation pushes a fixed nine-word preamble (three command words to
 * the FIFO, five 64-bit selector pushes to the backend-select port, one
 * more command word), waits 2 ms, then reads this register and requires
 * the OK value; a real V12 answers exactly 0x003d0046
 * (odyssey-octane2-v12-registers-20260926.md). Both the kernel's console
 * graphics initialisation and the kernel driver's X-server initialisation
 * path retry six times and give up if it never matches, so the whole
 * graphics bring-up depends on it.
 */
#define ODSY_CFIFO_SELFTEST 0x201594
#define ODSY_CFIFO_SELFTEST_OK 0x003d0046

/*
 * A "register-select" word, 0x0001nnnn with the register in the low 16
 * bits. Every such word carries its own operand-word count in bits 13:10,
 * the field value 15 standing for a 16-word burst
 * (docs/analysis/odyssey-user-stream-arities.md, established from the X
 * server's command stream in a traced login: a burst word carries its
 * count in bits 13:10, a 16-word burst encoded as 15). Replaying the whole
 * traced stream with only that rule leaves no word at a command position
 * unaccounted for, so odyssey_cmd_arity() reads the field rather than a
 * hand-built table -- no register-select word has an unknown arity.
 */
#define ODSY_CMD_REGISTER_SHAPE 0xffff0000
#define ODSY_CMD_REGISTER_TAG 0x00010000

/*
 * The operand-count field every command word carries: bits 13:10, with the
 * value 15 encoding a full 16-word burst (the DDX never emits a literal 15).
 * The pixel-run length below reads this same field.
 */
#define ODSY_CMD_ARITY_SHIFT 10
#define ODSY_CMD_ARITY_MASK 0xf
#define ODSY_CMD_ARITY_BURST 16

/*
 * Every command word carries its own operand count in bits 13:10, the field
 * value 15 encoding a 16-word burst (docs/analysis/odyssey-user-stream-
 * arities.md): the whole traced login stream obeys it, register-select words
 * and the bare structural words alike, so there is no "unknown arity" case
 * left to report. A register-select's low 16 bits carry the same field, so
 * this reads correctly whether passed the whole word or just the register.
 */
static unsigned odyssey_cmd_arity(uint32_t word)
{
    unsigned count = (word >> ODSY_CMD_ARITY_SHIFT) & ODSY_CMD_ARITY_MASK;

    return count == ODSY_CMD_ARITY_MASK ? ODSY_CMD_ARITY_BURST : count;
}

#define ODSY_REG_QUICK_FLUSH_ARG 0x0443  /* 1 operand: 0xfa or 0x96 seen */
#define ODSY_REG_STROBE 0x0046           /* 0 operands, repeated in flush */
#define ODSY_REG_FULL_FLUSH_EXTRA 0x0019 /* 0 operands, full flush only */
#define ODSY_REG_GL_BEGIN 0x4400         /* 1 operand: the GL primitive */
#define ODSY_REG_GL_END 0x4001           /* 0 operands */
/* 0x0405, glyph pixel setup, is also the texturing mode: ODSY_REG_TEX_MODE. */
#define ODSY_REG_PIXEL_MODE_EXT 0x1453   /* 5 operands: glyph pixel setup */

/*
 * A plain 1-operand register, seen with operand 0x00120000, immediately
 * ahead of the real copy-rect trigger below (docs/datasheets/odyssey.md's
 * command-port table) and, per the same table, ahead of the X module's
 * pixel-readback sequence too. Its own meaning is not established beyond
 * arity; only ODSY_CMD_COPY_RECT_TRIGGER, not this register, used to be
 * mistaken for the blit entry point (see that constant's comment).
 */
#define ODSY_REG_BLIT_PREFIX 0x0658

/*
 * The host-write/sync command the kernel driver's DMA completion wait
 * uses: two operands, op0 = slot | ODSY_HOST_WRITE_INTERRUPT, op1 = a
 * 16-bit token. Writes big-endian op1 into guest RAM at the host-write
 * base (ODSY_HOST_WRITE_ADDR_UPPER/_LOWER) plus 2 * (op0 & 0x7fff), then,
 * when ODSY_HOST_WRITE_INTERRUPT is set, raises the softsync interrupt
 * through ODSY_ROUTE_DEST_UPPER/_LOWER/_VECTORS. The kernel driver's DMA
 * completion wait only polls that halfword from its interrupt handler, so
 * a driver command that reaches this register times out (its 15 s DMA
 * wait) unless both the write and the interrupt happen.
 */
#define ODSY_REG_HOST_WRITE 0x0a5a
#define ODSY_HOST_WRITE_INTERRUPT (1u << 15)

/*
 * The page-table pixel/texture DMA descriptor
 * (docs/analysis/odyssey-dma-submit-swap-path.md). The driver pushes two 64-bit
 * doublewords through the command FIFO carrying an opcode plus the physical base
 * of a translation table: 0xe01 for a transfer into board memory and 0xe03 (the
 * driver's mode bit 4, a direction/variant) for one out of it. The command FIFO
 * splits an 8-byte store into two 32-bit words upper half first, so the two
 * doublewords reach the interpreter as four words: the opcode word, then three
 * more this model reads as one register-select-style operand run.
 *
 * ESTABLISHED: the opcode values, that a mode bit picks 0xe01 vs 0xe03, that the
 * descriptor is two sync-fenced doublewords, and how the driver packs the
 * table's physical address P into them (the kernel driver's DMA descriptor
 * builder, docs/analysis/odyssey-account-icons.md). Each 24-bit half of P
 * is stored as (h & 0x1ffff) | (h & 0xfe0000) << 6: the high half (P >> 24)
 * in the first doubleword's low word (operand 0 here), the low half in the
 * second doubleword's high word (operand 1). The second doubleword's low
 * word (operand 2) is always zero. INFERRED: the board reverses that packing,
 * P = w3[16:0] | w3[29:23] << 17 | w1[16:0] << 24 | w1[29:23] << 41. Every
 * traced descriptor then names a table whose entries point at guest pages.
 * The descriptor raises no fence of its own: the driver follows each one with
 * a 0x0a5a host-write fence. The opcode does not give an image transfer's
 * direction: the kernel's image path only ever emits 0xe01, for both
 * directions, and its 0xe03 branch is unreachable from the image ioctl
 * (docs/reviews/odyssey-read-header-encoding-evidence-20260930.md). Only the
 * diagnostic region path can emit 0xe03; the model keeps that opcode
 * reported and unperformed. An 0xe01 transfer's direction is the header's
 * word 7 bit 24 (ODSY_DMA_TO_HOST).
 */
#define ODSY_DMA_DESC_UPLOAD 0x00000e01   /* opcode 0xe01: either direction */
#define ODSY_DMA_DESC_READBACK 0x00000e03 /* opcode 0xe03: region path only */
#define ODSY_DMA_DESC_OPERANDS 3          /* the three words after the opcode */
#define ODSY_DMA_DESC_ADDR_LOW_MASK 0x1ffff /* bits 16:0 of a packed half */
#define ODSY_DMA_DESC_ADDR_HIGH_SHIFT 23    /* bits 29:23: half bits 23:17 */
#define ODSY_DMA_DESC_ADDR_HIGH_MASK 0x7f

/*
 * The two buffer-select doublewords the kernel driver's buffer-select push
 * sends ahead of a swap, 0x608 and 0x609 each with one operand word taken
 * from the drawable (docs/analysis/odyssey-swap-wake-mechanism.md;
 * 0x2000/0x2000 in the swap capture). The FIFO splits each 8-byte store
 * into two words upper half first, so each reaches the interpreter as an
 * opcode word followed by one operand word. They are register writes, not
 * a completion fence: the swap
 * completes on the retrace hook (odyssey_retrace()). 0x608 names the bound
 * drawing drawable and 0x609 the bound reading one (ODSY_REG_DRAW_MASK's
 * comment); what they do to rendering is unknown, so the model keeps each
 * word raw and draws by the 0x0424 mask alone. Distinct from the
 * DMA descriptor (0xe01/0xe03) and the host-write softsync (0x0a5a).
 */
#define ODSY_SWAP_SYNC_A 0x00000608
#define ODSY_SWAP_SYNC_B 0x00000609
/* The bound reading drawable's packed y:x origin, distinct from 0x841. */
#define ODSY_REG_READ_ORIGIN 0x0000060b
#define ODSY_SWAP_SYNC_OPERANDS 1

/*
 * The fence word the 0x0a5a host-write command's operands form (see
 * ODSY_REG_HOST_WRITE). Bits 14:0 are the fence slot, bit 15 requests the
 * softsync interrupt, and bits 31:16 are the token the board writes into the
 * fence halfword.
 */
#define ODSY_DMA_FENCE_SLOT_MASK 0x7fff
#define ODSY_DMA_FENCE_INTERRUPT (1u << 15)
#define ODSY_DMA_FENCE_TOKEN_SHIFT 16
/*
 * The completion cookie a head-ops publish() carries: the 32-bit fence word
 * above (slot, interrupt bit, token) under a validity marker, so a completion
 * that wants no fence -- a DMA transfer, which the driver fences itself --
 * passes zero. odyssey_fence() below is the single fence implementation.
 */
#define ODSY_FENCE_VALID (1ull << 32)
#define ODSY_FENCE_WORD_MASK 0xffffffffu

/*
 * The translation table (docs/analysis/odyssey-dma-submit-swap-path.md): a
 * 0x20-byte header, then 8-byte entries at 16 KiB (0x4000) page granularity,
 * terminated by an entry whose length field is zero -- the eight bytes the
 * driver's own allocation (pagecount * 8 + 0x28) leaves past a 0x20 header and
 * pagecount entries. ESTABLISHED: the header size, the 8-byte entry size and the
 * 16 KiB page size. OBSERVED in the three tables the login greeter's DMAs name
 * (read at the address the descriptor decode above gives): each entry is
 * (byte_address << 16) | length, the first may start part-way into its page
 * and run to the page's end, the following ones are whole pages, the last
 * may be short, and a zero entry ends the list. INFERRED: the board walks
 * them in that order. An over-long walk fails the transfer rather than being
 * guessed at.
 *
 * The 0x20-byte header is eight big-endian words
 * (docs/analysis/odyssey-dma-image-header.md). ESTABLISHED from the kernel's
 * own reading and patching of them: word 1 is the board rectangle's top-left
 * (y << 16 | x), word 3 the size (height << 16 | width, a zero height meaning
 * 0x10000), word 5 the source bytes skipped after each row, word 6's low 20
 * bits the bytes per source row, word 7's bits 18:16 a bytes-per-pixel code
 * through the kernel's table below, and word 7's bit 31 the request for the
 * DMA-done interrupt. The entries count pixel bytes only: the driver steps
 * over a row gap inside an entry and never counts it, so an entry's address
 * span can be longer than its length. INFERRED from values: word 0 names the
 * board buffer read or written, the same value the X server sends as the
 * first operand of its colour 0x1017 destination-select (0x2000 for the
 * screen), and word 2 is that buffer's width in 16-pixel units. Word 4 is the
 * drawable's pixel-format word; only a board-to-host read decodes it
 * (ODSY_DMA_TO_HOST).
 */
#define ODSY_DMA_PAGE_SIZE 0x4000
#define ODSY_DMA_TABLE_HEADER 0x20
#define ODSY_DMA_HDR_WORDS 8
#define ODSY_DMA_HDR_DEST 0
#define ODSY_DMA_HDR_ORIGIN 1
#define ODSY_DMA_HDR_WIDTH 2
#define ODSY_DMA_HDR_SIZE 3
#define ODSY_DMA_HDR_SKIP 5
#define ODSY_DMA_HDR_ROWBYTES 6
#define ODSY_DMA_HDR_CONTROL 7
#define ODSY_DMA_ROWBYTES_MASK 0xfffff
#define ODSY_DMA_BPP_SHIFT 16
#define ODSY_DMA_BPP_MASK 0x7
#define ODSY_DMA_IRQ_REQUEST (1u << 31)
#define ODSY_DMA_PTE_LEN_MASK 0xffff
#define ODSY_DMA_PTE_ADDR_SHIFT 16
/* A work budget bounding the entry walk when no terminator is reached. */
#define ODSY_DMA_MAX_PAGES 256

/*
 * Board-to-host transfers (docs/reviews/odyssey-read-header-encoding-
 * evidence-20260930.md, "Sanitized readback implementation handoff"). Every
 * rule below is software behaviour read from the kernel, the X module and
 * the GL library; the board itself was never observed, so each is an
 * APPROXIMATION of an unknown decode.
 *
 * - Word 7 bit 24 is the direction: set for board to host. The kernel sets
 *   it exactly when the ioctl mode's bit 0 is clear, which every reading
 *   caller uses, and sets bit 31 on the last block of every such transfer;
 *   the earlier blocks of a transfer split into pieces carry bit 24 alone.
 *   The opcode, word 0 bit 29, word 4 bit 28 and word 7 bits 31, 29, 28 and
 *   25 do not mark a read.
 * - Word 0 with bit 29 clear is a board base in 16-byte units, as for an
 *   upload. With bit 29 set it is a sentinel resolved through the bound
 *   drawable: 0x30000019 with word 4 bits 30:28 = 1 the read drawable and
 *   0x30000010 with them 0 the draw drawable (reachable from GL's DMA image
 *   write, never captured). The low bits' board meaning is unknown.
 * - Word 4 bit 31 is set for the front buffer and clear for the back under
 *   the GL library's convention, and is consulted for sentinel reads only;
 *   address transfers always carry it set. INFERRED mapping onto the
 *   model's two buffers: front is the buffer the pixel's main drawable
 *   shows (its displayed-buffer bit, as scanout resolves it), back the
 *   other. An address read of the colour buffer reads the shown buffer.
 *   Between a swap's scheduling and the retrace that latches it, the
 *   kernel's buffer state and the displayed bit disagree; the model follows
 *   the displayed bit.
 * - A sentinel transfer's word 1 is drawable-relative with y top-down: the
 *   current window origin is added. An address transfer's is a screen or
 *   object coordinate. INFERRED from the kernel's own clipping arithmetic.
 * - Word 7 bit 30 puts the bottom board row of the rectangle in host row 0.
 *   Every GL read into client memory sets it. That the board flips rows
 *   inside one block is INFERRED.
 * - A GL read leaves word 7 bits 7:0 zero; its layout is word 4 bits 2:0
 *   (source class) with bits 7:3 (transfer code). An X read puts the image
 *   format byte there instead, and the X server's 24-bit word 4 code does not
 *   agree with the GL vocabulary, so the format byte is used and word 4 is not.
 */
#define ODSY_DMA_HDR_FORMAT 4
#define ODSY_DMA_TO_HOST (1u << 24)        /* word 7 */
#define ODSY_DMA_BOTTOM_UP (1u << 30)      /* word 7 */
#define ODSY_DMA_SENTINEL (1u << 29)       /* word 0 */
#define ODSY_DMA_SENTINEL_READ 0x30000019u
#define ODSY_DMA_SENTINEL_DRAW 0x30000010u
#define ODSY_DMA_FMT_FRONT (1u << 31)      /* word 4 */
#define ODSY_DMA_FMT_SELECTOR_SHIFT 28     /* word 4 bits 30:28 */
#define ODSY_DMA_FMT_SELECTOR_MASK 0x7
#define ODSY_DMA_FMT_SELECTOR_READ 1
#define ODSY_DMA_FMT_SELECTOR_DRAW 0
#define ODSY_DMA_FMT_CLASS_MASK 0x7        /* word 4 bits 2:0 */
#define ODSY_DMA_FMT_CODE_SHIFT 3          /* word 4 bits 7:3 */
#define ODSY_DMA_FMT_CODE_MASK 0x1f
/* The GL library's source classes for 8- and 12-bit RGBA visuals. */
#define ODSY_DMA_GL_CLASS_RGBA8 2
#define ODSY_DMA_GL_CLASS_RGBA12 3

/* Board-to-host and sentinel reports, each made once (dma_read_reported). */
enum {
    ODSY_DMA_REPORT_DRAW_SENTINEL = 1u << 0,
    ODSY_DMA_REPORT_SENTINEL = 1u << 1,
    ODSY_DMA_REPORT_LAYOUT = 1u << 2,
    ODSY_DMA_REPORT_SOURCE = 1u << 3,
    ODSY_DMA_REPORT_BOUNDS = 1u << 4,
    ODSY_DMA_REPORT_INDEX = 1u << 5,
    ODSY_DMA_REPORT_SHAPE = 1u << 6,
    ODSY_DMA_REPORT_WRITE = 1u << 7,
    ODSY_DMA_REPORT_UPLOAD_SENTINEL = 1u << 8,
    ODSY_DMA_REPORT_INDEX_OBJECT = 1u << 9,
    ODSY_DMA_REPORT_COLOUR_OVER_INDEX = 1u << 10,
};

/*
 * Off-screen board-memory objects (docs/analysis/odyssey-gl-texturing.md,
 * section 3). A 0x1017 destination select's first operand is a board-memory
 * address in 16-byte units and its second the buffer's row length in
 * 16-pixel units (ESTABLISHED from five adjacent texture allocations); the
 * colour buffer the screen shows is one such base, every other base an
 * off-screen object: a texture level, or the X server's own 0x16a000
 * buffer. An image transfer or DMA lands its pixels in the selected buffer.
 *
 * The model keeps each object as its own run of 0xRRGGBBAA words in a
 * device-backed store, keyed by base, rather than at its board address: the
 * board's own layout of a one-byte or three-byte texel, and whether objects
 * of different formats pack as the allocator's addresses suggest, are not
 * established, and a store of whole texel words is what sampling needs. A
 * luminance texel is kept as its byte in every colour channel. The store is
 * a model budget, not the board's memory size: an object that does not fit
 * is refused and reported.
 */
#define ODSY_OBJ_POOL_BYTES (16 * MiB)
#define ODSY_OBJ_POOL_WORDS (ODSY_OBJ_POOL_BYTES / 4)
#define ODSY_DEST_WIDTH_INDEX 1        /* 0x1017's row length, 16-pixel units */
#define ODSY_DEST_WIDTH_UNIT 16
/*
 * The select's format word: 0x2... for every colour buffer a drawable shows
 * (0x20006850, 0x20006950, 0x20086a00, 0x20006960) and the id planes, and
 * 0x08... for the texture objects (0x080024d0, 0x08000020). INFERRED from
 * those values only: the top nibble tells the screen from an object.
 */
#define ODSY_DEST_FORMAT_SHIFT 28
#define ODSY_DEST_FORMAT_SCREEN 0x2

/*
 * The 2D stream's GL primitives (docs/analysis/odyssey-account-icons.md). The
 * X server draws rectangles with QUADS, one per four vertices, and the account
 * icons and the greeter's bevels with LINES spans, LINE_STRIP outlines and two
 * POLYGON trapezoids. LINE_LOOP is the strip plus its closing segment. POINTS
 * is what gr_osview and glfalls draw integer points with
 * (docs/analysis/odyssey-gl-demos.md, failure 4).
 */
#define ODSY_GL_POINTS 0x00000000
#define ODSY_GL_LINES 0x00000001
#define ODSY_GL_LINE_LOOP 0x00000002
#define ODSY_GL_LINE_STRIP 0x00000003
#define ODSY_GL_QUADS 0x00000007
#define ODSY_GL_POLYGON 0x00000009

/*
 * Two GL vertex-stream words carry their own tag instead of the
 * register-select shape above (same primary source, section 4).
 */
#define ODSY_GL_COLOR_3UB 0xc580cc08   /* 3 operands: R, G, B, one byte each */
#define ODSY_GL_VERTEX_2I 0x8080c800   /* 2 operands: X, Y */

/*
 * The greeter draws in PseudoColor: its fills set colour by index, not RGB.
 * The index-mode sequence is a pair of colour-mode words (ODSY_PROG_COLOUR_
 * MODE below), then 0xce80cc08 with three zero words, then the colour-index
 * word with one index (docs/analysis/odyssey-user-stream-arities.md, "Reading
 * of the greeter's fill"). The zero word changes no state this model needs,
 * but it must parse so the index word stays at a command position. Operand
 * counts are the bits-13:10 field, spelled out for clarity.
 *
 * A colour-index OpenGL client uses the same index word for glIndexi and
 * 0xc580c407 with one float for glIndexf; the kernel's context replay sends
 * the index as 0xce80c406, and a client's glIndexf in an RGBA context arrives
 * as 0xce80c407. In a colour-index context the library sends its RGBA colour
 * as 0xce80cc0e (three floats) and 0xce80d015 (four), which GL ignores in
 * index mode (ESTABLISHED from the colour-index probe and gr_osview
 * captures, docs/analysis/odyssey-gl-demos.md).
 */
#define ODSY_GL_COLOR_INDEX 0xc580c406   /* 1 operand: PseudoColor index */
#define ODSY_GL_COLOR_INDEX_F 0xc580c407 /* 1 operand: the index as a float */
#define ODSY_GL_REPLAY_INDEX_F 0xce80c407 /* 1 operand: the index as a float */
#define ODSY_GL_INDEX_MODE_COLOR_3F 0xce80cc0e /* 3 float operands */
#define ODSY_GL_INDEX_MODE_COLOR_4F 0xce80d015 /* 4 float operands */
#define ODSY_MODE_INDEX_ZEROS 0xce80cc08 /* 3 operands: three zero words */

/*
 * The other current-colour and normal words of the IRIX 6.5.30 GL library
 * (docs/reviews/odyssey-query-formats-7-13-evidence-20260930.md, "Sanitized
 * contract handoff", and
 * docs/reviews/odyssey-query-open-items-evidence-20261001.md; software
 * expectations, not measured hardware). The
 * three-int colour word carries the caller's raw signed integers; the kernel's
 * shadow template also uses it as its default saved-colour word. Each colour
 * word has a 0xce80 twin for a context whose colour mode does not render with
 * it. The library sends 0xc180cc18 for glNormal3i and 0xc180cc17 for the
 * short form. The library sends 0xc180cc16 for glNormal3b, three
 * sign-extended bytes one per word.
 */
#define ODSY_GL_COLOR_3I 0xc580cc0d        /* 3 signed integers */
#define ODSY_GL_INDEX_MODE_COLOR_3I 0xce80cc0d
#define ODSY_GL_COLOR_4F 0xc580d015        /* 4 floats: R, G, B, A */
#define ODSY_GL_NORMAL_3B 0xc180cc16       /* 3 signed bytes, one per word */
#define ODSY_GL_NORMAL_3S 0xc180cc17       /* 3 signed shorts, one per word */
#define ODSY_GL_NORMAL_3I 0xc180cc18       /* 3 signed integers */

/*
 * The true-colour immediate-mode GL words a real OpenGL client draws with, the
 * float siblings of the integer 2D words above (docs/analysis/odyssey-gl-
 * triangle-stream.md). A true-colour triangle uses these, not the greeter's
 * colour-index/integer forms, so recognizing them is what tells the two
 * streams apart. glBegin/glEnd (ODSY_REG_GL_BEGIN/END above) are shared: a
 * float-GL BEGIN carries a geometry primitive other than the greeter's QUADS.
 */
#define ODSY_GL_COLOR_3F 0xc580cc0e   /* 3 operands: R, G, B as IEEE floats */
#define ODSY_GL_VERTEX_2F 0x8080c801  /* 2 operands: X, Y as IEEE floats */
/* The GL library's glVertex3f, object coordinates (ESTABLISHED). */
#define ODSY_GL_VERTEX_3F 0x8080cc03  /* 3 operands: X, Y, Z as IEEE floats */

/*
 * Depth state (docs/analysis/odyssey-gl-shading-depth-lighting.md,
 * ESTABLISHED from the GL library's entry points and the captures):
 *
 * - 0x0404 bit 16: the depth test. The GL library's glEnable sets it
 *   for GL_DEPTH_TEST only when the drawable has a depth buffer.
 * - 0x0420 bits 2:0: the comparison, GL_NEVER = 0 to GL_ALWAYS = 7
 *   (the GL library's glDepthFunc stores func & 7).
 * - 0x0452 bit 24: the depth write enable (the GL library's
 *   glDepthMask); bits 23:20 the colour write mask, 0xf for every
 *   colour draw and 0 for a depth-only clear: bit 20 red, 21 green,
 *   22 blue, 23 alpha (the GL library's glColorMask,
 *   docs/reviews/odyssey-lighting-context-save-ea411c21ca.md).
 */
#define ODSY_MODE_DEPTH_TEST (1u << 16)
#define ODSY_REG_DEPTH_FUNC 0x0420      /* 1 operand */
#define ODSY_DEPTH_FUNC_MASK 0x7
#define ODSY_REG_WRITE_MASK 0x0452      /* 1 operand */
#define ODSY_WRITE_DEPTH (1u << 24)
#define ODSY_WRITE_COLOUR_SHIFT 20
#define ODSY_WRITE_COLOUR_MASK 0xf
/*
 * Out of reset: the hardware values are unknown. The model starts at GL's
 * own initial state, GL_LESS with depth and all colour writes on, so a
 * client that never writes these draws as it did before they were decoded.
 */
#define ODSY_DEPTH_FUNC_RESET 0x1
#define ODSY_WRITE_MASK_RESET 0x01f00000

/*
 * Lighting data (docs/analysis/odyssey-gl-shading-depth-lighting.md, section
 * 1, ESTABLISHED from the entry points and two captures with distinct
 * values). glNormal3f is its own word before each vertex; glMaterialfv
 * pushes the front ambient, diffuse, specular and emission as four floats
 * each and the square root of the shininess as one; the back material is the
 * same shape at 0x35..0x39 and 0x908091ea carries four floats derived from
 * the shininess. Light colours are class 0x32 selects, index 3 i + 0, 1, 2
 * (ambient, diffuse, specular of light i) and 24 (the light-model ambient),
 * each followed by 0x8e809015 and four floats; light geometry is a class
 * 0x31 select of index 16 i followed by 0x9080bda2 and sixteen floats. The
 * normal matrix is matrix block 0x34, sixteen floats row-major.
 */
#define ODSY_GL_NORMAL_3F 0xc180cc19         /* 3 operands */
#define ODSY_GL_MATERIAL_AMBIENT 0xc680d02f  /* 4 operands, as the next 7 */
#define ODSY_GL_MATERIAL_DIFFUSE 0xc780d030
#define ODSY_GL_MATERIAL_SPECULAR 0xc880d031
#define ODSY_GL_MATERIAL_EMISSION 0xc980d032
#define ODSY_GL_BACK_AMBIENT 0xca80d035
#define ODSY_GL_BACK_DIFFUSE 0xcb80d036
#define ODSY_GL_BACK_SPECULAR 0xcc80d037
#define ODSY_GL_BACK_EMISSION 0xcd80d038
#define ODSY_GL_SHININESS_SQRT 0xd080c433    /* 1 operand */
#define ODSY_GL_BACK_SHININESS_SQRT 0xd080c439 /* 1 operand */
#define ODSY_GL_SHININESS_DERIVED 0x908091ea /* 4 operands */
#define ODSY_GL_LIGHT_COLOUR_LOAD 0x8e809015 /* 4 operands */
#define ODSY_GL_LIGHT_VEC_LOAD 0x9080bda2    /* 16 operands */
#define ODSY_GL_CLIP_PLANE_LOAD 0x908091bb   /* 4 operands */
#define ODSY_GL_LIGHT_MODEL_AMBIENT 24
#define ODSY_GL_LIGHT_VEC_FLOATS 16
#define ODSY_GL_MATRIX_SELECT_NORMAL 0x34
/*
 * Light block floats: 0-2 position or direction, 3-5 minus the precomputed
 * half vector of a directional light (zero for a positional one), 7-9 the
 * attenuation, 10-12 minus the spot direction, 13 minus the cutoff cosine,
 * 14 minus the spot exponent (INFERRED from their values). 6 and 15 have no
 * known role.
 */
#define ODSY_LIGHT_POS 0
#define ODSY_LIGHT_HALF 3
#define ODSY_LIGHT_ATTEN 7
#define ODSY_LIGHT_SPOT_DIR 10
#define ODSY_LIGHT_SPOT_COS 13
#define ODSY_LIGHT_SPOT_EXP 14

/*
 * The geometry program (docs/analysis/odyssey-gl-geometry-program.md,
 * sections 1 and 2, ESTABLISHED from the library and the captures). Every
 * program word is a class in bits 31:24, bits 22:10 = 0x20 and an 11-bit
 * value split as (value & 0x3ff) | (value >> 10) << 23. Class 0x28 selects a
 * constant-memory index and 0x60 stores a microcode routine address there,
 * then advances; 0x29, 0x2a and 0x2c are the program's entries and 0x33
 * the list index the engine continues from. 0x27 selects a program-memory
 * entry and 0x40 to 0x5f each store one of its six halfwords, 5 down to 0,
 * the sixteen-bit value split as (h & 0x3ff) | (h >> 10) << 23, then the
 * next entry follows (the kernel driver's microcode download and the GL
 * library's program upload use the same words). 0x25 carries a value of
 * unknown meaning, and 0x90808800 with two operands runs a one-shot;
 * neither changes what the model draws, so they are accepted and not kept.
 */
#define ODSY_PROG_SHAPE 0x007ffc00
#define ODSY_PROG_TAG 0x00008000
#define ODSY_PROG_LOW_MASK 0x3ff
#define ODSY_PROG_HIGH_SHIFT 23
#define ODSY_PROG_CLASS_SHIFT 24
/*
 * The colour mode, in the same word shape. The GL library's context
 * default-state setup derives four words from its RGB-mode byte
 * alone and pushes them as classes 0x22, 0x23, 0x24 and 0x25: 0, 2, 0xa,
 * 0xe for an RGBA context and 1, 3, 0xb, 0xf for a colour-index one, so bit
 * 0 of each flips with the mode. The kernel replays them with the other
 * class 0x20 to 0x35 descriptors when it restores a context, and the X
 * server's index and RGB fill branches send the first two
 * (docs/analysis/odyssey-user-stream-arities.md; ESTABLISHED as the emitter
 * and words, docs/reviews/odyssey-gl-demo-fixes-4a9bfa0357.md). That bit 0
 * selects whether a fragment's colour is an index is INFERRED from that
 * split; which of the four the board reads, and the other bits, are
 * UNKNOWN. The model keeps 0x22 and 0x23, reads bit 0 of 0x22 -- one of
 * four words that always agree -- and reports a pair that disagrees.
 */
#define ODSY_PROG_COLOUR_MODE 0x22
#define ODSY_PROG_COLOUR_MODE_B 0x23
#define ODSY_COLOUR_MODE_INDEX 0x1
#define ODSY_PROG_SET_UCODE 0x25
#define ODSY_PROG_PROGRAM_ADDR 0x27
#define ODSY_PROG_CONST_SELECT 0x28
#define ODSY_PROG_ENTRY_29 0x29
#define ODSY_PROG_ENTRY_2A 0x2a
#define ODSY_PROG_ENTRY_2C 0x2c
#define ODSY_PROG_LIGHT_VEC_SELECT 0x31
#define ODSY_PROG_LIGHT_COLOUR_SELECT 0x32
#define ODSY_PROG_LIST_INDEX 0x33
#define ODSY_PROG_PROGRAM_HALFWORD 0x40
#define ODSY_PROG_PROGRAM_HALFWORD_LAST 0x5f
#define ODSY_PROG_CONST_STORE 0x60
#define ODSY_PROG_HALFWORD_HIGH_MASK 0x3f
#define ODSY_PROG_ENTRY_HALFWORDS 6
#define ODSY_PROG_RUN_ONCE 0x90808800        /* 2 operands */
/* gl_entry[] slots. */
enum { ODSY_ENTRY_29, ODSY_ENTRY_2A, ODSY_ENTRY_2C, ODSY_ENTRY_33 };
/* How far a program list is read before it counts as unrecognised. */
#define ODSY_PROG_LIST_MAX 64

/*
 * APPROXIMATION: the model does not run the geometry engine's microcode.
 * It recognises the program the library selected by the routine addresses
 * below and computes OpenGL's lighting itself (odyssey_gl_light()). The
 * addresses are entry points of the geometry-engine microcode IRIX 6.5.30
 * ships, the same image on IP30 and IP35 media (its identity is recorded
 * in the datasheet), as the GL library selects them. They are the minimum
 * the probe's programs and the assembled lit lists need; the library's
 * table of precompiled programs is deliberately not reproduced. A
 * program built from anything else is reported once and drawn unlit, and a
 * different microcode image would need these facts checked again. Outside
 * the recognised set fall the 6.5.30 library's other precompiled lit
 * programs, all textured or texture-generating: they draw unlit with a
 * report (docs/reviews/odyssey-lighting-context-save-ea411c21ca.md, claim 1).
 * A program the library relocates into the program window is recognised by
 * what it uploaded there (odyssey_gl_resident()), then by the same entries.
 */
#define ODSY_UCODE_END 0x4c5         /* the list terminator */
/* The three precompiled programs the probe selects (entries 29, 2a, 2c). */
#define ODSY_UCODE_UNLIT 0x0d6       /* unlit: 0x0d6 0x0e0 0x0ea */
#define ODSY_UCODE_UNLIT_2A 0x0e0
#define ODSY_UCODE_UNLIT_2C 0x0ea
#define ODSY_UCODE_UNLIT_CLIP 0x0f3  /* unlit, clip head: 0x0f3 0x100 0x10c */
#define ODSY_UCODE_UNLIT_CLIP_2A 0x100
#define ODSY_UCODE_UNLIT_CLIP_2C 0x10c
#define ODSY_UCODE_LIT_DIR1 0x5cb    /* one directional light, infinite */
#define ODSY_UCODE_LIT_DIR1_2A 0x5d8 /* viewer, one-sided: 0x5cb 0x5d8 0x5e5 */
#define ODSY_UCODE_LIT_DIR1_2C 0x5e5
/*
 * The precompiled program glfalls selects: two positional lights,
 * GL_NORMALIZE, infinite viewer, one-sided, textured (docs/analysis/odyssey-
 * gl-geometry-program.md, "The precompiled table", and odyssey-gl-demos.md,
 * failure 2). Its entries lie above the resident image, so they only ever
 * reach the board relocated into the program window (0x738 0x760 0x787 in
 * the capture); these are the image addresses the relocation came from.
 * APPROXIMATION, extending the one above: the program is drawn with the
 * model's own lighting equation, not run, and its texturing uses the
 * bounded level-zero shared sampler.
 */
#define ODSY_UCODE_LIT_POS2_TEX 0x9ac
#define ODSY_UCODE_LIT_POS2_TEX_2A 0x9d4
#define ODSY_UCODE_LIT_POS2_TEX_2C 0x9fb
/*
 * The program window (docs/analysis/odyssey-gl-geometry-
 * program.md, "Program memory"): the library copies up to 0xc7 entries of
 * the image into 0x738 .. 0x7fe and relocates the program's addresses at or
 * above its first source entry by 0x738 minus that entry.
 */
#define ODSY_PROG_WINDOW_BASE 0x738
#define ODSY_PROG_WINDOW_ENTRIES SGI_ODYSSEY_GL_WINDOW_ENTRIES
/*
 * The assembled unlit list: its head, then 0x4bb in texture mode or 0x4c0
 * ("The assembled list", lighting off).
 */
#define ODSY_UCODE_HEAD_UNLIT 0x4ae
#define ODSY_UCODE_UNLIT_TEXTURE 0x4bb
#define ODSY_UCODE_UNLIT_FINISH 0x4c0
/* Assembled lit list heads: the viewer and the first light's kind. */
#define ODSY_UCODE_HEAD_INF_DIR 0x290
#define ODSY_UCODE_HEAD_INF_POS 0x272
#define ODSY_UCODE_HEAD_LOCAL_DIR 0x281
#define ODSY_UCODE_HEAD_LOCAL_POS 0x263
/* The normal transform without and with GL_NORMALIZE. */
#define ODSY_UCODE_NORMAL 0x2b6
#define ODSY_UCODE_NORMALIZE 0x29c
/*
 * Per-light fragments: a further directional or positional light, and a
 * light's unattenuated or attenuated term. 0x300 also opens the finish
 * after an attenuated or spot last light.
 */
#define ODSY_UCODE_LIGHT_DIR 0x300
#define ODSY_UCODE_LIGHT_POS 0x398
#define ODSY_UCODE_LIGHT_PLAIN 0x2cf
#define ODSY_UCODE_LIGHT_ATTEN 0x2de
/* The lighting finish: one-sided, two-sided and texture-mode variants. */
#define ODSY_UCODE_FINISH_A 0x3cf
#define ODSY_UCODE_FINISH_A_TWO_SIDED 0x3e5
#define ODSY_UCODE_FINISH_A_TEXTURE 0x3da
#define ODSY_UCODE_FINISH_ONE_SIDED 0x3f5
#define ODSY_UCODE_FINISH_TWO_SIDED 0x415
#define ODSY_UCODE_FINISH_TEXTURE 0x40b
#define ODSY_UCODE_END_ONE_SIDED 0x49a
#define ODSY_UCODE_END_TWO_SIDED 0x49f
/* The list's tail: the one-shot's routine and the one after it. */
#define ODSY_UCODE_TAIL_ONE_SHOT 0x203
#define ODSY_UCODE_TAIL 0x25e
/*
 * A hardware clip-plane stage is a 0 word followed by one of these
 * (odyssey-gl-geometry-program.md, "The assembled list", hardware clip
 * planes).
 */
#define ODSY_UCODE_CLIP 0x00c
#define ODSY_UCODE_CLIP_TEXTURED 0x01a
#define ODSY_UCODE_CLIP_TEXGEN 0x028

/*
 * Texture state (docs/analysis/odyssey-gl-texturing.md, sections 1 and 2;
 * ESTABLISHED as the words and the GL library's emitters). Nothing is pushed
 * for glTexParameter, glTexEnv or glEnable(GL_TEXTURE_2D) at once; the next
 * draw's validation pushes:
 *
 * - 0x0405 bit 3/4: the observed 1D/2D texture mode. Bits 2:0 are
 *   unrelated: the user clip-plane enable count (below).
 * - 0x1c16, seven operands: the sampler (log2 sizes, width, height, the
 *   component code, wrap modes and filters).
 * - 0x141b, five operands: the texture environment (replace, modulate,
 *   decal, blend, add) and its colour.
 * - 0x0e80 + i, three operands each, 64 entries: the level table, each a
 *   level's object base, a format-and-width word and 0.
 * - 0x0406: the table index of level 0.
 * - 0xc380c41f and one float, or 0xc380c821 and two floats, before a
 *   vertex: its texture coordinate, untransformed; the board applies the
 *   texture matrix (slot 0x14).
 *
 * The model resolves reviewed 2D level chains through the shared rasterizer
 * once per synchronous draw. Unknown sampler words remain observable and
 * drop the draw. 0x8f80b968 requests on-board construction; the reviewed
 * template copies source (2x, 2y) to destination (x, y). The board's filter
 * is unknown, so other templates remain reported and dropped.
 */
#define ODSY_REG_TEX_MODE 0x0405        /* 1 operand */
#define ODSY_TEX_MODE_1D (1u << 3)
#define ODSY_TEX_MODE_2D (1u << 4)
#define ODSY_TEX_MODE_3D (1u << 5)
#define ODSY_REG_TEX_SAMPLER 0x1c16     /* 7 operands */
#define ODSY_REG_TEX_ENV 0x141b         /* 5 operands */
#define ODSY_REG_TEX_LEVEL 0x0e80       /* 3 operands, 0x0e80 .. 0x0ebf */
#define ODSY_REG_TEX_LEVEL0 0x0406      /* 1 operand */
#define ODSY_REG_TEX_LOD 0x1013        /* 4 operands */
#define ODSY_REG_TEX_COLOUR_TABLE0 0x3e00 /* 16 blocks of 16 words */
#define ODSY_REG_TEX_COLOUR_TABLE1 0x3e10
#define ODSY_TEX_COLOUR_TABLE_BLOCKS 16
#define ODSY_TEX_COLOUR_TABLE_BLOCK_WORDS 16
#define ODSY_GL_TEXCOORD_1F 0xc380c41f  /* 1 operand: s */
#define ODSY_GL_TEXCOORD_2F 0xc380c821  /* 2 operands: s, t */
#define ODSY_GL_TEXCOORD_3F 0xc380cc23  /* 3 operands: s, t, r */
#define ODSY_GL_TEXCOORD_SHAPE 0xffffff00
#define ODSY_GL_TEXCOORD_TAG 0xc380c800
#define ODSY_GL_TEX_RECT 0x8f80b968     /* 14 operands */

/* Six retained words: colour, scale, bias and cubic coefficients. */
#define ODSY_REG_FOG 0x181a
#define ODSY_MODE_FOG (1u << 22)

/*
 * The board-side geometry transform (docs/analysis/odyssey-gl-triangle-
 * stream.md, "the geometry transform is board-side"). A matrix target-select
 * word (0 operands, its own tag rather than the register-select shape) names
 * the start index of a block in its low bits, then the matrix-load word
 * carries 16 IEEE-754 floats as a ROW-major 4x4. The GL library's four
 * matrix validations each select one block and load it:
 *
 *   0x00  the combined matrix: the projection composed with the window
 *         (viewport) map, the combined-matrix validation.
 *   0x14  the texture matrix, the texture-matrix validation.
 *   0x24  the model-view matrix, the model-view validation.
 *   0x34  the normal transform, the normal-matrix validation.
 *
 * So the board never receives the projection on its own: it arrives folded
 * into slot 0x00 (odyssey_gl_load_combined()). The texture matrix transforms
 * ST before clipping. The normal transform is
 * kept for the family's own lighting (odyssey_gl_light()). A load aimed at
 * any other block is directed at a GL matrix mode the shared context refuses
 * (ODSY_GL_SLOT_NORMAL), which keeps every access observable
 * (docs/designs/odyssey-gl-family-side.md, resolution of gap 1).
 *
 * The select is matched by shape rather than by the exact words the analysis
 * saw, because it is decode-neutral for the 2D greeter: a select carries no
 * operands, so recognizing one consumes exactly what dropping it as an
 * unmodelled word did, and its only effect is the internal slot choice a later
 * load reads. The load is matched by its exact word, so no word the greeter's
 * 2D stream might carry is ever consumed differently than before.
 */
#define ODSY_GL_MATRIX_SELECT_SHAPE 0xffffff00
#define ODSY_GL_MATRIX_SELECT_TAG 0x34008000
#define ODSY_GL_MATRIX_SELECT_COMBINED 0x00    /* the select word's low byte */
#define ODSY_GL_MATRIX_SELECT_TEXTURE 0x14
#define ODSY_GL_MATRIX_SELECT_MODELVIEW 0x24
#define ODSY_GL_MATRIX_LOAD 0x9080bdc8
#define ODSY_GL_MATRIX_FLOATS 16
/*
 * The GL library's combined-matrix validation follows each slot-0x00 load
 * with a select of index 16 and this word carrying four floats
 * (ESTABLISHED): (-0, -0, -0, 1)
 * under the identity projection, (-0, -0, -0, -1) under glOrtho(-1, 1, -1,
 * 1, -1, 1) and (-0, -0, -0, -3) under glFrustum(-1, 1, -1, 1, 1, 3). Only
 * the fourth float's sign has an observed role (odyssey_gl_load_combined());
 * the rest are kept and unread.
 */
#define ODSY_GL_MATRIX_SELECT_BLOCK16 0x10
#define ODSY_GL_LOAD_4F 0x908091df
#define ODSY_GL_BLOCK16_FLOATS 4
#define ODSY_GL_BLOCK16_SIGN 3
/* The select's own slot, so the four-float load knows its target. */
#define ODSY_GL_SLOT_BLOCK16 0x10002
/* The normal transform's slot (ODSY_GL_MATRIX_SELECT_NORMAL). */
#define ODSY_GL_SLOT_NORMAL_MATRIX 0x10003
/*
 * The board's depth unit: the client's depth d in [0, 1] is d x 2S with
 * S = 2^32 - 2^14. ESTABLISHED: the clear's z operand is 0x4fffffc0
 * (8589901824 = 2S) for glClearDepth(1), 0x4effffc0 for 0.25 and 0 for 0,
 * and the GL library's glClear multiplies by the double 8589901824.0.
 */
#define ODSY_GL_DEPTH_S 4294950912.0
#define ODSY_F32_ONE 0x3f800000
#define ODSY_F32_POINT_TWO 0x3e4ccccd
#define ODSY_F32_POINT_EIGHT 0x3f4ccccd
#define ODSY_DEPTH24_MAX 0xffffffu
/* A GL matrix mode the shared context does not keep (neither modelview nor
 * projection), so sgi_gl_set_matrix() logs and refuses a load aimed at it. */
#define ODSY_GL_SLOT_NORMAL 0x1702
/* This model's own slots for the combined and texture matrices. */
#define ODSY_GL_SLOT_COMBINED 0x10000
#define ODSY_GL_SLOT_TEXTURE 0x10001
/*
 * Half the extent of the fixed viewport the shared context draws through
 * (odyssey_gl_load_combined()): window coordinates within +-8192 pixels,
 * the 13-bit reach of the window origin, pass unclipped.
 */
#define ODSY_GL_WINDOW_REACH 8192

/*
 * The screen-aligned rectangle (docs/analysis/odyssey-gl-clear-stream.md).
 * ESTABLISHED from the IRIX 6.5.30 GL library's glClear implementation
 * and its stippled-rectangle emitter: inside BEGIN 7 the client sends
 * this word and ten floats -- R, G, B, A, then two opposite corners
 * (x, y, z) in window pixels -- and closes with END. glClear of the colour
 * buffer is exactly this, the corners (0, 0) and (width, height). INFERRED:
 * the board fills the axis-aligned rectangle between the corners, the upper
 * edges exclusive (the client passes x and x + width). The stipple the
 * second emitter can enable, the depth the corners carry and every other
 * fragment operation are not modeled; the fill is the plain colour.
 */
#define ODSY_GL_RECT 0x8f80a944
#define ODSY_GL_RECT_FLOATS 10
/* The identity the shared GL context keys draws by; this board has one. */
#define ODSY_GL_CONTEXT_HW_ID 1

/*
 * odyssey_copyrect()'s command is a fixed 8-word literal template
 * following a bare trigger, not decomposed through the generic
 * register-select mechanism -- three of the eight words are runtime
 * source/size/dest slots, the rest constant. 0x00010658 (ODSY_REG_
 * BLIT_PREFIX, above) is an ordinary 1-operand register ahead of the real
 * sequence, not the trigger: the model used to key off it directly, a
 * fixed 10-word blit state that swallowed the DMA-sync command
 * (ODSY_REG_HOST_WRITE) whenever it followed a copy in the real command
 * stream, since 0x658's own operand word left the state machine still
 * expecting nine more literal words. The true trigger, corroborated by
 * both docs/research/graphics/odyssey-xsgi-2d-command-grammar-20260922.md
 * and docs/datasheets/odyssey.md's command-port table, is 0x00002031.
 */
#define ODSY_CMD_COPY_RECT_TRIGGER 0x00002031
#define ODSY_CMD_COPY_RECT_WORDS 8
#define ODSY_CMD_COPY_RECT_SRC_INDEX 1
#define ODSY_CMD_COPY_RECT_SIZE_INDEX 3
#define ODSY_CMD_COPY_RECT_DST_INDEX 6
#define ODSY_COPY_RECT_SOURCE_SHIFT 12
#define ODSY_COPY_RECT_DEST_SHIFT 20
#define ODSY_COPY_RECT_FORMAT_SOURCE_SHIFT 24

/*
 * The one-bit pattern-quad text path (docs/analysis/odyssey-glyph-raster-
 * format.md). The greeter draws every text label as a stipple: it loads a
 * 32x32-bit pattern, selects a shape, turns pattern mode on, then draws the
 * string's bounding rectangle as a QUADS fill. Where a pattern bit is set the
 * quad's pixel is painted in the current colour; where it is clear nothing is
 * drawn. All four are ordinary register-select words whose operand count the
 * bit-field arity rule already gives: 0x3c4e/0x3c4f carry a 16-word burst
 * (buffer words 0-15 and 16-31), 0x043f and 0x0404 one operand each.
 */
#define ODSY_REG_PATTERN_LOW 0x3c4e   /* pattern buffer words 0-15 */
#define ODSY_REG_PATTERN_HIGH 0x3c4f  /* pattern buffer words 16-31 */
#define ODSY_REG_PATTERN_SHAPE 0x043f /* 1 operand: 0x4/0x5/0x6, 0x0 = off */
#define ODSY_REG_PATTERN_MODE 0x0404  /* 1 operand: pattern, GL mode bits */
#define ODSY_PATTERN_WORDS 16

/*
 * The line stipple (docs/analysis/odyssey-account-icons.md). The X
 * server's stippled-span routine pushes 0x0c48 before each stippled span
 * with three operands: the stipple row's high 16 bits, the whole 32-bit
 * row rotated to the span, and
 * (1 << 9) | 0x100 | (stipple width - 17), 0x30f for the traced 32-pixel
 * stipples. With 0x0404 bits 3 and 4 set, a line paints its k-th covered
 * pixel from the first vertex (k = 1 next to it) only where bit (k & 31) of
 * the 32-bit operand is set: ESTABLISHED against all 798 traced stippled
 * spans, which then form one screen-aligned checkerboard. The 16-bit operand,
 * the third operand's fields and whether the board repeats at 16 or 32 bits
 * are unknown. Float GL lines use a separate reviewed software encoding:
 * bit 3 enables a 16-bit pattern from word 0, repeated word 2 low byte + 1
 * times per bit. The two paths keep their own pattern and phase rules.
 */
#define ODSY_REG_LINE_STIPPLE 0x0c48    /* 3 operands */
#define ODSY_MODE_GL_LINE_STIPPLE (1u << 3)
#define ODSY_LINE_STIPPLE_PATTERN 1     /* the 32-bit rotated row */
#define ODSY_LINE_STIPPLE_CONTROL 2
#define ODSY_LINE_STIPPLE_CONTROL_32 0x30f

/*
 * The drawable-id plane write (docs/analysis/odyssey-account-panel-black-
 * band.md). The X server's drawable-id plane write assigns a window's
 * screen area to its drawable by writing the board's id planes rather
 * than the colour planes: it sends a 0x1017 destination-select block
 * whose third operand is the constant
 * ODSY_DEST_ID_PLANES, a 0x1023 block whose first operand is the plane write
 * mask (mask << 12), a COLOR_INDEX word carrying the raw id value (id << 3),
 * and a QUADS rectangle. A colour fill's 0x1017 third operand instead carries
 * a value derived from the drawable's pixel format (0x20086a00 colour-index,
 * 0x20006950 RGB), so the constant discriminates the two. Both the 0x1017
 * third operand and the 0x142000 first operand travel together for this
 * purpose in every observed case; which the hardware actually keys on is not
 * settled, so this keys on the third operand as the note recommends.
 */
#define ODSY_REG_DEST_SELECT 0x1017 /* 4 operands: destination-buffer select */
#define ODSY_REG_PLANE_MASK 0x1023  /* 4 operands: plane write mask << 12 */
#define ODSY_DEST_ID_PLANES 0x20080080 /* 0x1017 third word: write id planes */
#define ODSY_DEST_SELECT_VALUE_INDEX 2 /* the third operand */
#define ODSY_DEST_SELECT_BASE_INDEX 0  /* the buffer's base, 0x2000 */
#define ODSY_PLANE_MASK_SHIFT 12       /* the 0x1023 operand holds mask << 12 */

/*
 * The 0x043f shape operand. 0x5 is 64 wide by 16 rows (two words per row),
 * 0x4 is 32 by 32 (one word per row), 0x6 is 16 by 64 (two rows per word).
 * Bit 31 of a row's first word is the leftmost pixel and row 0 is the top row;
 * row 0 column 0 lands on the quad's top-left vertex. Any other value, 0x0
 * included, means no pattern is in effect.
 */
#define ODSY_PATTERN_SHAPE_64X16 0x5
#define ODSY_PATTERN_SHAPE_32X32 0x4
#define ODSY_PATTERN_SHAPE_16X64 0x6

/*
 * The 0x0404 mode operand's pattern-enable bits: the DDX sets bits 3 and 4
 * together (operands 0x80018 / 0x100018) immediately before a pattern load,
 * and clears them before every solid fill and image operation. Which bit does
 * what is unknown, so both must be set to enable. (The 0x043f operand's own
 * bit 4 -- opaque mode, painting a background colour where a pattern bit is 0
 * -- never appears in the login trace and is left unimplemented.)
 */
#define ODSY_PATTERN_MODE_ENABLE 0x18
/* Proposed shape-4 screen-aligned rectangle policy. */
#define ODSY_GL_RECT_PATTERN_ENABLE (1u << 4)

/*
 * The GL library keeps the shade model in the same register
 * (docs/analysis/odyssey-gl-shading-depth-lighting.md): the GL library's
 * glShadeModel merges (mode == GL_FLAT) << 12 into its 0x0404 shadow,
 * and the captured smooth and flat triangles differ only in this bit,
 * each sending its own
 * colour before every vertex (ESTABLISHED). Set means GL_FLAT, clear
 * GL_SMOOTH.
 */
#define ODSY_MODE_FLAT_SHADE (1u << 12)
/*
 * Bit 18 of the same register is GL_DITHER (docs/analysis/odyssey-gl-
 * dither.md, ESTABLISHED): glEnable/glDisable(GL_DITHER) set and clear it in
 * the library's 0x0404 shadow, a new context has it on, the library clears
 * it around its own clears, and the X server's word keeps it off.
 */
#define ODSY_MODE_DITHER (1u << 18)
/*
 * Bit 14 of the same register is GL_ALPHA_TEST
 * (docs/reviews/odyssey-fragment-state-evidence-20261001.md): glEnable/
 * glDisable(GL_ALPHA_TEST) set and clear it in the library's 0x0404 shadow.
 * 0x041e carries the function and reference together in one word: bits 14:12
 * the comparison, GL_NEVER = 0 to GL_ALWAYS = 7, and bits 11:0 the reference
 * under the common 12-bit rule (value / 4096.0, the library's own truncated
 * encoding of its float reference).
 */
#define ODSY_MODE_ALPHA_TEST (1u << 14)
#define ODSY_REG_ALPHA_FUNC 0x041e      /* 1 operand */
#define ODSY_ALPHA_FUNC_SHIFT 12
#define ODSY_ALPHA_FUNC_MASK 0x7
#define ODSY_ALPHA_REF_MASK 0xfff
#define ODSY_ALPHA_REF_SCALE 4096.0f

/*
 * Bit 17 of the same register is a computed enable, not a plain mirror of
 * glEnable(GL_BLEND): the library sets it when blending is on and colour
 * logic op, bit 20 below, is off (docs/reviews/odyssey-fragment-state-
 * evidence-20261001.md). The library also rewrites 0x0c21 to all zeros
 * whenever this bit is clear, so a draw must not apply that register's
 * factors, equation or colour while the bit is clear -- doing so would
 * overwrite the real blend state with the library's parked zeros.
 */
#define ODSY_MODE_BLEND (1u << 17)
#define ODSY_REG_BLEND 0x0c21        /* 3 operands */
#define ODSY_BLEND_DST_MASK 0x3f
#define ODSY_BLEND_SRC_SHIFT 6
#define ODSY_BLEND_SRC_MASK 0x7f
#define ODSY_BLEND_EQ_SHIFT 13
#define ODSY_BLEND_EQ_MASK 0x7
#define ODSY_BLEND_COLOUR_MASK 0xfff
#define ODSY_BLEND_COLOUR_SHIFT 12
#define ODSY_BLEND_COLOUR_SCALE 4096.0f

/*
 * Bits 19 and 20 of the same register are GL_INDEX_LOGIC_OP and
 * GL_COLOR_LOGIC_OP. 0x0422 carries the mode either enable feeds the
 * shared layer, GL_CLEAR (0) to GL_SET (15) recovered as op - GL_CLEAR.
 */
#define ODSY_MODE_INDEX_LOGIC_OP (1u << 19)
#define ODSY_MODE_COLOR_LOGIC_OP (1u << 20)
#define ODSY_REG_LOGIC_OP 0x0422        /* 1 operand */
#define ODSY_LOGIC_OP_MASK 0xf

/*
 * Bit 15 of the same register is GL_STENCIL_TEST
 * (docs/reviews/odyssey-fragment-state-evidence-20261001.md), conditional on
 * the drawable's stencil planes. This model has none (the shared rasterizer's
 * stencil pipeline is wired for no visual here), and absent a stencil buffer
 * OpenGL's own stencil test is defined to always pass, so feeding the enable
 * is decoding real GL state, not drawing with a stencil test that has any
 * effect. 0x041f carries the function, reference and value mask together:
 * bits 27:25 the comparison, GL_NEVER = 0 to GL_ALWAYS = 7, bits 7:0 the
 * reference clamped to [0, 2^s - 1], bits 15:8 the value mask ANDed with
 * 2^s - 1, and bits 24:22, 21:19, 18:16 the fail, depth-fail and depth-pass
 * operations: KEEP 0, ZERO 1, REPLACE 2, INCR 3, DECR 4, INVERT 5. 0x452
 * bits 19:12 are the write mask under the same (2^s - 1) rule, forced 0
 * without stencil planes.
 */
#define ODSY_MODE_STENCIL_TEST (1u << 15)
#define ODSY_REG_STENCIL_FUNC 0x041f    /* 1 operand */
#define ODSY_STENCIL_FUNC_SHIFT 25
#define ODSY_STENCIL_FUNC_MASK 0x7
#define ODSY_STENCIL_REF_MASK 0xff
#define ODSY_STENCIL_VALUE_MASK_SHIFT 8
#define ODSY_STENCIL_VALUE_MASK_MASK 0xff
#define ODSY_STENCIL_OP_FAIL_SHIFT 22
#define ODSY_STENCIL_OP_ZFAIL_SHIFT 19
#define ODSY_STENCIL_OP_ZPASS_SHIFT 16
#define ODSY_STENCIL_OP_MASK 0x7
#define ODSY_WRITE_STENCIL_SHIFT 12
#define ODSY_WRITE_STENCIL_MASK 0xff

/*
 * 0x452 bits 11:0 are glIndexMask ANDed with the drawable's index-plane mask
 * (docs/reviews/odyssey-fragment-state-evidence-20261001.md): every retained
 * capture is RGBA, where the field is always 0 because an RGBA drawable has
 * no index planes, so no capture shows what a real colour-index drawable
 * sends here. This board's own reset default for the word also leaves these
 * bits 0, a value every colour-index draw already relies on reading as
 * unmasked (odyssey_gl_store_colour()). Applying the bits as a literal write
 * mask would make that the ordinary, unconfigured case instead of the
 * exception, which no known guest does -- so the bits are decoded and a
 * non-default value is reported, not applied.
 */
#define ODSY_WRITE_INDEX_MASK 0xfff

/*
 * Bit 8 of the 0x0404 mode register is GL_CULL_FACE, bits 10:9 the face it
 * culls -- 01 GL_BACK, 10 GL_FRONT, 11 GL_FRONT_AND_BACK, with 00 never
 * sent by a probe word -- and bit 23 glFrontFace's orientation, literally
 * set for GL_CW and clear for GL_CCW
 * (docs/reviews/odyssey-fragment-state-evidence-20261001.md).
 *
 * Bit 23's literal sense is NOT the sense fed to the shared layer. The
 * board's own established window transform (docs/datasheets/odyssey.md,
 * "window transform") bakes y' = -(vh/2) y + (h - vy - vh/2 - 1/256) into
 * the ordinary projection matrix odyssey_gl_load_combined() loads, rather
 * than routing through sgi_gl_set_window_matrix()'s window_rows_down path.
 * A single-axis mirror reverses 2D winding exactly once, and the shared
 * header documents that path's own row-direction flip as leaving "facing
 * ... judged as the client sees it" (include/hw/display/sgi/gl-context.h),
 * i.e. window_rows_down carries its own compensation for the mirror it
 * introduces. Odyssey's baked mirror has no such compensation unless this
 * family supplies one, so the literal bit is inverted before it reaches the
 * shared glFrontFace state: register GL_CW feeds SGI_GL_CCW, and register
 * GL_CCW feeds SGI_GL_CW. This cancels the baked mirror's sign flip so the
 * shared rasterizer's front/back test again matches the orientation a
 * client submits.
 *
 * This inversion is this implementation's own derivation from the above
 * citations, not a literal statement in the evidence review, and has not
 * had independent review. It is consistent with the only empirical check
 * available, the proposal's own test 6
 * (docs/reviews/odyssey-fragment-state-proposal-20261001.md, not amended by
 * the review): a CCW triangle with front LINE, back FILL draws an outline,
 * and reversing the winding or setting bit 23 draws a fill. The underlying
 * hardware contract for this bit's sense needs independent evidence review
 * before being treated as settled.
 */
#define ODSY_MODE_CULL_FACE (1u << 8)
#define ODSY_MODE_CULL_MODE_SHIFT 9
#define ODSY_MODE_CULL_MODE_MASK 0x3
#define ODSY_MODE_FRONT_FACE_CW (1u << 23)

/*
 * Bits 5, 6 and 7 of the same register are GL_POLYGON_OFFSET_POINT, _LINE
 * and _FILL (docs/reviews/odyssey-fragment-state-evidence-20261001.md):
 * glEnable/glDisable set and clear them in the library's 0x0404 shadow.
 * 0x0458 carries the constant term alone -- bits 24:0 a signed value in
 * steps of 2^-24 of window depth, with no slope term -- so it is fed as a
 * factor of 0 and this bias via SGI_GL_OP_POLYGON_OFFSET_EXT.
 */
#define ODSY_MODE_OFFSET_POINT (1u << 5)
#define ODSY_MODE_OFFSET_LINE (1u << 6)
#define ODSY_MODE_OFFSET_FILL (1u << 7)
#define ODSY_REG_POLYGON_OFFSET 0x0458 /* 1 operand */
#define ODSY_POLYGON_OFFSET_BITS 25
#define ODSY_POLYGON_OFFSET_SCALE (1.0f / (1 << 24))

/*
 * 0x0415 packs glPolygonMode's two faces in one word
 * (docs/reviews/odyssey-fragment-state-evidence-20261001.md): bits 3:2 the
 * front face, bits 1:0 the back face, each GL_POINT 0, GL_LINE 1, GL_FILL 2;
 * the board's reset value 0xa decodes to FILL for both. Fed as two separate
 * SGI_GL_OP_POLYGON_MODE calls, one per face.
 */
#define ODSY_REG_POLYGON_MODE 0x0415 /* 1 operand */
#define ODSY_POLYGON_MODE_FRONT_SHIFT 2
#define ODSY_POLYGON_MODE_BACK_SHIFT 0
#define ODSY_POLYGON_MODE_MASK 0x3

/*
 * 0x084a packs the line width and point size together
 * (docs/reviews/odyssey-fragment-state-evidence-20261001.md): two operands,
 * line width then point size, each a 24-bit unsigned field (bits 23:0) read
 * as size = field / 128.0, the fixed-point convention this register file
 * uses elsewhere.
 */
#define ODSY_REG_LINE_POINT_SIZE 0x084a /* 2 operands: line width, point size */
#define ODSY_LINE_POINT_SIZE_MASK 0xffffff
#define ODSY_LINE_POINT_SIZE_SCALE 128.0f

/*
 * The scissor and window clip box, 0x081d, two operands
 * (docs/reviews/odyssey-fragment-state-evidence-20261001.md): word 0
 * (x_min << 12) | x_max, word 1 (y_min << 12) | y_max, each field 12 bits,
 * inclusive, relative to the window origin with y counted in rows down from
 * the window's top edge. The library always sends the intersection of the
 * window, the viewport and, while GL_SCISSOR_TEST is on, the scissor box; an
 * empty intersection arrives as minimum 1, maximum 0. Fed to the shared
 * layer as SGI_GL_OP_SCISSOR's x, y, width, height taken directly with no
 * flip: x = x_min, width = x_max - x_min + 1, y = y_min, height =
 * y_max - y_min + 1, clamped to 0 rather than negative for an empty box.
 * This family never sets the shared context's window_rows_down flag --
 * odyssey_gl_load_combined() loads the client's own combined matrix through
 * a fixed viewport centred on the window origin, which already makes the
 * ordinary projection/viewport math come out top-down -- so "no flip" here
 * means the direct y = y_min mapping, not that literal flag. Confirmed
 * against the evidence review's own worked example: a box of x 0-127, y
 * 0-7 is observed to affect only the top eight rows of the window, which
 * only the direct mapping predicts. Whether the board's own row count
 * matches the library's top-down box is inferred, not directly observed.
 *
 * No probe ever shows a 0x404 enable bit for GL_SCISSOR_TEST: the library
 * keeps the three rectangles intersected into 0x81d whether or not
 * scissoring is on, so there is no guest-visible toggle for a model to key
 * on. The shared context's scissor test is instead left permanently enabled
 * from reset onward (odyssey_reset_registers()), with the box defaulting to
 * the full SGI_ODYSSEY_WIDTH x SGI_ODYSSEY_HEIGHT window so a guest that
 * never writes 0x81d draws exactly as it did before this register existed.
 */
#define ODSY_REG_CLIP_BOX 0x081d /* 2 operands */
#define ODSY_CLIP_BOX_MIN_SHIFT 12
#define ODSY_CLIP_BOX_FIELD_MASK 0xfff

/*
 * User clip planes, class 0x31 index 0x120 + 4j
 * (docs/reviews/odyssey-fragment-state-evidence-20261001.md), reusing the
 * same index-select field as the light-vector loads
 * (ODSY_PROG_LIGHT_VEC_SELECT, gl_light_vec_sel): the slot select word is
 * followed by ODSY_GL_CLIP_PLANE_LOAD and the plane's four eye-space floats,
 * a, b, c, d, passed through to SGI_GL_OP_CLIP_PLANE unchanged -- the shared
 * layer transforms by the inverse modelview itself. The library loads every
 * enabled plane, user planes first and then its own two internal
 * depth-range planes, into consecutive slots starting at 0x120, including
 * slots beyond the hardware's six-plane budget: a model accepts those loads
 * and leaves the ones past SGI_GL_CLIP_PLANES silently unmodeled, since the
 * shared context has no storage for them and this is expected library
 * traffic, not a guest error.
 *
 * Which of the stored planes are actually tested is a separate question,
 * answered by 0x0405 bits 2:0 (the same register as ODSY_TEX_MODE_2D): a
 * three-bit thermometer count n of enabled planes -- bit 2 set for n > 0,
 * bit 1 for n > 1, bit 0 for n > 2 -- recomputed on every 0x405 write, planes
 * 0..n-1 fed SGI_GL_OP_ENABLE and the rest SGI_GL_OP_DISABLE. Because the
 * budget caps at 3, slots 3 to 5 -- which can still receive the library's
 * two internal depth-range planes -- can never be enabled through this
 * field, so they never clip anything in this model; the datasheet's "leave
 * depth_range at [0, 1]" is consistent with that.
 */
#define ODSY_GL_CLIP_PLANE_BASE 0x120
#define ODSY_GL_CLIP_PLANE_STRIDE 4
#define ODSY_TEX_MODE_CLIP_COUNT_MASK 0x7

/*
 * The BEGIN-0xa image raster path: the greeter's account icons (the pixmaps
 * above root/EZsetup/demos/guest) and the text-field caret
 * (docs/analysis/odyssey-glyph-raster-format.md). BEGIN with primitive 0xa
 * opens an image; its destination box is the 0xcf80a92f record's floats 4, 5
 * (top-left) and 7, 8 (exclusive bottom-right). A single (0, 0) vertex and a
 * transfer header follow, then the pixel bursts. These structural words are
 * not register-selects, so they are recognised by value; the state word
 * 0x29008000 | v (the transfer trigger and its teardown twin) carries its own
 * value in the low 10 bits. The pixel bursts are register-select words the
 * generic arity rule already sizes, routed to the painter while an image is
 * open.
 */
#define ODSY_CMD_STATE_WORD 0x29008000
#define ODSY_CMD_STATE_WORD_MASK 0xfffffc00
#define ODSY_CMD_IMAGE_BOX 0xcf80a92f         /* 10 operands: raster-box floats */
#define ODSY_CMD_IMAGE_HEADER 0x00004570      /* 1 operand: transfer header */
#define ODSY_CMD_IMAGE_HEADER_TAIL 0x00000071 /* 0 operands */

/* BEGIN's operand for an image (as opposed to 0x7, QUADS). */
#define ODSY_GL_PRIMITIVE_IMAGE 0xa

/*
 * The floats 4, 5, 7, 8 of the 0xcf80a92f box record are the destination
 * corners; the full layout is [0, 0, 0, 0, x1, y1, 0, x2, y2, 0].
 */
#define ODSY_IMAGE_BOX_X1_INDEX 4
#define ODSY_IMAGE_BOX_Y1_INDEX 5
#define ODSY_IMAGE_BOX_X2_INDEX 7
#define ODSY_IMAGE_BOX_Y2_INDEX 8

/*
 * A pixel burst is 0x00014000 | (words - words/16) << 10 | fmt: bit 14 set,
 * the arity in bits 13:10, and the format in the low bits, with bits 9:5
 * clear. Format 0x11 carries one 0x00RRGGBB pixel per word for RGB
 * sources, or one RRGGBBAA word for source code 0x6b. It also carries one
 * 0x00RRGGBB word for the account icons. Format 0x10 packs four colour
 * indices per word; format 0x12 carries two RGBA16 words per pixel for
 * source 0x6b. Other formats remain unsupported.
 */
#define ODSY_IMAGE_BURST_BASE 0x4000
#define ODSY_IMAGE_BURST_FMT_MASK 0x1f
#define ODSY_IMAGE_BURST_RESERVED_MASK 0x3e0
#define ODSY_IMAGE_FMT_CI 0x10
#define ODSY_IMAGE_FMT_RGB 0x11
#define ODSY_IMAGE_FMT_RGBA16 0x12

/*
 * The source pixel format: the transfer header's low byte, and the same code
 * in the low byte of a DMA header's word 7 (docs/analysis/odyssey-gl-
 * texturing.md, section 3; ESTABLISHED from the X server's icons and caret
 * and the GL library's texture uploads). All are big-endian byte streams:
 * 0x4c one 0x00RRGGBB word a pixel, 0x4b one 0xRRGGBBAA word, 0x49 three
 * bytes R, G, B, 0x42 one luminance byte, 0x41 one colour-index byte. The
 * burst tag's own format bits (0x10, 0x11) do not fix the layout: the X
 * server's 0x00RRGGBB icon words and the library's RRGGBBAA texels share
 * the tag 0x7c11.
 */
#define ODSY_IMAGE_CODE_MASK 0xff
#define ODSY_IMAGE_CODE_CI 0x41
#define ODSY_IMAGE_CODE_LUM 0x42
#define ODSY_IMAGE_CODE_LUM_ALPHA 0x48
#define ODSY_IMAGE_CODE_RGB 0x49
#define ODSY_IMAGE_CODE_RGBA 0x4b
#define ODSY_IMAGE_CODE_XRGB 0x4c
#define ODSY_IMAGE_CODE_RGBA12 0x6b

/* The transfer header's group length, bits 23:20, with 15 standing for 16. */
#define ODSY_IMAGE_HEADER_GROUP_SHIFT 20
#define ODSY_IMAGE_HEADER_GROUP_MASK 0xf
#define ODSY_IMAGE_HEADER_GROUP_FULL 16

/*
 * How many operand words cmd_operand[] retains: every one a register-select
 * word can carry, up to the 16 of a burst. The window-clip register reads all
 * eight of its operands. Before that register was decoded only five were
 * kept, and a migration stream from then carries only those five.
 */
#define ODSY_CMD_MAX_OPERANDS SGI_ODYSSEY_CMD_OPERANDS
#define ODSY_CMD_OLD_OPERANDS 5

/*
 * Window placement (docs/analysis/odyssey-gl-window-placement.md). A GL
 * client's vertices and clear rectangle are window-relative; the kernel tells
 * the board where the current context's window is, after every context switch
 * into it and every clip change, with three register-select words on either
 * command port:
 *
 * - 0x841, two operands: the screen x and y of the window's top-left pixel in
 *   bits 12:0 of each (ESTABLISHED encoding; the kernel masks 0x1fff and
 *   preserves the upper bits, which are ignored here). Treated as a signed
 *   13-bit field, because a window partly off the left or top edge has a
 *   negative origin and the kernel clamps only the rectangles (INFERRED).
 * - 0x204b, eight operands: four clip rectangles in screen pixels, two words
 *   each, x0 << 12 | y0 then x1 << 12 | y1, inclusive, 12-bit fields
 *   (ESTABLISHED). A rectangle whose minimum exceeds its maximum is empty; the
 *   kernel marks unused ones (1, 1) .. (0, 0). The top byte is preserved by
 *   the kernel and ignored here.
 * - 0x840, two operands: the first is a constant 0x142000 whose meaning is
 *   unknown; the second is the drawable-id compare word, bit 16 enabling it,
 *   bits 15:8 the id-plane mask and bits 7:0 the value (ESTABLISHED as the
 *   kernel builds it). No capture enables it.
 *
 * The board adds the origin to window coordinates and keeps only pixels inside
 * one of the rectangles and, with the compare enabled, whose id plane matches
 * (INFERRED: the only reading under which the client's window-relative clear
 * and the kernel's screen rectangle describe the same window). Both ports
 * write the one copy: the kernel re-pushes all three at every switch-in, so no
 * per-context copy is kept.
 *
 * Screen-to-screen copies (0x2031, odyssey_copy_rect_buffer()) are
 * APPROXIMATION, INFERRED to skip the rectangles, keeping only the plane
 * bound and the id compare: IRIX 6.5.30's X server issues copies whose
 * destinations lie wholly outside the rectangles in force, with no 0x204b
 * push in between, and expects them to land
 * (docs/reviews/odyssey-window-move-copy-diagnosis-20261001.md, its
 * evidence review). A per-command clip enable in one of the block's
 * constant words fits the same traces equally well; OpenBSD's tail word
 * 0x3222204b carries 0x204b in its low half, where the X server's tails
 * carry 0x0041 or 0x4000. Whether the origin applies to copies is unknown:
 * it was 0 in every traced copy.
 */
/*
 * The window restore (docs/analysis/odyssey-gl-texturing.md, "Window
 * restore"). To render into an off-screen object the GL library overrides
 * 0x841, 0x204b and 0x840 on the user port, and before its next screen draw
 * it sends only the bare word 0x90008116 and a 0x1017 select; no window word
 * follows. The kernel's own window block ends with the same word, and the X
 * server sends it before each of its selects. INFERRED: the word makes the
 * context's committed window -- the values the kernel last pushed on its own
 * port -- current again. The alternative, that something unseen restores the
 * registers, draws the same pictures in every capture.
 */
#define ODSY_CMD_WINDOW_RESTORE 0x90008116 /* 0 operands */
#define ODSY_REG_WINDOW_ORIGIN 0x0841   /* 2 operands: x, y */
#define ODSY_REG_DID_COMPARE 0x0840     /* 2 operands: constant, compare word */
#define ODSY_REG_WINDOW_CLIP 0x204b     /* 8 operands: four rectangles */
#define ODSY_ORIGIN_BITS 13
#define ODSY_CLIP_FIELD_MASK 0xfff
#define ODSY_CLIP_X_SHIFT 12
#define ODSY_DID_COMPARE_ENABLE (1u << 16)
#define ODSY_DID_COMPARE_MASK_SHIFT 8
#define ODSY_DID_COMPARE_OPERAND 1

/*
 * The clip rectangles out of reset: the hardware's reset value is unknown. The
 * model starts with rectangle 0 spanning the whole 12-bit range, as the
 * kernel's own "no clipping" branch pushes it, and the other three empty, so a
 * guest that draws before any 0x204b (the firmware, or a driver that never
 * clips) draws as it did before the register was decoded.
 */
#define ODSY_CLIP_RESET_FULL 0x00ffffff
#define ODSY_CLIP_RESET_EMPTY 0x00001001

#define ODSY_CONTROL_WIDGET_MASK 0xf

/* Memory configuration: size code, bank count and column-address latency. */
#define ODSY_CONFIG_SIZE_SHIFT 30
#define ODSY_CONFIG_TWO_BANKS (1u << 29)
#define ODSY_CONFIG_LATENCY_2 (1u << 28)
/*
 * Bits 27:24 the live V12 capture read back as 0x1
 * (odyssey-octane2-v12-registers-20260926.md): a "driver's don't-care
 * copy field" per that capture, its meaning otherwise not established.
 * Modeled as an unconditional constant since only the one profile has
 * been measured and nothing ties it to size, bank count or latency.
 */
#define ODSY_CONFIG_COPY_FIELD (1u << 24)

static uint32_t odyssey_identity(SGIOdysseyState *s)
{
    return (s->revision << 28) | (ODSY_PART << 12) |
           (ODSY_MANUFACTURER << 1) | 1;
}

/* Reads and writes are reported apart: a write carries a command word. */
#define ODSY_UNIMP_WRITE (1u << 31)

static void odyssey_unimplemented(SGIOdysseyState *s, hwaddr addr,
                                  unsigned size, bool write)
{
    uint32_t token = (uint32_t)addr | (write ? ODSY_UNIMP_WRITE : 0);
    unsigned int i;

    for (i = 0; i < s->reported_addr_count; i++) {
        if (s->reported_addr[i] == token) {
            return;
        }
    }
    if (s->reported_addr_count == SGI_ODYSSEY_UNIMP_REPORTS) {
        return;
    }
    s->reported_addr[s->reported_addr_count++] = token;
    qemu_log_mask(LOG_UNIMP, "sgi-odyssey: %u-byte %s at 0x%" HWADDR_PRIx
                  " is not modeled%s\n", size, write ? "write" : "read", addr,
                  s->reported_addr_count == SGI_ODYSSEY_UNIMP_REPORTS ?
                  "; further offsets stay silent" : "");
}

static void odyssey_cmd_unimplemented(SGIOdysseyState *s, uint32_t word)
{
    unsigned int i;

    for (i = 0; i < s->reported_cmd_count; i++) {
        if (s->reported_cmd[i] == word) {
            return;
        }
    }
    if (s->reported_cmd_count == SGI_ODYSSEY_UNIMP_REPORTS) {
        return;
    }
    s->reported_cmd[s->reported_cmd_count++] = word;
    qemu_log_mask(LOG_UNIMP, "sgi-odyssey: command FIFO word 0x%08x is not "
                  "modeled%s\n", word,
                  s->reported_cmd_count == SGI_ODYSSEY_UNIMP_REPORTS ?
                  "; further words stay silent" : "");
}

/*
 * The modeled register an access names, or -1. A configuration-block
 * register answers its 64-bit address and the 32-bit spelling of either
 * half identically: a live V12 board reads the same value back at a
 * register's base and at base+4 across the whole block, not just on the
 * identity register (odyssey-octane2-v12-registers-20260926.md). Every
 * other register has only been seen as a 32-bit access. Every other
 * width, alignment and offset is unmodeled rather than an error: what the
 * board does with them is not established.
 */
static int64_t odyssey_reg(hwaddr addr, unsigned size)
{
    if (addr < ODSY_BLOCK_END) {
        if ((size == 8 || size == 4) && (addr & 7) == 0) {
            return addr;
        }
        if (size == 4 && (addr & 7) == 4) {
            return addr & ~7;
        }
        return -1;
    }
    if (size != 4) {
        return -1;
    }
    switch (addr) {
    case ODSY_INTR_FLAGS:
    case ODSY_INTR_CLEAR:
    case ODSY_INTR_ENABLE_SET:
    case ODSY_INTR_ENABLE_CLEAR:
    case ODSY_ROUTE_DEST_UPPER:
    case ODSY_ROUTE_DEST_LOWER:
    case ODSY_ROUTE_VECTORS:
    case ODSY_STATUS0:
    case ODSY_CFIFO_SELFTEST:
    case ODSY_MEMORY_CONFIG:
    case ODSY_FLCTL_CONFIG:
    case ODSY_SW_SYNC:
    case ODSY_NIC_PROM:
    case ODSY_NIC_KERNEL:
    case ODSY_BUZZ_1:
    case ODSY_BUZZ_2:
    case ODSY_BUZZ_3:
    case ODSY_BUZZ_4:
    case ODSY_BUZZ_5:
    case ODSY_BUZZ_6:
    case ODSY_SWAP_ARM:
    case ODSY_SWAP_PENDING:
    case ODSY_SWAP_CONTROL:
    case ODSY_SWAP_CONTROL_COMPANION:
    case ODSY_DISPLAYED_BUFFER:
    case ODSY_I2C_MAIN_CMD_READ:
    case ODSY_I2C_MAIN_STATUS_READ:
    case ODSY_I2C_OPT_CMD_READ:
    case ODSY_I2C_OPT_STATUS_READ:
    case ODSY_HOST_WRITE_ADDR_UPPER:
    case ODSY_HOST_WRITE_ADDR_LOWER:
    case ODSY_CTX_SHADOW_ADDR_UPPER:
    case ODSY_CTX_SHADOW_ADDR_LOWER:
    case ODSY_RESET_CONTROL:
    case ODSY_BACKEND_VERSION:
        return addr;
    default:
        return -1;
    }
}

/*
 * An interrupt packet into the widget-addressed fabric, through the XIO
 * widget layer (hw/mips/sgi/xio-widget.h).
 */
static void odyssey_send_packet(SGIOdysseyState *s, hwaddr where,
                                uint8_t vector, bool set)
{
    /* Retain the packet for host-only qtest inspection. */
    s->test_last_interrupt_addr = where;
    s->test_last_interrupt_value = vector | (set ? SGI_XIO_INTERRUPT_SET : 0);
    sgi_xio_send_interrupt(&s->fabric_as, where, vector, set);
}

/* The fabric address of a widget/address pair: the widget in bits 51:48. */
static bool odyssey_xio_compose(SGIOdysseyState *s, unsigned int widget,
                                uint64_t address, hwaddr *fabric)
{
    return sgi_xio_compose("sgi-odyssey", widget, address, fabric);
}

/* The one modeled source uses the generic destination and its own vector. */
static void odyssey_deliver_interrupt(SGIOdysseyState *s, bool set)
{
    unsigned int widget = (s->int_dest_upper >> ODSY_INT_DEST_WIDGET_SHIFT) &
                          ((1u << ODSY_INT_DEST_WIDGET_BITS) - 1);
    uint64_t addr = ((uint64_t)(s->int_dest_upper & 0xffff) << 32) |
                    s->int_dest_lower;
    uint8_t vector = s->int_dest_upper >> ODSY_INT_DEST_VECTOR_SHIFT;
    hwaddr where;

    if (!s->int_dest_upper && !s->int_dest_lower) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: interrupt %s with no "
                      "destination programmed; no packet sent\n",
                      set ? "raised" : "lowered");
        return;
    }
    if (!odyssey_xio_compose(s, widget, addr, &where)) {
        return;
    }
    odyssey_send_packet(s, where, vector, set);
}

/*
 * One routed source's interrupt: a single set pulse, not a level the guest
 * clears through ODSY_INTR_CLEAR like the one source
 * odyssey_deliver_interrupt() above delivers -- the routed handlers ack
 * nothing, so there is no matching "clear" call. Every routed source shares
 * one destination, ODSY_ROUTE_DEST_UPPER/_LOWER, distinct from
 * int_dest_upper/int_dest_lower above; the sources differ only in their byte
 * of ODSY_ROUTE_VECTORS. The vectors are whatever the hub allocator handed
 * the driver, so they are read from the register, never assumed.
 */
static void odyssey_deliver_routed(SGIOdysseyState *s, unsigned int shift,
                                   const char *what)
{
    unsigned int widget = (s->route_dest_upper >>
                           ODSY_ROUTE_DEST_WIDGET_SHIFT) &
                          ((1u << ODSY_INT_DEST_WIDGET_BITS) - 1);
    uint64_t addr = ((uint64_t)(s->route_dest_upper & 0xffff) << 32) |
                    s->route_dest_lower;
    uint8_t vector = s->route_vectors >> shift;
    hwaddr where;

    if (!s->route_dest_upper && !s->route_dest_lower) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: %s interrupt with "
                      "no destination programmed; no packet sent\n", what);
        return;
    }
    if (!odyssey_xio_compose(s, widget, addr, &where)) {
        return;
    }
    odyssey_send_packet(s, where, vector, true);
}

/* The softsync source ODSY_REG_HOST_WRITE (0x0a5a) raises. */
static void odyssey_deliver_softsync(SGIOdysseyState *s)
{
    odyssey_deliver_routed(s, ODSY_ROUTE_VECTOR_SOFTSYNC_SHIFT, "softsync");
}

static void odyssey_update_irq(SGIOdysseyState *s)
{
    bool level = (s->intr_flags & s->intr_enable) != 0;

    if (level != s->interrupt_asserted) {
        s->interrupt_asserted = level;
        odyssey_deliver_interrupt(s, level);
    }
}

/*
 * Latch the command-FIFO low-water flag. The FIFO drains synchronously, so
 * this stands in for the real board raising the flag once its level crosses
 * back below the low-water mark. The flag latches whatever the enable mask
 * says; it reaches the driver only while the source is enabled. The driver
 * enables it from its high-water path and its handler disables it again
 * through ODSY_INTR_ENABLE_SET/_CLEAR, so it fires once per high-water
 * episode, and the driver clears the flag through ODSY_INTR_CLEAR.
 */
static void odyssey_raise_lowater(SGIOdysseyState *s)
{
    s->intr_flags |= ODSY_INTR_FIFO_LOW_WATER;
    odyssey_update_irq(s);
}

/* One bit slot, reported finished inside the write the guest polls after. */
static void odyssey_nic_slot(SGIOdysseyState *s, uint32_t value)
{
    uint32_t pulse = (value & ODSY_NIC_PULSE_MASK) >> ODSY_NIC_PULSE_SHIFT;
    uint32_t sample = (value & ODSY_NIC_SAMPLE_MASK) >> ODSY_NIC_SAMPLE_SHIFT;
    bool level = sgi_onewire_slot(s->nic_bus, pulse, sample);

    s->nic = (value & ~(ODSY_NIC_LEVEL | ODSY_NIC_DONE)) | ODSY_NIC_DONE |
             (level ? ODSY_NIC_LEVEL : 0);
}

static uint32_t odyssey_i2c_status(uint32_t command)
{
    return ODSY_I2C_STATUS_READY | (command != 0 ? ODSY_I2C_STATUS_DONE : 0);
}

static uint64_t odyssey_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIOdysseyState *s = opaque;
    uint64_t value = 0;

    switch (odyssey_reg(addr, size)) {
    case ODSY_IDENTITY:
        value = odyssey_identity(s);
        break;
    case ODSY_CONTROL:
        value = s->widget_control;
        break;
    case ODSY_XT_REQ_TIMEOUT:
        value = s->xt_req_timeout;
        break;
    case ODSY_INT_DEST_UPPER:
        value = s->int_dest_upper;
        break;
    case ODSY_INT_DEST_LOWER:
        value = s->int_dest_lower;
        break;
    case ODSY_ERROR_COMMAND:
        value = s->error_command;
        break;
    case ODSY_STATUS:
    case ODSY_ERROR_ADDR_UPPER:
    case ODSY_ERROR_ADDR_LOWER:
        /* No crosstalk error is ever posted, so the error class is clear. */
        break;
    case ODSY_INTR_FLAGS:
        value = s->intr_flags;
        break;
    case ODSY_INTR_ENABLE_SET:
        value = s->intr_enable;
        break;
    case ODSY_STATUS0:
        /* Enabled, empty, below low water: the FIFO drains synchronously. */
        value = ODSY_STATUS0_READY;
        break;
    case ODSY_CFIFO_SELFTEST:
        /*
         * Answers OK only after the kernel driver's command-FIFO
         * initialisation preamble; zero before.
         */
        value = s->cfifo_selftest_ready ? ODSY_CFIFO_SELFTEST_OK : 0;
        break;
    case ODSY_MEMORY_CONFIG:
        value = s->memory_config;
        break;
    case ODSY_FLCTL_CONFIG:
        value = s->flctl_config;
        break;
    case ODSY_SW_SYNC:
        value = s->sw_sync;
        break;
    case ODSY_BUZZ_1:
        value = s->buzz[0];
        break;
    case ODSY_BUZZ_2:
        value = s->buzz[1];
        break;
    case ODSY_BUZZ_3:
        value = s->buzz[2];
        break;
    case ODSY_BUZZ_4:
        value = s->buzz[3];
        break;
    case ODSY_BUZZ_5:
        value = s->buzz[4];
        break;
    case ODSY_BUZZ_6:
        value = s->buzz[5];
        break;
    case ODSY_SWAP_ARM:
        value = s->swap_arm;
        break;
    case ODSY_SWAP_PENDING:
        value = s->swap_pending;
        break;
    case ODSY_SWAP_CONTROL:
        value = s->swap_control;
        break;
    case ODSY_SWAP_CONTROL_COMPANION:
        value = s->swap_control_companion;
        break;
    case ODSY_DISPLAYED_BUFFER:
        /* The buffer the last latched swap put on screen (odyssey_retrace()). */
        value = s->displayed_buffer;
        break;
    case ODSY_NIC_PROM:
    case ODSY_NIC_KERNEL:
        value = s->nic;
        break;
    case ODSY_I2C_MAIN_CMD_READ:
        value = s->i2c_main_cmd;
        break;
    case ODSY_I2C_MAIN_STATUS_READ:
        value = odyssey_i2c_status(s->i2c_main_cmd);
        break;
    case ODSY_I2C_OPT_CMD_READ:
        value = s->i2c_opt_cmd;
        break;
    case ODSY_I2C_OPT_STATUS_READ:
        value = odyssey_i2c_status(s->i2c_opt_cmd);
        break;
    case ODSY_BACKEND_VERSION:
        /* Pinned at 0 for every profile; see the define's comment. */
        value = 0;
        break;
    case ODSY_ROUTE_DEST_UPPER:
    case ODSY_ROUTE_DEST_LOWER:
    case ODSY_ROUTE_VECTORS:
    case ODSY_HOST_WRITE_ADDR_UPPER:
    case ODSY_HOST_WRITE_ADDR_LOWER:
    case ODSY_CTX_SHADOW_ADDR_UPPER:
    case ODSY_CTX_SHADOW_ADDR_LOWER:
    case ODSY_RESET_CONTROL:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: read of the write-only "
                      "register at 0x%" HWADDR_PRIx "\n", addr);
        break;
    default:
        odyssey_unimplemented(s, addr, size, false);
        break;
    }
    trace_sgi_odyssey_mmio(addr, size, value, false);
    return value;
}

/*
 * One data word of a backend record, delivered to the entry the record's
 * header selected. The colour lookup RAM and the per-drawable display-mode
 * registers take their words here; the two I2C command indices keep their
 * existing dispatch, so a count == 1 I2C selector still reaches the same
 * register it always did. The third I2C-unit index 0x2700 has no established
 * function and stays unimplemented, as it did before this became a stream
 * (docs/analysis/odyssey-colormap-format.md).
 */
static void odyssey_backend_data(SGIOdysseyState *s, uint32_t index,
                                 uint32_t data)
{
    /* Scanout resolves through both, so either change repaints. */
    if (index < ODSY_LUT_SIZE) {
        s->lut[index] = data & ODSY_LUT_ENTRY_MASK;
        sgi_scanout_invalidate_all(&s->scanout);
        return;
    }
    if (index >= ODSY_DISPMODE_BASE &&
        index < ODSY_DISPMODE_BASE + ODSY_DISPMODE_COUNT) {
        s->dispmode[index - ODSY_DISPMODE_BASE] = data;
        sgi_scanout_invalidate_all(&s->scanout);
        return;
    }
    switch (index) {
    case ODSY_I2C_MAIN_CMD_INDEX:
        s->i2c_main_cmd = data;
        break;
    case ODSY_I2C_OPT_CMD_INDEX:
        s->i2c_opt_cmd = data;
        break;
    default:
        odyssey_unimplemented(s, ODSY_BACKEND_SELECT, 8, true);
        break;
    }
}

/*
 * One 32-bit word of the backend word stream. While no record is open the
 * word is a header: its 0b11 tag opens a record of `count` data words
 * starting at `index` (a zero count is a pad). Any word without the tag at a
 * header position has no established meaning and is reported rather than
 * guessed. While a record is open the word is a data word for the running
 * index, which then advances (see odyssey_backend_data).
 */
static void odyssey_backend_word(SGIOdysseyState *s, uint32_t word)
{
    if (s->backend_count == 0) {
        if (((word >> ODSY_BACKEND_TAG_SHIFT) & ODSY_BACKEND_TAG_MASK) !=
            ODSY_BACKEND_TAG_HEADER) {
            odyssey_unimplemented(s, ODSY_BACKEND_SELECT, 8, true);
            return;
        }
        s->backend_index = (word >> ODSY_BACKEND_SELECTOR_INDEX_SHIFT) &
                           ODSY_BACKEND_SELECTOR_INDEX_MASK;
        s->backend_count = word & ODSY_BACKEND_COUNT_MASK;
        return;
    }
    odyssey_backend_data(s, s->backend_index, word);
    s->backend_index = (s->backend_index + 1) &
                       ODSY_BACKEND_SELECTOR_INDEX_MASK;
    s->backend_count--;
}

/*
 * The indexed register file's write port: an 8-byte store is two words,
 * upper half first, a 4-byte store is one word (see ODSY_BACKEND_SELECT).
 */
static void odyssey_backend_write(SGIOdysseyState *s, uint64_t value,
                                  unsigned size)
{
    if (size == 8) {
        odyssey_backend_word(s, value >> 32);
        odyssey_backend_word(s, (uint32_t)value);
    } else {
        odyssey_backend_word(s, (uint32_t)value);
    }
}

/*
 * The hardware shadow page (docs/analysis/odyssey-context-state.md, section
 * 3): the layout the kernel driver's shadow-page template setup gives the
 * page the kernel names in 0x4004/0x400c and reads back after every switch
 * (ESTABLISHED as the kernel's template and reads; that the board fills it
 * is INFERRED).
 *
 * - +0x80: the pipe state, bits 3:0 the open primitive's type or 0xf idle;
 *   the other fields are ring positions, 0 selecting slot 0.
 * - +0x84/+0x88: the current attribute descriptor, each attribute's ring
 *   slot selector (bits 25:24 the texture coordinate's); +0x8c + 8 S the
 *   same for vertex slot S, 0 to 3. Within the kernel range the context
 *   completeness evidence review scanned, only the template writes them, so
 *   the board is their only writer there; for the rest of the kernel that
 *   is inferred.
 * - +0x240/+0x250: the current colour index and colour words the kernel
 *   driver's context restore replays, operands at +0x800 and +0x840.
 * - +0x100 + 16 A + 4 slot: attribute A's word in ring slot `slot`, its
 *   operands at +0x300 + 64 A + 16 slot. A 1 is the index, 2 the colour, 3
 *   the normal, 4 the texture coordinate, 7 the edge flag, 8 to 11 the front
 *   material, 12 the shininess. The kernel replays rings 1, 2 and 7 at every
 *   switch-in, and the others on its full restore path: a switch to a
 *   nonzero context other than the last one loaded, whether or not a
 *   primitive was open (the "Integer normals" verdict in
 *   docs/reviews/odyssey-query-open-items-evidence-20261001.md).
 */
#define ODSY_HW_PIPE_STATE 0x80
#define ODSY_HW_PIPE_IDLE 0xf
#define ODSY_HW_CURRENT_ATTRIBUTES 0x84
#define ODSY_HW_DESCRIPTOR_WORDS 10     /* +0x84 to +0xab */
#define ODSY_HW_INDEX_WORD 0x240
#define ODSY_HW_COLOUR_WORD 0x250
#define ODSY_HW_INDEX_OPERANDS 0x800
#define ODSY_HW_COLOUR_OPERANDS 0x840
#define ODSY_HW_RING_WORD 0x100
#define ODSY_HW_RING_WORD_STRIDE 16
#define ODSY_HW_RING_OPERANDS 0x300
#define ODSY_HW_RING_OPERAND_STRIDE 64
#define ODSY_HW_ATTR_INDEX 1
#define ODSY_HW_ATTR_COLOUR 2
#define ODSY_HW_ATTR_NORMAL 3
#define ODSY_HW_ATTR_TEXTURE 4
#define ODSY_HW_ATTR_EDGE 7
#define ODSY_HW_ATTR_MATERIAL 8
#define ODSY_HW_ATTR_SHININESS 12
#define ODSY_HW_ATTR_BACK_MATERIAL 14
#define ODSY_HW_ATTR_BACK_SHININESS 18
/* The index word the kernel's template carries; its colour is cc0d. */
#define ODSY_GL_REPLAY_INDEX 0xce80c406   /* 1 operand */

/* The public current-attribute records, retained as raw command words. */
enum {
    ODSY_QUERY_INDEX,
    ODSY_QUERY_COLOUR,
    ODSY_QUERY_NORMAL,
    ODSY_QUERY_TEXCOORD,
    ODSY_QUERY_EDGE,
};

/* Deterministic model defaults; physical back-material reset is unknown. */
static const uint32_t odyssey_back_material_defaults[16] = {
    ODSY_F32_POINT_TWO, ODSY_F32_POINT_TWO, ODSY_F32_POINT_TWO, ODSY_F32_ONE,
    ODSY_F32_POINT_EIGHT, ODSY_F32_POINT_EIGHT,
    ODSY_F32_POINT_EIGHT, ODSY_F32_ONE,
    0, 0, 0, ODSY_F32_ONE,
    0, 0, 0, ODSY_F32_ONE,
};

static void odyssey_back_material_reset(SGIOdysseyState *s)
{
    memcpy(s->gl_back_material, odyssey_back_material_defaults,
           sizeof(s->gl_back_material));
    s->gl_back_shininess_sqrt = 0;
}

static void odyssey_shadow_put(SGIOdysseyState *s, uint64_t base,
                               uint32_t off, uint32_t word)
{
    MemTxResult res = MEMTX_OK;

    address_space_stl_be(&address_space_memory, base + off, word,
                         MEMTXATTRS_UNSPECIFIED, &res);
    if (res != MEMTX_OK && !s->ctx_shadow_fail_reported) {
        s->ctx_shadow_fail_reported = true;
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: context shadow store to "
                      "0x%" PRIx64 " failed\n", base + off);
    }
}

/* One selected ring entry: attribute a's word and its n raw operands. */
static void odyssey_shadow_ring(SGIOdysseyState *s, uint64_t base,
                                unsigned a, unsigned slot, uint32_t word,
                                const uint32_t *ops, unsigned n)
{
    odyssey_shadow_put(s, base, ODSY_HW_RING_WORD +
                       ODSY_HW_RING_WORD_STRIDE * a + 4 * slot, word);
    for (unsigned int i = 0; i < n; i++) {
        odyssey_shadow_put(s, base, ODSY_HW_RING_OPERANDS +
                           ODSY_HW_RING_OPERAND_STRIDE * a +
                           16 * slot + 4 * i, ops[i]);
    }
}

static void odyssey_open_current_records(SGIOdysseyState *s,
                                         uint32_t record[19][5])
{
    static const uint32_t material[4] = {
        ODSY_GL_MATERIAL_AMBIENT, ODSY_GL_MATERIAL_DIFFUSE,
        ODSY_GL_MATERIAL_SPECULAR, ODSY_GL_MATERIAL_EMISSION,
    };
    static const uint32_t back_material[4] = {
        ODSY_GL_BACK_AMBIENT, ODSY_GL_BACK_DIFFUSE,
        ODSY_GL_BACK_SPECULAR, ODSY_GL_BACK_EMISSION,
    };

    memset(record, 0, 19 * 5 * sizeof(uint32_t));
    memcpy(record[ODSY_HW_ATTR_INDEX], s->query_current[ODSY_QUERY_INDEX],
           sizeof(record[0]));
    memcpy(record[ODSY_HW_ATTR_COLOUR], s->query_current[ODSY_QUERY_COLOUR],
           sizeof(record[0]));
    memcpy(record[ODSY_HW_ATTR_NORMAL], s->query_current[ODSY_QUERY_NORMAL],
           sizeof(record[0]));
    memcpy(record[ODSY_HW_ATTR_TEXTURE],
           s->query_current[ODSY_QUERY_TEXCOORD], sizeof(record[0]));
    memcpy(record[ODSY_HW_ATTR_EDGE], s->query_current[ODSY_QUERY_EDGE],
           sizeof(record[0]));
    for (unsigned i = 0; i < 4; i++) {
        record[ODSY_HW_ATTR_MATERIAL + i][0] = material[i];
        memcpy(&record[ODSY_HW_ATTR_MATERIAL + i][1],
               &s->gl_material[4 * i], 4 * sizeof(uint32_t));
        record[ODSY_HW_ATTR_BACK_MATERIAL + i][0] = back_material[i];
        memcpy(&record[ODSY_HW_ATTR_BACK_MATERIAL + i][1],
               &s->gl_back_material[4 * i], 4 * sizeof(uint32_t));
    }
    record[ODSY_HW_ATTR_SHININESS][0] = ODSY_GL_SHININESS_SQRT;
    record[ODSY_HW_ATTR_SHININESS][1] = s->gl_shininess_sqrt;
    record[ODSY_HW_ATTR_BACK_SHININESS][0] = ODSY_GL_BACK_SHININESS_SQRT;
    record[ODSY_HW_ATTR_BACK_SHININESS][1] = s->gl_back_shininess_sqrt;
}

static void odyssey_open_record_vertex(SGIOdysseyState *s, uint32_t word)
{
    SGIOdysseyOpenVertex v = { .word = word, .slot = s->gl_open_r };
    SGIOdysseyOpenVertex *held = s->gl_open_vertex;
    unsigned n = s->gl_open_held;

    memcpy(v.operands, s->cmd_operand,
           odyssey_cmd_arity(word) * sizeof(uint32_t));
    odyssey_open_current_records(s, v.attribute);
    if (s->gl_primitive == 2 || s->gl_primitive == 6) {
        s->gl_open_r = s->gl_open_r == 3 ? 1 : s->gl_open_r + 1;
    } else {
        s->gl_open_r = (s->gl_open_r + 1) & 3;
    }
    s->gl_open_c = MIN(s->gl_open_c + 1, 7u);
    s->gl_open_m3 = (s->gl_open_m3 + 1) % 3;
    s->gl_open_m4 = (s->gl_open_m4 + 1) & 3;

    switch (s->gl_primitive) {
    case 0:                         /* Points retain no vertices. */
        n = 0;
        break;
    case 1:                         /* Lines retain one of each pair. */
        if (n) {
            n = 0;
        } else {
            held[0] = v;
            n = 1;
        }
        break;
    case 2: case 6:                 /* Anchor and latest vertex. */
        held[n ? 1 : 0] = v;
        n = MIN(n + 1, 2u);
        break;
    case 3:                         /* Line strip. */
        held[0] = v;
        n = 1;
        break;
    case 4:                         /* Triangles. */
    case 7:                         /* Quads. */
        if (n == (s->gl_primitive == 4 ? 2 : 3)) {
            n = 0;
        } else {
            held[n++] = v;
        }
        break;
    case 5:                         /* Triangle strip. */
        if (n == 2) {
            held[0] = held[1];
            held[1] = v;
        } else {
            held[n++] = v;
        }
        break;
    case 8:                         /* Quad strip. */
        if (n == 3) {
            held[0] = held[2];
            held[1] = v;
            n = 2;
        } else {
            held[n++] = v;
        }
        break;
    }
    s->gl_open_held = n;
}

static void odyssey_restart_finish(SGIOdysseyState *s)
{
    uint32_t n = s->gl_open_held;
    uint32_t count;

    if (!s->gl_restart_armed || s->gl_restart_absorb) {
        return;
    }
    if (s->gl_restart_m3 >= 3 || n != s->gl_restart_total) {
        odyssey_cmd_unimplemented(s, 0x741);
        s->gl_restart_armed = false;
        return;
    }
    s->gl_open_c = s->gl_restart_c == 7 ? 7 :
                   MIN(s->gl_restart_c + n, 7u);
    s->gl_open_m3 = (s->gl_restart_m3 + n) % 3;
    s->gl_open_m4 = (s->gl_restart_m4 + n) & 3;
    count = s->gl_open_c;
    if (count == 7) {
        while ((count % 3 != s->gl_open_m3 ||
                count % 4 != s->gl_open_m4) && count < 19) {
            count++;
        }
    }
    s->gl_in_prim = sgi_gl_resume_primitive(&s->gl_ctx, s->gl_restart_type,
                                             s->gl_restart_vertex, n, count);
    s->gl_restart_armed = false;
}

/*
 * The board's save of the outgoing context's geometry-engine state into the
 * hardware shadow, before the completion word. Register state needs no
 * save: the kernel re-pushes it from the owner's software shadow on every
 * switch-in, so the model keeps its single copy. What the kernel cannot
 * restore itself is the current vertex attributes, which it replays from
 * this page: the colour index, colour and edge flag at every switch-in, the
 * other rings on the kernel's full restore path.
 *
 * The index and colour go out as the last command word each record retained,
 * with its raw operands and the count its bits 13:10 give, at +0x240/+0x800
 * and +0x250/+0x840 and in ring slot 0 of attributes 1 and 2. Nothing is
 * converted between a setter, this save and a later query, so the kernel's
 * replay restores the same record the client set, in whatever format it
 * used. This is an approximation
 * (docs/reviews/odyssey-query-formats-7-13-evidence-20260930.md): the
 * physical board's saved words and operand encodings are unknown, and the
 * template's fixed 0xce80c406 and 0xc580cc0d are only the kernel's defaults
 * for a page never saved. The normal goes to ring 3 slot 0 the same way, as
 * the normal record's word and raw operands, so a replay leaves a later
 * query answering in the format the client used; a never-set record saves
 * its default 0xc180cc19 0 0 1.0. The board's saved normal word is unknown
 * (docs/reviews/odyssey-query-open-items-evidence-20261001.md). Back
 * material is retained without evaluating it for lighting. The texture
 * coordinate goes to ring 4 slot 0 as the raw two-float word. The edge flag
 * goes to ring 7 slot 0 as its retained word, 0x2f00c02e (true, also the
 * never-set default) or 0x3000c02e, with no operands; without it every
 * switch-in replayed the template's false word and cleared the client's
 * flag. The board's saved edge word is unknown.
 *
 * Every save writes all ten descriptor words. Idle selects ring slot 0.
 * A float primitive of type 0..8 packs its retained raw attribute records
 * into ring slots 0..N-1 at save time, and current records into slot 3.
 * Its descriptors point to those slots, while the raw vertex words retain
 * their model cursor slots. The store itself leaves the primitive open,
 * including for the 0x7f snapshot. Integer 2D, type 10 and type 9 polygon
 * assembly still report an unsupported switch and save idle. Type 9's
 * assembler holds every vertex, beyond this page policy's retained set.
 * This producer and its restart
 * decode are model approximations, not established board behavior
 * (docs/reviews/odyssey-context-completeness-evidence-20261001.md).
 */
static void odyssey_ctx_save(SGIOdysseyState *s, uint64_t base)
{
    static const uint32_t material[4] = {
        ODSY_GL_MATERIAL_AMBIENT, ODSY_GL_MATERIAL_DIFFUSE,
        ODSY_GL_MATERIAL_SPECULAR, ODSY_GL_MATERIAL_EMISSION,
    };
    static const uint32_t back_material[4] = {
        ODSY_GL_BACK_AMBIENT, ODSY_GL_BACK_DIFFUSE,
        ODSY_GL_BACK_SPECULAR, ODSY_GL_BACK_EMISSION,
    };
    const uint32_t *index = s->query_current[ODSY_QUERY_INDEX];
    const uint32_t *colour = s->query_current[ODSY_QUERY_COLOUR];
    const uint32_t *normal = s->query_current[ODSY_QUERY_NORMAL];
    uint32_t edge = s->query_current[ODSY_QUERY_EDGE][0];
    bool open = s->gl_begin_open && !s->gl_integer_prim &&
                s->gl_primitive <= 8;
    unsigned int i;

    if (s->gl_begin_open && !open && !s->ctx_open_prim_logged) {
        s->ctx_open_prim_logged = true;
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: context switch inside "
                      "primitive 0x%x; its buffered vertices are not saved\n",
                      s->gl_primitive);
    }
    odyssey_shadow_put(s, base, ODSY_HW_PIPE_STATE, ODSY_HW_PIPE_IDLE);
    for (i = 0; i < ODSY_HW_DESCRIPTOR_WORDS; i++) {
        odyssey_shadow_put(s, base, ODSY_HW_CURRENT_ATTRIBUTES + 4 * i, 0);
    }
    odyssey_shadow_put(s, base, ODSY_HW_INDEX_WORD, index[0]);
    for (i = 0; i < odyssey_cmd_arity(index[0]); i++) {
        odyssey_shadow_put(s, base, ODSY_HW_INDEX_OPERANDS + 4 * i,
                           index[1 + i]);
    }
    odyssey_shadow_put(s, base, ODSY_HW_COLOUR_WORD, colour[0]);
    for (i = 0; i < odyssey_cmd_arity(colour[0]); i++) {
        odyssey_shadow_put(s, base, ODSY_HW_COLOUR_OPERANDS + 4 * i,
                           colour[1 + i]);
    }
    odyssey_shadow_ring(s, base, ODSY_HW_ATTR_INDEX, 0, index[0], &index[1],
                        odyssey_cmd_arity(index[0]));
    odyssey_shadow_ring(s, base, ODSY_HW_ATTR_COLOUR, 0, colour[0], &colour[1],
                        odyssey_cmd_arity(colour[0]));
    odyssey_shadow_ring(s, base, ODSY_HW_ATTR_NORMAL, 0, normal[0],
                        &normal[1], odyssey_cmd_arity(normal[0]));
    odyssey_shadow_ring(s, base, ODSY_HW_ATTR_EDGE, 0, edge, NULL, 0);
    odyssey_shadow_ring(s, base, ODSY_HW_ATTR_TEXTURE, 0,
                        s->tex_mode & ODSY_TEX_MODE_3D ?
                        ODSY_GL_TEXCOORD_3F : ODSY_GL_TEXCOORD_2F,
                        s->tex_mode & ODSY_TEX_MODE_3D ?
                        (uint32_t[3]) { s->tex_coord[0], s->tex_coord[1],
                                       s->tex_coord_r } : s->tex_coord,
                        s->tex_mode & ODSY_TEX_MODE_3D ? 3 : 2);
    for (i = 0; i < 4; i++) {
        odyssey_shadow_ring(s, base, ODSY_HW_ATTR_MATERIAL + i, 0,
                            material[i],
                            &s->gl_material[i * 4], 4);
    }
    odyssey_shadow_ring(s, base, ODSY_HW_ATTR_SHININESS, 0,
                        ODSY_GL_SHININESS_SQRT, &s->gl_shininess_sqrt, 1);
    for (i = 0; i < 4; i++) {
        odyssey_shadow_ring(s, base, ODSY_HW_ATTR_BACK_MATERIAL + i, 0,
                            back_material[i], &s->gl_back_material[i * 4], 4);
    }
    odyssey_shadow_ring(s, base, ODSY_HW_ATTR_BACK_SHININESS, 0,
                        ODSY_GL_BACK_SHININESS_SQRT,
                        &s->gl_back_shininess_sqrt, 1);
    if (open) {
        uint32_t current[19][5];
        uint32_t state = s->gl_primitive | (s->gl_open_held << 4) |
                         (s->gl_open_r << 6) | (s->gl_open_m3 << 12) |
                         (s->gl_open_m4 << 14) | (s->gl_open_c << 21);

        odyssey_open_current_records(s, current);
        odyssey_shadow_put(s, base, ODSY_HW_PIPE_STATE, state);
        for (i = 0; i < s->gl_open_held; i++) {
            const SGIOdysseyOpenVertex *v = &s->gl_open_vertex[i];
            unsigned slot = v->slot;
            uint32_t selector = UINT32_C(0x55555555) * i;

            odyssey_shadow_ring(s, base, 0, slot, v->word,
                                v->operands, odyssey_cmd_arity(v->word));
            odyssey_shadow_put(s, base, 0x8c + 8 * slot, selector);
            odyssey_shadow_put(s, base, 0x90 + 8 * slot, selector);
            for (unsigned a = 1; a < 19; a++) {
                if (!v->attribute[a][0]) {
                    continue;
                }
                odyssey_shadow_ring(s, base, a, i, v->attribute[a][0],
                                    &v->attribute[a][1],
                                    odyssey_cmd_arity(v->attribute[a][0]));
            }
        }
        odyssey_shadow_put(s, base, 0x84, UINT32_MAX);
        odyssey_shadow_put(s, base, 0x88, UINT32_MAX);
        for (i = 0; i < 19; i++) {
            if (!current[i][0]) {
                continue;
            }
            odyssey_shadow_ring(s, base, i, 3, current[i][0],
                                &current[i][1],
                                odyssey_cmd_arity(current[i][0]));
        }
    }
}

/*
 * The kernel driver's context switch: a single 8-byte store to
 * ODSY_CTX_SWITCH_PORT, not a command-FIFO word (see that macro's
 * comment for the trace-derived word format). The new context id in
 * bits 31:16 is not modeled: the board learns the context from the shadow
 * address pair, which names where the outgoing state is saved
 * (odyssey_ctx_save()), and the kernel restores the rest. A store naming
 * context 0x7f is the kernel driver's shadow snapshot and saves the same
 * way.
 * Any other shape at this port -- the wrong opcode or the valid bit clear
 * -- has no established meaning and is reported unimplemented instead of
 * guessed, the same stance odyssey_cmd_arity() takes for an unestablished
 * command arity.
 */
static void odyssey_query_cancel(SGIOdysseyState *s);

static void odyssey_ctx_switch(SGIOdysseyState *s, uint64_t value)
{
    uint64_t shadow_base;

    if (((value >> ODSY_CTX_SWITCH_OPCODE_SHIFT) & 0xff) !=
        ODSY_CTX_SWITCH_OPCODE || !(value & ODSY_CTX_SWITCH_VALID)) {
        odyssey_unimplemented(s, ODSY_CTX_SWITCH_PORT, 8, true);
        return;
    }
    if (!s->ctx_shadow_addr_upper && !s->ctx_shadow_addr_lower) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: context switch (0x%"
                      PRIx64 ") with no shadow address programmed; no "
                      "completion written\n", value);
        return;
    }
    odyssey_query_cancel(s);
    shadow_base = ((uint64_t)(s->ctx_shadow_addr_upper & 0xffff) << 32) |
                  s->ctx_shadow_addr_lower;
    /*
     * A plain guest-physical address, the same direct-RAM path the
     * DMA-sync host-write command uses and for the same reason:
     * s->fabric_as carries interrupt/doorbell packets only, not general
     * RAM.
     */
    odyssey_ctx_save(s, shadow_base);
    odyssey_shadow_put(s, shadow_base, ODSY_CTX_SWITCH_DONE_OFFSET, 1);
}

/*
 * 2D fills carry no alpha channel in this grammar; the framebuffer's
 * fourth byte is modeled fully opaque (the colour plane's red-high R,G,B,A
 * packing, resolved by odyssey_resolve_row() below).
 */
static uint32_t odyssey_pack_pixel(uint32_t r, uint32_t g, uint32_t b)
{
    return (r << 24) | (g << 16) | (b << 8) | 0xff;
}

/*
 * Colour buffer b's board memory and plane (odyssey_realize()), b being 0 or
 * 1 in the displayed-buffer bits' sense (ODSY_REG_DRAW_MASK).
 */
static SGIBoardMemory *odyssey_buffer_mem(SGIOdysseyState *s, unsigned b)
{
    return b ? &s->buffer1_mem : &s->mem;
}

static const SGIPlane *odyssey_buffer_plane(SGIOdysseyState *s, unsigned b)
{
    return b ? sgi_board_memory_plane(&s->buffer1_mem, s->buffer1_plane) :
               sgi_board_memory_plane(&s->mem, s->colour_plane);
}

static uint32_t odyssey_buffer_load(SGIOdysseyState *s, unsigned b,
                                    int64_t x, int64_t y)
{
    return sgi_plane_load(odyssey_buffer_mem(s, b),
                          odyssey_buffer_plane(s, b), x, y);
}

static void odyssey_buffer_store(SGIOdysseyState *s, unsigned b,
                                 int64_t x, int64_t y, uint32_t word)
{
    sgi_plane_store(odyssey_buffer_mem(s, b), odyssey_buffer_plane(s, b),
                    x, y, word);
}

/*
 * The buffers a colour write lands in, bit b for buffer b, from the last
 * 0x0424 drawing mask (ODSY_REG_DRAW_MASK). A right-eye bit folds into its
 * buffer: stereo is not modeled.
 */
static unsigned odyssey_draw_buffers(SGIOdysseyState *s)
{
    return (s->draw_mask & ODSY_DRAW_MASK_BUFFER0 ? 1 : 0) |
           (s->draw_mask & ODSY_DRAW_MASK_BUFFER1 ? 2 : 0);
}

/*
 * The one completion fence, shared by the standalone 0x0a5a host-write command
 * and the DMA engine's publish() (see odyssey_publish() below): write the
 * cookie's 16-bit token, big-endian, to the guest fence slot at the host-write
 * base plus twice the slot, and pulse the softsync interrupt when the cookie
 * asks. The base is the driver-programmed host_write_addr pair; the slot, token
 * and interrupt request travel in the cookie (ODSY_FENCE_* / ODSY_DMA_FENCE_*).
 * address_space_memory, not s->fabric_as, carries the write: the fabric is
 * Bedrock's inbound packet face (interrupt delivery only), not general RAM.
 */
static void odyssey_fence(SGIOdysseyState *s, uint64_t cookie)
{
    uint32_t word = cookie & ODSY_FENCE_WORD_MASK;
    uint32_t slot = word & ODSY_DMA_FENCE_SLOT_MASK;
    uint16_t token = (word >> ODSY_DMA_FENCE_TOKEN_SHIFT) & 0xffff;
    uint64_t base = ((uint64_t)(s->host_write_addr_upper & 0xffff) << 32) |
                    s->host_write_addr_lower;

    address_space_stw_be(&address_space_memory, base + 2 * slot, token,
                         MEMTXATTRS_UNSPECIFIED, NULL);
    if (word & ODSY_DMA_FENCE_INTERRUPT) {
        odyssey_deliver_softsync(s);
    }
}

/*
 * The shared graphics foundation's head hooks (hw/display/sgi/head-ops.h).
 * resolve_row turns the colour plane into displayed colour through each
 * pixel's drawable, display mode and the lookup RAM; publish raises the
 * completion fence a written transfer signals; dma_chunk traces the guest
 * bytes the DMA engine reads for the family's replay tools; retrace is the
 * vertical-retrace callback the shared SGIScanout drives at the mode's rate.
 * dma_address stays NULL: this host's inbound path is direct-mapped, so a
 * device address is the bus address. The rest stay NULL, taking the defaults
 * head-ops.h documents.
 */
static void odyssey_publish(void *opaque, const SGICompletion *c)
{
    SGIOdysseyState *s = opaque;

    /*
     * A transfer that reached board or guest memory raises the fence; a failed
     * one does not, so the driver's DMA wait times out as it would on a real
     * board that never wrote the fence halfword. A cookie without the validity
     * marker (a future credit-return record) wants no fence.
     */
    if (c->kind == SGI_DONE_WRITTEN && (c->cookie & ODSY_FENCE_VALID)) {
        odyssey_fence(s, c->cookie);
    }
}

/*
 * A run of guest bytes the DMA engine read, handed to the family's replay
 * trace at the device address before any composition (dma_address is NULL here,
 * so the two are the same), tagged with the submitting sequence. The family
 * owns the trace event; an external analyser reads it.
 */
static void odyssey_dma_chunk(void *opaque, uint64_t seq, uint64_t addr,
                              const uint8_t *bytes, uint32_t len)
{
    trace_sgi_odyssey_dma_chunk(seq, addr, len);
}

/* The top eight bits of a lookup-RAM entry's ten-bit channels, as RGB. */
static uint32_t odyssey_lut_rgb(SGIOdysseyState *s, uint32_t entry)
{
    uint32_t word = entry < ODSY_LUT_SIZE ? s->lut[entry] : 0;

    return (((word >> ODSY_LUT_RED_SHIFT) & 0xff) << 16) |
           (((word >> ODSY_LUT_GREEN_SHIFT) & 0xff) << 8) |
           ((word >> ODSY_LUT_BLUE_SHIFT) & 0xff);
}

/*
 * One 8-bit RGB channel through the 1,536-entry gamma table: widened to 12
 * bits as the X server widens it (c << 4 | c >> 4; the board's own widening
 * is UNKNOWN), mapped to the table in three ranges -- v below 0x200, v / 2 +
 * 0x100 below 0x400, else v / 4 + 0x200 -- and shown as the top eight bits of
 * that entry's channel at shift. INFERRED: that the table applies to RGB
 * pixels at all (the blank zeroes it to blacken the screen).
 */
static uint32_t odyssey_gamma(SGIOdysseyState *s, uint32_t c, unsigned shift)
{
    uint32_t v = (c << 4) | (c >> 4);
    uint32_t t = v < 0x200 ? v : v < 0x400 ? v / 2 + 0x100 : v / 4 + 0x200;

    return (s->lut[ODSY_GAMMA_BASE + t] >> shift) & 0xff;
}

/* Pixel (x, y)'s main drawable id, bits 7:3 of its id-plane byte. */
static uint32_t odyssey_main_id(SGIOdysseyState *s, uint32_t x, uint32_t y)
{
    return (s->did[y * SGI_ODYSSEY_WIDTH + x] >> ODSY_DID_MAIN_SHIFT) &
           (ODSY_DISPMODE_COUNT - 1);
}

/*
 * The colormap group main drawable main_id shows through: bits 8:4 of its
 * display mode, main id 0 taking drawable 1's (odyssey_resolve_row()).
 * Below ODSY_GAMMA_GROUP the plane word is an index.
 */
static uint32_t odyssey_display_group(SGIOdysseyState *s, uint32_t main_id)
{
    uint32_t mode = s->dispmode[main_id ? main_id : ODSY_DID_ROOT_MODE];

    return (mode >> ODSY_DISPMODE_GROUP_SHIFT) & ODSY_DISPMODE_GROUP_MASK;
}

/*
 * Resolve the colour buffers at scanout (docs/analysis/odyssey-colormap-
 * scanout.md, "What the model must do"). Per pixel: the id plane's main
 * drawable (bits 7:3) selects the buffer shown, by its ODSY_DISPLAYED_BUFFER
 * bit, and display mode 0x2900 + id, whose bits 8:4 are the group. INFERRED:
 * that the displayed-buffer bits index windows by main drawable id, as the
 * display-mode path's 1 << id writes to 0x10c4 and 0x10cc do. Main id 0
 * shows by its own bit 0; how the board treats id 0 is unknown. A group at
 * or above 26 is an RGB mode: each channel of the plane word goes through
 * the gamma table. Below it the plane word is an index: lut[256 * group +
 * (index & 0xff)], or lut[index & 0xfff] for group 0, the 4,096-entry case;
 * no second gamma pass. Main id 0 takes drawable
 * 1's mode (ODSY_DID_ROOT_MODE, an approximation). Group 25 (0x1900, the
 * table the server keeps out of its software gamma) resolves as an index
 * table and is reported once as unverified; a non-zero overlay field (bits
 * 2:0) is UNKNOWN territory, resolved through the main id and reported once.
 */
static void odyssey_resolve_row(void *opaque, unsigned y, uint32_t *dst)
{
    SGIOdysseyState *s = opaque;
    const uint8_t *did = &s->did[y * SGI_ODYSSEY_WIDTH];
    uint32_t x;

    for (x = 0; x < SGI_ODYSSEY_WIDTH; x++) {
        uint32_t main_id = odyssey_main_id(s, x, y);
        uint32_t word = odyssey_buffer_load(s, (s->displayed_buffer >>
                                                main_id) & 1, x, y);
        uint32_t group = odyssey_display_group(s, main_id);

        if ((did[x] & ODSY_DID_OVERLAY_MASK) && !s->scanout_overlay_reported) {
            s->scanout_overlay_reported = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: overlay drawable id %u "
                          "is not modeled; resolved through the main id\n",
                          did[x] & ODSY_DID_OVERLAY_MASK);
        }
        if (group >= ODSY_GAMMA_GROUP) {
            dst[x] = odyssey_gamma(s, word >> 24, ODSY_LUT_RED_SHIFT) << 16 |
                     odyssey_gamma(s, (word >> 16) & 0xff,
                                   ODSY_LUT_GREEN_SHIFT) << 8 |
                     odyssey_gamma(s, (word >> 8) & 0xff, ODSY_LUT_BLUE_SHIFT);
            continue;
        }
        if (group == ODSY_CMAP_UNVERIFIED_GROUP &&
            !s->scanout_group25_reported) {
            s->scanout_group25_reported = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: colormap group 25 (0x1900) "
                          "resolved as an index table; unverified\n");
        }
        dst[x] = odyssey_lut_rgb(s, group ? 256 * group + (word & 0xff) :
                                 word & ODSY_INDEX_MASK);
    }
}

/*
 * A vertical retrace, driven by the shared SGIScanout at the mode's rate
 * (docs/analysis/odyssey-swap-wake-mechanism.md), as the guest driver's
 * retrace handler, swap scheduling and buffer swap use it.
 *
 * The driver issues a swap by writing ODSY_SWAP_ARM, a toggle mask with one
 * bit per window whose displayed buffer must flip at the next retrace, and
 * then waits for the retrace ISR. That ISR reads ODSY_DISPLAYED_BUFFER as a
 * per-window bitmask and completes window w's swap only once bit w matches
 * the buffer it asked for. So at each retrace the board first latches the
 * pending flips into the displayed-buffer bits and then raises the retrace
 * source, whose vector is byte 23:16 of ODSY_ROUTE_VECTORS.
 *
 * The retrace interrupt fires at every retrace once the routing is
 * programmed, not only when a swap is armed: the kernel driver's
 * vertical-retrace handler is where the driver's vblank counters advance,
 * and a swap with a non-zero interval is deferred until those counters
 * reach its target and then issued from the ISR itself. Before the driver
 * programs the routing nothing is delivered. The driver programs no
 * retrace-specific enable bit that any available source shows, so whether
 * the board can mask this source is unknown and it is left unmasked.
 *
 * The flip changes which colour buffer the window's pixels show
 * (odyssey_resolve_row()), so drawing into the buffer a window does not show
 * stays hidden until the retrace that latches its swap, and repeated swaps
 * alternate the two buffers.
 *
 * APPROXIMATION: the board's encoding of ODSY_DISPLAYED_BUFFER is inferred
 * from the driver's compare, not independently measured, and the latch's
 * timing within the retrace is unknown. The model latches at its retrace
 * callback and repaints the whole frame from the new selection; a frame is
 * never presented half from each buffer.
 */
static void odyssey_retrace(void *opaque)
{
    SGIOdysseyState *s = opaque;
    uint32_t flips = s->swap_arm;

    if (flips) {
        s->swap_arm = 0;
        s->displayed_buffer ^= flips;
        trace_sgi_odyssey_swap_latch(flips, s->displayed_buffer);
        sgi_scanout_invalidate_all(&s->scanout);
    }
    if (s->route_dest_upper || s->route_dest_lower) {
        odyssey_deliver_routed(s, ODSY_ROUTE_VECTOR_RETRACE_SHIFT, "retrace");
    }
}

static const SGIHeadOps odyssey_head_ops = {
    .publish = odyssey_publish,
    .resolve_row = odyssey_resolve_row,
    .dma_chunk = odyssey_dma_chunk,
    .retrace = odyssey_retrace,
};

/* Rebuild the kept (non-empty) clip rectangles from the raw 0x204b words. */
static void odyssey_clip_update(SGIOdysseyState *s)
{
    unsigned int i;

    s->clip_kept = 0;
    for (i = 0; i < SGI_ODYSSEY_CLIP_RECTS; i++) {
        uint32_t lo = s->clip_words[2 * i], hi = s->clip_words[2 * i + 1];
        SGIClipRect r = {
            .x0 = (lo >> ODSY_CLIP_X_SHIFT) & ODSY_CLIP_FIELD_MASK,
            .y0 = lo & ODSY_CLIP_FIELD_MASK,
            .x1 = (hi >> ODSY_CLIP_X_SHIFT) & ODSY_CLIP_FIELD_MASK,
            .y1 = hi & ODSY_CLIP_FIELD_MASK,
        };

        if (r.x0 <= r.x1 && r.y0 <= r.y1) {
            s->clip_rect[s->clip_kept++] = r;
        }
    }
}

/*
 * The plane and drawable-id test (the window-placement comment at
 * ODSY_REG_WINDOW_ORIGIN): inside the plane and, with the drawable-id
 * compare enabled, over an id-plane byte that matches its mask and value.
 * The plane itself drops an out-of-plane store, but the family checks first
 * so the shared code never has to guess whose error it is (head-ops.h).
 * odyssey_pixel_allowed() below adds the window-clip rectangles for every
 * write except a copy destination, which calls this helper directly
 * (odyssey_copy_rect_buffer()).
 */
static bool odyssey_pixel_in_bounds(SGIOdysseyState *s, int64_t x, int64_t y)
{
    if (x < 0 || y < 0 || x >= SGI_ODYSSEY_WIDTH || y >= SGI_ODYSSEY_HEIGHT) {
        return false;
    }
    if (s->did_compare & ODSY_DID_COMPARE_ENABLE) {
        uint8_t mask = s->did_compare >> ODSY_DID_COMPARE_MASK_SHIFT;
        uint8_t value = s->did_compare;

        if ((s->did[y * SGI_ODYSSEY_WIDTH + x] & mask) != value) {
            return false;
        }
    }
    return true;
}

/*
 * The one pixel test every write but a copy destination passes: the plane
 * and drawable-id test above, then inside at least one kept clip rectangle
 * -- with none kept nothing is drawn.
 */
static bool odyssey_pixel_allowed(SGIOdysseyState *s, int64_t x, int64_t y)
{
    unsigned int i;

    if (!odyssey_pixel_in_bounds(s, x, y)) {
        return false;
    }
    for (i = 0; i < s->clip_kept; i++) {
        const SGIClipRect *r = &s->clip_rect[i];

        if (x >= r->x0 && x <= r->x1 && y >= r->y0 && y <= r->y1) {
            return true;
        }
    }
    return false;
}

/* A colour write, into each buffer the drawing mask selects. */
static void odyssey_paint_pixel(SGIOdysseyState *s, int64_t x, int64_t y,
                                uint32_t pixel)
{
    unsigned buffers = odyssey_draw_buffers(s);

    if (!odyssey_pixel_allowed(s, x, y)) {
        return;
    }
    for (unsigned b = 0; b < ODSY_BUFFERS; b++) {
        if (buffers & (1u << b)) {
            odyssey_buffer_store(s, b, x, y, pixel);
        }
    }
}

/*
 * The plane word a fill stores: the packed RGB pixel of an RGB fill, or the
 * raw index of a colour-index fill (ODSY_INDEX_MASK). No colormap is applied
 * here: scanout resolves each pixel through its own drawable
 * (odyssey_resolve_row()).
 */
static uint32_t odyssey_fill_pixel(bool is_index, uint32_t value)
{
    return is_index ? value & ODSY_INDEX_MASK : value;
}

/*
 * odyssey_fillrect(): BEGIN(QUADS)/COLOR_3UB or COLOR_INDEX/four VERTEX_2I
 * corners/END drives the solid rectangle fill. odyssey_gl_end() routes a
 * QUADS fill here when pattern mode is off, and to odyssey_pattern_fill()
 * below when it is on. is_index selects whether value is a packed RGB pixel
 * or a PseudoColor index resolved per pixel.
 */
static void odyssey_fill_rect(SGIOdysseyState *s, uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h, bool is_index,
                              uint32_t value)
{
    uint32_t row, col;

    for (row = 0; row < h; row++) {
        for (col = 0; col < w; col++) {
            odyssey_paint_pixel(s, x + col, y + row,
                                odyssey_fill_pixel(is_index, value));
        }
    }
    sgi_scanout_invalidate_all(&s->scanout);
}

/*
 * odyssey_clear_planes(): the X server's drawable-id plane write. For each
 * pixel of the rectangle, replace the plane bits the 0x1023 mask selects with
 * the id value the COLOR_INDEX word carried, leaving the colour framebuffer
 * untouched (docs/analysis/odyssey-account-panel-black-band.md).
 */
static void odyssey_clear_planes(SGIOdysseyState *s, uint32_t x, uint32_t y,
                                 uint32_t w, uint32_t h, uint8_t value,
                                 uint8_t mask)
{
    uint32_t row, col;

    for (row = 0; row < h; row++) {
        for (col = 0; col < w; col++) {
            uint32_t px = x + col, py = y + row;
            uint8_t *slot;

            if (!odyssey_pixel_allowed(s, px, py)) {
                continue;
            }
            slot = &s->did[py * SGI_ODYSSEY_WIDTH + px];
            *slot = (*slot & ~mask) | (value & mask);
        }
    }
    /* A pixel's drawable picks its colour at scanout. */
    sgi_scanout_invalidate_all(&s->scanout);
}

/*
 * odyssey_copyrect()'s screen-to-screen blit, the console-scroll
 * primitive. Copies in whichever row/column direction keeps a read ahead
 * of the matching write, so an overlapping source and destination in the
 * same framebuffer -- the case a scroll relies on -- reads the old
 * contents before they are overwritten.
 *
 * APPROXIMATION: which buffer the board reads the source from is part of
 * the uncleared read selection (ODSY_REG_DRAW_MASK), so the copy runs
 * within each buffer the drawing mask selects, source and destination in
 * the same buffer.
 *
 * APPROXIMATION, INFERRED: a destination pixel passes odyssey_pixel_in_bounds()
 * only, not the window-clip rectangles odyssey_pixel_allowed() also tests
 * (the window-placement comment at ODSY_REG_WINDOW_ORIGIN). The plane bound
 * and the drawable-id compare still apply.
 */
static void odyssey_copy_rect_buffer(SGIOdysseyState *s, unsigned source,
                                     unsigned dest)
{
    uint32_t sx = s->copy_src & 0xffff;
    uint32_t sy = (s->copy_src >> 16) & 0xffff;
    uint32_t w = s->copy_size & 0xffff;
    uint32_t h = (s->copy_size >> 16) & 0xffff;
    uint32_t dx = s->copy_dst & 0xffff;
    uint32_t dy = (s->copy_dst >> 16) & 0xffff;
    uint32_t w_clip = dx < SGI_ODYSSEY_WIDTH ?
                      MIN(w, SGI_ODYSSEY_WIDTH - dx) : 0;
    uint32_t h_clip = dy < SGI_ODYSSEY_HEIGHT ?
                      MIN(h, SGI_ODYSSEY_HEIGHT - dy) : 0;
    bool reverse_rows = dy > sy;
    bool reverse_cols = dx > sx;
    uint32_t row, col;

    for (row = 0; row < h_clip; row++) {
        uint32_t sr = reverse_rows ? h_clip - 1 - row : row;

        for (col = 0; col < w_clip; col++) {
            uint32_t sc = reverse_cols ? w_clip - 1 - col : col;
            /* An out-of-plane source reads zero, as the guarded read did. */
            uint32_t pixel = odyssey_buffer_load(s, source, sx + sc, sy + sr);

            if (odyssey_pixel_in_bounds(s, dx + sc, dy + sr)) {
                odyssey_buffer_store(s, dest, dx + sc, dy + sr, pixel);
            }
        }
    }
}

static bool odyssey_copy_rect_to_object(SGIOdysseyState *s);

static void odyssey_copy_rect(SGIOdysseyState *s)
{
    unsigned source = (s->cmd_operand[2] >> ODSY_COPY_RECT_SOURCE_SHIFT) & 0xf;
    unsigned dest = (s->cmd_operand[7] >> ODSY_COPY_RECT_DEST_SHIFT) & 0xf;
    unsigned format_source =
        (s->cmd_operand[4] >> ODSY_COPY_RECT_FORMAT_SOURCE_SHIFT) & 0xf;
    unsigned format_dest =
        (s->cmd_operand[4] >> ODSY_COPY_RECT_DEST_SHIFT) & 0xf;

    if (s->cmd_operand[0] == ODSY_DMA_SENTINEL_READ) {
        if (!odyssey_copy_rect_to_object(s)) {
            odyssey_cmd_unimplemented(s, ODSY_CMD_COPY_RECT_TRIGGER);
        }
        return;
    }

    if (source != format_source || dest != format_dest) {
        odyssey_cmd_unimplemented(s, ODSY_CMD_COPY_RECT_TRIGGER);
        return;
    }

    /*
     * The two observed copy blocks select 0 then 4 in both their source
     * and destination fields. This is an experimental model policy: the
     * selector values choose model buffers 0 and 1, but do not identify
     * physical V12 memory banks. Keep the source and destination choices
     * separate so a copy never changes another colour buffer.
     */
    if ((source == 0 || source == 4) && (dest == 0 || dest == 4)) {
        odyssey_copy_rect_buffer(s, source >> 2, dest >> 2);
    } else if (source == dest && (source == 2 || source == 3)) {
        /* Existing public and modeled templates retain draw-mask behavior. */
        unsigned buffers = odyssey_draw_buffers(s);

        for (unsigned b = 0; b < ODSY_BUFFERS; b++) {
            if (buffers & (1u << b)) {
                odyssey_copy_rect_buffer(s, b, b);
            }
        }
    } else {
        odyssey_cmd_unimplemented(s, ODSY_CMD_COPY_RECT_TRIGGER);
        return;
    }
    sgi_scanout_invalidate_all(&s->scanout);
}

/* Pattern mode is on when the 0x0404 mode's bits 3,4 are set and a shape is
 * selected. A quad drawn in this state paints the current colour through the
 * pattern instead of as a solid rectangle
 * (docs/analysis/odyssey-glyph-raster-format.md). */
static bool odyssey_pattern_enabled(SGIOdysseyState *s)
{
    if ((s->pattern_mode & ODSY_PATTERN_MODE_ENABLE) !=
        ODSY_PATTERN_MODE_ENABLE) {
        return false;
    }
    switch (s->pattern_shape) {
    case ODSY_PATTERN_SHAPE_64X16:
    case ODSY_PATTERN_SHAPE_32X32:
    case ODSY_PATTERN_SHAPE_16X64:
        return true;
    default:
        return false;
    }
}

/*
 * The pattern bit at quad-relative column u, row v, for the current shape.
 * Row 0 column 0 is the quad's top-left vertex and bit 31 of a row's first
 * word is its leftmost pixel. A coordinate past the shape's own extent is
 * treated as clear: the DDX never draws a quad larger than its pattern.
 */
static bool odyssey_pattern_bit(SGIOdysseyState *s, uint32_t u, uint32_t v)
{
    uint32_t word, bit;

    switch (s->pattern_shape) {
    case ODSY_PATTERN_SHAPE_64X16:
        if (u >= 64 || v >= 16) {
            return false;
        }
        word = s->pattern_buffer[2 * v + (u >> 5)];
        bit = 31 - (u & 31);
        break;
    case ODSY_PATTERN_SHAPE_32X32:
        if (u >= 32 || v >= 32) {
            return false;
        }
        word = s->pattern_buffer[v];
        bit = 31 - u;
        break;
    case ODSY_PATTERN_SHAPE_16X64:
        if (u >= 16 || v >= 64) {
            return false;
        }
        word = s->pattern_buffer[v >> 1];
        bit = 31 - 16 * (v & 1) - u;
        break;
    default:
        return false;
    }
    return (word >> bit) & 1;
}

/*
 * A pattern quad: paint the current colour at each pixel whose pattern bit is
 * set, leaving the rest of the rectangle as it was. Only the rows and columns
 * the quad covers are read, so the stale part of the persistent pattern buffer
 * beyond the tile never contributes.
 */
static void odyssey_pattern_fill(SGIOdysseyState *s, uint32_t x, uint32_t y,
                                 uint32_t w, uint32_t h, uint32_t pat_u0,
                                 uint32_t pat_v0, bool is_index,
                                 uint32_t value)
{
    uint32_t u, v;

    for (v = 0; v < h; v++) {
        for (u = 0; u < w; u++) {
            if (odyssey_pattern_bit(s, pat_u0 + u, pat_v0 + v)) {
                odyssey_paint_pixel(s, x + u, y + v,
                                    odyssey_fill_pixel(is_index, value));
            }
        }
    }
    sgi_scanout_invalidate_all(&s->scanout);
}

/*
 * A raster-box coordinate arrives as an IEEE-754 single the DDX built with
 * cvt.s.w, so the 32-bit operand is a float bit pattern, not an integer.
 * Truncate toward zero; a NaN or negative clamps to the screen origin.
 */
static uint32_t odyssey_float_to_u32(uint32_t bits)
{
    float f;

    memcpy(&f, &bits, sizeof(f));
    if (!(f > 0.0f)) {
        return 0;
    }
    return (uint32_t)f;
}

/* The index of the object at board base base, or -1. */
static int odyssey_object_find(SGIOdysseyState *s, uint32_t base)
{
    for (unsigned int i = 0; i < s->obj_count; i++) {
        if (s->obj_base[i] == base) {
            return i;
        }
    }
    return -1;
}

/*
 * The object at base, created with the given row length in pixels when it
 * does not exist; a new row length for an existing object discards its
 * storage (its layout changed). -1, reported once, when the table is full.
 */
static int odyssey_object_select(SGIOdysseyState *s, uint32_t base,
                                 uint32_t width)
{
    int i = odyssey_object_find(s, base);

    if (i < 0) {
        if (s->obj_count == SGI_ODYSSEY_OBJECTS) {
            if (!s->obj_full_reported) {
                s->obj_full_reported = true;
                qemu_log_mask(LOG_UNIMP, "sgi-odyssey: more than %u "
                              "board-memory objects; object 0x%x dropped\n",
                              SGI_ODYSSEY_OBJECTS, base);
            }
            return -1;
        }
        i = s->obj_count++;
        s->obj_base[i] = base;
        s->obj_width[i] = width;
        s->obj_offset[i] = 0;
        s->obj_rows[i] = 0;
    } else if (width && s->obj_width[i] != width) {
        s->obj_width[i] = width;
        s->obj_rows[i] = 0;
    }
    return i;
}

/*
 * Make room for rows rows in object i: grow it in place when it is the last
 * in the store, else move it to the end. The space a moved object leaves is
 * not reused. False, reported once, when the store's budget is spent.
 */
static bool odyssey_object_rows(SGIOdysseyState *s, int i, uint32_t rows)
{
    uint64_t need = (uint64_t)s->obj_width[i] * rows;
    uint64_t have = (uint64_t)s->obj_width[i] * s->obj_rows[i];

    if (rows <= s->obj_rows[i]) {
        return true;
    }
    if (have && s->obj_offset[i] + have == s->obj_used &&
        s->obj_offset[i] + need <= ODSY_OBJ_POOL_WORDS) {
        s->obj_used = s->obj_offset[i] + need;
    } else if (s->obj_used + need <= ODSY_OBJ_POOL_WORDS) {
        uint8_t *bytes = s->obj_mem.bytes;

        memmove(bytes + (uint64_t)s->obj_used * 4,
                bytes + (uint64_t)s->obj_offset[i] * 4, have * 4);
        memset(bytes + ((uint64_t)s->obj_used + have) * 4, 0,
               (need - have) * 4);
        s->obj_offset[i] = s->obj_used;
        s->obj_used += need;
    } else {
        if (!s->obj_full_reported) {
            s->obj_full_reported = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: board-memory object 0x%x "
                          "(%u x %u) exceeds the model's %u MiB store\n",
                          s->obj_base[i], s->obj_width[i], rows,
                          (unsigned)(ODSY_OBJ_POOL_BYTES / MiB));
        }
        return false;
    }
    s->obj_rows[i] = rows;
    return true;
}

/* Object i's texel (x, y) as 0xRRGGBBAA; outside its rows or width, dropped. */
static void odyssey_object_store(SGIOdysseyState *s, int i, uint32_t x,
                                 uint32_t y, uint32_t rgba)
{
    if (x >= s->obj_width[i] || !odyssey_object_rows(s, i, y + 1)) {
        return;
    }
    stl_be_p(s->obj_mem.bytes +
             ((uint64_t)s->obj_offset[i] + (uint64_t)y * s->obj_width[i] + x) *
             4, rgba);
}

static uint32_t odyssey_object_load(SGIOdysseyState *s, int i, uint32_t x,
                                    uint32_t y)
{
    if (x >= s->obj_width[i] || y >= s->obj_rows[i]) {
        return 0;
    }
    return ldl_be_p(s->obj_mem.bytes +
                    ((uint64_t)s->obj_offset[i] +
                     (uint64_t)y * s->obj_width[i] + x) * 4);
}

/* Copy a recognized GL read-drawable rectangle into its scratch object. */
static bool odyssey_copy_rect_to_object(SGIOdysseyState *s)
{
    uint32_t w = s->copy_size & 0xffff;
    uint32_t h = s->copy_size >> 16;
    uint32_t dx = s->copy_dst & 0xffff;
    uint32_t dy = s->copy_dst >> 16;
    int64_t sx = (s->copy_src & 0xffff) + (int64_t)s->read_origin[0];
    int64_t sy = (s->copy_src >> 16) + (int64_t)s->read_origin[1];
    uint32_t fmt = s->cmd_operand[4];
    uint32_t tail = s->cmd_operand[7];
    bool rgba8 = fmt == 0x92233002 && tail == 0x3801210b;
    bool rgba16 = (fmt & ~ODSY_DMA_FMT_FRONT) == 0x12633042 &&
                  (tail == 0x3802204b || tail == 0xf802204b);
    int obj = odyssey_object_find(s, s->cmd_operand[5]);

    if (s->cmd_operand[0] != ODSY_DMA_SENTINEL_READ ||
        s->cmd_operand[2] != 0x80200050 ||
        (!rgba8 && !rgba16) ||
        s->cmd_operand[5] != s->dest_base || obj < 0 || !w || !h ||
        (uint64_t)dx + w > s->obj_width[obj] ||
        sx < 0 || sy < 0 ||
        sx + w > SGI_ODYSSEY_WIDTH ||
        sy + h > SGI_ODYSSEY_HEIGHT ||
        !odyssey_object_rows(s, obj, dy + h)) {
        return false;
    }
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            unsigned shown = (s->displayed_buffer >>
                              odyssey_main_id(s, sx + x, sy + y)) & 1;
            unsigned source = fmt & ODSY_DMA_FMT_FRONT ? shown : !shown;

            odyssey_object_store(s, obj, dx + x, dy + y,
                                 odyssey_buffer_load(s, source, sx + x,
                                                     sy + y));
        }
    }
    return true;
}

/*
 * The object an image transfer writes, or -1 for the screen: the last 0x1017
 * select, when it named a base other than the colour buffer's.
 */
static int odyssey_image_object(SGIOdysseyState *s)
{
    if (!s->dest_base || s->dest_base == s->dma_colour_base ||
        s->dest_buffer_word == ODSY_DEST_ID_PLANES) {
        return -1;
    }
    return odyssey_object_find(s, s->dest_base);
}

/*
 * Lay one image pixel, as 0xRRGGBBAA, at the raster cursor in the selected
 * buffer (odyssey_image_object()) and advance it. Each axis is a directed
 * half-open interval: increasing endpoints visit [first, last), while
 * decreasing endpoints visit [first - 1, last]. Payload advances along X,
 * then along Y. A cleared pixel (paint false) still advances the cursor but
 * leaves the framebuffer, so a transparent index shows what was drawn below.
 */
static void odyssey_image_put(SGIOdysseyState *s, bool paint, uint32_t rgba,
                              bool is_index, bool preserve_alpha)
{
    bool x_decreasing = s->image_x2 < s->image_x1;
    bool y_decreasing = s->image_y2 < s->image_y1;
    bool y_finished = y_decreasing ?
        s->image_cur_y == UINT32_MAX || s->image_cur_y < s->image_y2 :
        s->image_cur_y >= s->image_y2;
    int obj;

    if (s->image_x2 == s->image_x1 || s->image_y2 == s->image_y1 ||
        y_finished) {
        return;
    }
    obj = odyssey_image_object(s);
    if (paint && obj >= 0) {
        /* An off-screen object: box coordinates, no window or id tests. */
        odyssey_object_store(s, obj, s->image_cur_x, s->image_cur_y, rgba);
    } else if (paint) {
        /* Screen pixels are window-relative; object pixels are not. */
        odyssey_paint_pixel(s,
                            (int64_t)s->image_cur_x + s->win_origin[0],
                            (int64_t)s->image_cur_y + s->win_origin[1],
                            is_index || preserve_alpha ? rgba :
                            (rgba & ~0xffu) | 0xff);
    }
    if ((x_decreasing && s->image_cur_x == s->image_x2) ||
        (!x_decreasing && s->image_cur_x == s->image_x2 - 1)) {
        s->image_cur_x = x_decreasing ? s->image_x1 - 1 : s->image_x1;
        if (y_decreasing) {
            s->image_cur_y = s->image_cur_y == s->image_y2 ? UINT32_MAX :
                             s->image_cur_y - 1;
        } else {
            s->image_cur_y++;
        }
    } else if (x_decreasing) {
        s->image_cur_x--;
    } else {
        s->image_cur_x++;
    }
    sgi_scanout_invalidate_all(&s->scanout);
}

/*
 * One byte-stream texel of the open image at the raster cursor, bounded by
 * the header's group length in pixels.
 */
static void odyssey_image_texel(SGIOdysseyState *s, bool paint, uint32_t rgba,
                                bool is_index)
{
    if (s->image_group_len &&
        s->image_group_painted >= s->image_group_len) {
        return;
    }
    odyssey_image_put(s, paint, rgba, is_index, false);
    s->image_group_painted++;
}

/*
 * Decode one pixel of a byte-stream source format (ODSY_IMAGE_CODE_*) whose
 * bytes are b[0 ..]: its 0xRRGGBBAA word, or for 0x41 the raw index, and
 * whether it paints (a colour
 * index 0 does not; see odyssey_image_burst_word()). False for a code this
 * model does not know.
 */
static bool odyssey_decode_pixel(SGIOdysseyState *s, uint32_t code,
                                 const uint8_t *b, uint32_t *rgba, bool *paint)
{
    *paint = true;
    switch (code) {
    case ODSY_IMAGE_CODE_XRGB:
        *rgba = odyssey_pack_pixel(b[1], b[2], b[3]);
        return true;
    case ODSY_IMAGE_CODE_RGBA:
    case ODSY_IMAGE_CODE_RGBA12:
        *rgba = ldl_be_p(b);
        return true;
    case ODSY_IMAGE_CODE_RGB:
        *rgba = odyssey_pack_pixel(b[0], b[1], b[2]);
        return true;
    case ODSY_IMAGE_CODE_LUM:
        *rgba = odyssey_pack_pixel(b[0], b[0], b[0]);
        return true;
    case ODSY_IMAGE_CODE_LUM_ALPHA:
        *rgba = (odyssey_pack_pixel(b[0], b[0], b[0]) & ~0xffu) | b[1];
        return true;
    case ODSY_IMAGE_CODE_CI:
        /* The raw index: scanout resolves it through the pixel's drawable. */
        *paint = b[0] != 0;
        *rgba = b[0];
        return true;
    default:
        return false;
    }
}

/* Bytes a pixel of a source format takes, 0 for an unknown code. */
static unsigned odyssey_pixel_bytes(uint32_t code)
{
    switch (code) {
    case ODSY_IMAGE_CODE_XRGB:
    case ODSY_IMAGE_CODE_RGBA:
    case ODSY_IMAGE_CODE_RGBA12:
        return 4;
    case ODSY_IMAGE_CODE_RGB:
        return 3;
    case ODSY_IMAGE_CODE_LUM:
    case ODSY_IMAGE_CODE_CI:
        return 1;
    case ODSY_IMAGE_CODE_LUM_ALPHA:
        return 2;
    default:
        return 0;
    }
}

/*
 * One data word of a pixel burst, laid out by the transfer header's format
 * code (ODSY_IMAGE_CODE_*): its bytes join a stream that yields a pixel
 * every odyssey_pixel_bytes() bytes, so a three-byte pixel may span two
 * words. A bounded group's trailing padding (past image_group_len) is
 * dropped with any partial pixel, so the raster position stays exact. A
 * header with a code this model does not know falls back on the burst tag's
 * format, 0x11 one 0x00RRGGBB pixel a word and 0x10 four indices, and is
 * reported once.
 *
 * Approximation: colour index 0 in an image is the text-field caret's erase
 * (a 6x20 all-zeros block the DDX writes before redrawing the I-beam). With
 * plain overwrite it would paint opaque black over the mauve input field; the
 * write mode that makes the erase-then-redraw correct is not established
 * (docs/analysis/odyssey-glyph-raster-format.md). Leave index-0 image pixels
 * unpainted rather than stamp black -- the field keeps the colour its fill
 * drew, and the black I-beam still lands as its own pattern quad.
 */
static bool odyssey_image_pairing_supported(uint32_t code, uint32_t fmt)
{
    if (code == ODSY_IMAGE_CODE_RGBA12) {
        return fmt == ODSY_IMAGE_FMT_RGB || fmt == ODSY_IMAGE_FMT_RGBA16;
    }
    if (code == ODSY_IMAGE_CODE_LUM_ALPHA) {
        return fmt == ODSY_IMAGE_FMT_CI;
    }
    return fmt != ODSY_IMAGE_FMT_RGBA16;
}

static void odyssey_image_burst_word(SGIOdysseyState *s, uint32_t word)
{
    uint32_t code = s->image_code;
    unsigned int n = odyssey_pixel_bytes(code), i;
    uint8_t b[4];

    if (!odyssey_image_pairing_supported(code, s->image_fmt)) {
        return;
    }
    if (s->image_fmt == ODSY_IMAGE_FMT_RGBA16) {
        uint32_t rgba;
        uint16_t r, g, b16, a;

        if (s->image_group_len &&
            s->image_group_painted >= s->image_group_len) {
            return;
        }
        if (s->image_nbytes) {
            r = s->image_bytes >> 16;
            g = s->image_bytes;
            b16 = word >> 16;
            a = word;
            rgba = odyssey_pack_pixel(
                ((uint32_t)r * 255 + UINT16_MAX / 2) / UINT16_MAX,
                ((uint32_t)g * 255 + UINT16_MAX / 2) / UINT16_MAX,
                ((uint32_t)b16 * 255 + UINT16_MAX / 2) / UINT16_MAX);
            rgba = (rgba & ~0xffu) |
                   (((uint32_t)a * 255 + UINT16_MAX / 2) / UINT16_MAX);
            s->image_bytes = 0;
            s->image_nbytes = 0;
            odyssey_image_put(s, true, rgba, false, true);
        } else {
            s->image_bytes = word;
            s->image_nbytes = 1;
        }
        s->image_group_painted++;
        if (s->image_group_len &&
            s->image_group_painted >= s->image_group_len &&
            s->image_nbytes) {
            s->image_bytes = 0;
            s->image_nbytes = 0;
        }
        return;
    }

    if (!n) {
        if (!s->image_code_reported) {
            s->image_code_reported = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: image source format 0x%x "
                          "is not modeled; laid out by the burst tag\n",
                          code);
        }
        code = s->image_fmt == ODSY_IMAGE_FMT_RGB ? ODSY_IMAGE_CODE_XRGB :
               ODSY_IMAGE_CODE_CI;
        n = odyssey_pixel_bytes(code);
    }
    stl_be_p(b, word);
    for (i = 0; i < 4; i++) {
        uint8_t px[4];
        uint32_t rgba;
        bool paint;

        s->image_bytes = (s->image_bytes << 8) | b[i];
        if (++s->image_nbytes < n) {
            continue;
        }
        s->image_nbytes = 0;
        /* The pixel's bytes, first byte highest, at the start of px[]. */
        stl_be_p(px, s->image_bytes << (8 * (4 - n)));
        odyssey_decode_pixel(s, code, px, &rgba, &paint);
        odyssey_image_texel(s, paint, rgba, code == ODSY_IMAGE_CODE_CI);
    }
}

/* True, and set *fmt, when reg is a pixel-burst register-select word. */
static bool odyssey_image_burst_fmt(uint32_t reg, uint32_t *fmt)
{
    uint32_t f = reg & ODSY_IMAGE_BURST_FMT_MASK;

    if (!(reg & ODSY_IMAGE_BURST_BASE) ||
        (reg & ODSY_IMAGE_BURST_RESERVED_MASK)) {
        return false;
    }
    switch (f) {
    case ODSY_IMAGE_FMT_CI:
    case ODSY_IMAGE_FMT_RGB:
    case ODSY_IMAGE_FMT_RGBA16:
        *fmt = f;
        return true;
    default:
        /* Other documented pixel-burst formats remain unsupported. */
        return false;
    }
}

/*
 * The family side of the shared OpenGL context (hw/display/sgi/gl-context.h)
 * and software rasterizer (hw/display/sgi/rasterizer.h). It is a thin decoder:
 * odyssey_cmd_dispatch() turns the float-GL words into SGIGLCmds and feeds them
 * through odyssey_gl_feed(); the shared context runs the GL state machine and
 * assembles primitives, emitting each to the sink below as its last vertex
 * arrives. The sink rasterizes into the one colour plane through the raster
 * target. The 2D greeter path (odyssey_gl_end() and friends) is untouched
 * (docs/designs/odyssey-gl-family-side.md).
 *
 * The rasterizer rule set is CHOSEN, PENDING CAPTURE: MGRAS's Octane RE4 set
 * is the only one sgi_raster_rules_supported() implements, and a flat single
 * triangle in one colour exercises none of the parameters that distinguish
 * heads (the fill is colour-constant, so subpixel snapping and interpolation
 * do not change which pixels take the colour, and there is no depth or line
 * work). Odyssey's own subpixel grid and interpolated-colour depth are
 * unmeasured; an Odyssey rule set of its own waits on a hardware capture
 * (docs/designs/odyssey-gl-family-side.md, "The rasterizer rule set").
 */
static const SGIRasterRules odyssey_gl_rules = {
    .subpixel_bits = 8,
    .color_bits = 8,
    .depth_bits = 24,
    .rounding = SGI_ROUND_HALF_AWAY,
    .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
    .arith = SGI_ARITH_DOUBLE,
    .line_rule = SGI_LINE_COVERAGE_DOUBLE,
    .evidence = "chosen, pending capture: borrowed from MGRAS (Octane RE4); "
                "Odyssey's subpixel grid and GL colour depth are unmeasured "
                "(odyssey-gl-family-side.md)",
};

/* Whether a GL fragment's colour is an index (ODSY_PROG_COLOUR_MODE). */
static bool odyssey_gl_index_mode(SGIOdysseyState *s)
{
    return s->colour_mode[0] & ODSY_COLOUR_MODE_INDEX;
}

/*
 * A colour index carried through the shared rasterizer, which knows only
 * RGBA: bits 7:0 of the index ride in blue and bits 11:8 in green, so at the
 * rules' eight colour bits the stored colour's low twelve bits are the index
 * again (odyssey_gl_store_colour()). Smooth shading then interpolates the
 * index as GL does, within one run of 256.
 */
static void odyssey_gl_index_rgba(uint32_t index, float rgba[4])
{
    rgba[0] = 0;
    rgba[1] = ((index >> 8) & 0xf) / 255.0f;
    rgba[2] = (index & 0xff) / 255.0f;
    rgba[3] = 1;
}

/*
 * A float index (glIndexf, glClearIndex): its integer part, wrapped to the
 * twelve index bits the way a write keeps only the buffer's bits.
 */
static uint32_t odyssey_float_index(uint32_t bits)
{
    float f;

    memcpy(&f, &bits, sizeof(f));
    if (!isfinite(f)) {
        return 0;
    }
    /*
     * The integer part, wrapped to the buffer's twelve bits without an
     * out-of-range conversion: fmodf is exact.
     */
    f = fmodf(floorf(f), 4096.0f);
    if (f < 0) {
        f += 4096.0f;
    }
    return (uint32_t)f & ODSY_INDEX_MASK;
}

/*
 * Synthetic RGB carries the index, so ordinary channel masks do not apply.
 * 0x452 bits 11:0, the index write mask proper, are decoded but never
 * applied here (see the field's own comment): no retained capture shows
 * what a real colour-index drawable sends there, so there is no basis for
 * a masking rule to implement. Whenever the field reads as anything but
 * all-ones -- GL's own "fully open" write mask, and not the board's own
 * reset value of 0 -- that gap is reported once rather than left silent.
 */
static void odyssey_gl_store_colour(void *opaque, SGIColourTarget target,
                                    int x, int y, uint32_t colour)
{
    SGIOdysseyState *s = opaque;
    uint32_t mask = (s->gl_write_mask >> ODSY_WRITE_COLOUR_SHIFT) &
                    ODSY_WRITE_COLOUR_MASK;
    uint32_t index_mask = s->gl_write_mask & ODSY_WRITE_INDEX_MASK;

    if (index_mask != ODSY_WRITE_INDEX_MASK && !s->gl_index_mask_logged) {
        s->gl_index_mask_logged = true;
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: index write mask 0x%03x is "
                      "decoded but not applied\n", index_mask);
    }
    if (mask && target < ODSY_BUFFERS && odyssey_pixel_allowed(s, x, y)) {
        odyssey_buffer_store(s, target, x, y, colour & ODSY_INDEX_MASK);
    }
}

/* The shared renderer supplies all four already quantized channel bytes. */
static void odyssey_gl_store_rgba(void *opaque, SGIColourTarget target,
                                  int x, int y, const SGIRasterColour *colour)
{
    SGIOdysseyState *s = opaque;
    const uint32_t *c = colour->component;

    if (target < ODSY_BUFFERS && odyssey_pixel_allowed(s, x, y)) {
        odyssey_buffer_store(s, target, x, y,
                             (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3]);
    }
}

static bool odyssey_gl_load_rgba(void *opaque, SGIColourTarget target,
                                 int x, int y, SGIRasterColour *colour)
{
    SGIOdysseyState *s = opaque;
    SGIBoardMemory *mem;
    const SGIPlane *p;
    uint64_t offset, at, limit;
    uint32_t word;

    if (target >= ODSY_BUFFERS) {
        return false;
    }
    mem = odyssey_buffer_mem(s, target);
    p = odyssey_buffer_plane(s, target);
    limit = MIN(mem->size, mem->bytes_len);
    /* Invalid storage reads as zero through sgi_plane_load(), not success. */
    if (!p || !mem->bytes || !sgi_plane_contains(p, x, y)) {
        return false;
    }
    offset = (uint64_t)y * p->stride + (uint64_t)x * sizeof(uint32_t);
    if (offset > p->len || sizeof(uint32_t) > p->len - offset ||
        uadd64_overflow(p->base, offset, &at) || at > limit ||
        sizeof(uint32_t) > limit - at) {
        return false;
    }
    word = sgi_plane_load(mem, p, x, y);
    for (unsigned i = 0; i < 4; i++) {
        colour->component[i] = (word >> (24 - 8 * i)) & 0xff;
    }
    return true;
}

/* Stored A describes the model's fourth byte, not physical V12 precision. */
static SGIRasterRules odyssey_gl_operation_rules(SGIOdysseyState *s)
{
    SGIRasterRules rules = odyssey_gl_rules;

    rules.alpha_bits = odyssey_gl_index_mode(s) ? 0 : 8;
    return rules;
}

/*
 * The depth plane (odyssey_realize()): 24-bit depth, 0 nearest, through the
 * same pixel test as a colour write. It is a model plane because the board
 * memory the real depth buffer occupies is not known.
 */
static uint32_t odyssey_gl_load_depth(void *opaque, int x, int y)
{
    SGIOdysseyState *s = opaque;

    return sgi_plane_load(&s->depth_mem,
                          sgi_board_memory_plane(&s->depth_mem,
                                                 s->depth_plane), x, y);
}

static void odyssey_gl_store_depth(void *opaque, int x, int y, uint32_t depth)
{
    SGIOdysseyState *s = opaque;

    if (odyssey_pixel_allowed(s, x, y)) {
        sgi_plane_store(&s->depth_mem,
                        sgi_board_memory_plane(&s->depth_mem, s->depth_plane),
                        x, y, depth & ODSY_DEPTH24_MAX);
    }
}

static double odyssey_f32(uint32_t bits);

/* Experimental policy; the board's scale sign and clamps are unmeasured. */
static float odyssey_gl_fog_factor(void *opaque, float window_z, float eye_z)
{
    SGIOdysseyState *s = opaque;
    const uint32_t *w = s->gl_fog;
    double scale = (w[2] & 0xffffff) ?
                   fabs(odyssey_f32(((w[2] & 0xffffff) << 5) +
                                   (96u << 23))) : 0;
    double t = ldexp(scale * window_z +
                     sextract32(w[3], 0, 12) / 256.0,
                     (w[3] >> 12) & 7);
    double p[4] = {
        sextract32(w[4] >> 12, 0, 12) / 256.0,
        sextract32(w[4], 0, 12) / 256.0,
        sextract32(w[5] >> 12, 0, 12) / 256.0,
        sextract32(w[5], 0, 12) / 256.0,
    };
    double u, f;

    (void)eye_z;
    t = t >= 1 ? 1 : t > 0 ? t : 0;
    u = 1 - t;
    f = p[0] * u * u * u + 3 * p[1] * u * u * t +
        3 * p[2] * u * t * t + p[3] * t * t * t;
    return f >= 1 ? 1 : f > 0 ? f : 0;
}

static SGIRasterTarget odyssey_gl_target(SGIOdysseyState *s)
{
    return (SGIRasterTarget) {
        .store_colour = odyssey_gl_index_mode(s) ?
                        odyssey_gl_store_colour : NULL,
        .store_rgba = odyssey_gl_index_mode(s) ? NULL : odyssey_gl_store_rgba,
        .load_rgba = odyssey_gl_index_mode(s) ? NULL : odyssey_gl_load_rgba,
        .load_depth = odyssey_gl_load_depth,
        .store_depth = odyssey_gl_store_depth,
        .fog_factor = odyssey_gl_fog_factor,
        .opaque = s,
    };
}

typedef struct OdysseyTexture {
    SGIOdysseyState *s;
    uint32_t offset, stride, width, height;
    uint32_t base_format;
    uint32_t words;
} OdysseyTexture;

typedef struct OdysseyTextureVolume {
    SGIOdysseyState *s;
    uint32_t base, stride_units, level_word2;
    uint32_t width, height, depth, base_format, words;
} OdysseyTextureVolume;

typedef struct OdysseyTextureChain {
    /*
     * Also supplies the state pointer for the post-filter callback. Keeping
     * this first lets image.opaque serve both that callback and the 3D reader.
     */
    OdysseyTextureVolume volume;
    OdysseyTexture storage[SGI_GL_TEX_LEVELS_MAX];
    SGIGLTexLevel levels[SGI_GL_TEX_LEVELS_MAX];
    SGIGLTexPyramid pyramid;
} OdysseyTextureChain;

static void odyssey_gl_transpose(const uint32_t *rows, float m[16]);
static void odyssey_gl_feed(SGIOdysseyState *s, uint16_t op,
                            const uint32_t *args, uint16_t nargs);

static bool odyssey_texture_read(void *opaque, uint32_t x, uint32_t y,
                                  uint32_t *rgba)
{
    const OdysseyTexture *texture = opaque;
    const SGIOdysseyState *s = texture->s;
    uint64_t word, end;

    if (x >= texture->width || y >= texture->height ||
        umul64_overflow(y, texture->stride, &word) ||
        uadd64_overflow(word, texture->offset, &word) ||
        uadd64_overflow(word, x, &word) ||
        uadd64_overflow(word, 1, &end) || end > texture->words ||
        end > s->obj_mem.bytes_len / 4) {
        return false;
    }
    *rgba = ldl_be_p(s->obj_mem.bytes + word * 4);
    if (texture->base_format == SGI_GL_ALPHA) {
        *rgba = (*rgba >> 24) & 0xff;
    }
    return true;
}

static bool odyssey_texture_read_3d(void *opaque, uint32_t x, uint32_t y,
                                    uint32_t z, uint32_t *rgba)
{
    const OdysseyTextureVolume *volume = opaque;
    SGIOdysseyState *s = volume->s;
    uint64_t slice_offset, base;
    int object;
    OdysseyTexture texture;

    if (x >= volume->width || y >= volume->height || z >= volume->depth ||
        umul64_overflow(volume->stride_units, volume->level_word2,
                        &slice_offset) ||
        umul64_overflow(slice_offset, 64, &slice_offset) ||
        umul64_overflow(z, slice_offset, &slice_offset) ||
        uadd64_overflow(volume->base, slice_offset, &base) ||
        base > UINT32_MAX) {
        return false;
    }
    object = odyssey_object_find(s, base);
    if (object < 0) {
        return false;
    }
    texture = (OdysseyTexture) {
        .s = s, .offset = s->obj_offset[object],
        .stride = s->obj_width[object], .width = volume->width,
        .height = volume->height, .base_format = volume->base_format,
        .words = volume->words,
    };
    return odyssey_texture_read(&texture, x, y, rgba);
}

static bool odyssey_texture_read_level(void *opaque, uint32_t level,
                                       uint32_t x, uint32_t y, uint32_t *rgba)
{
    OdysseyTextureChain *chain = opaque;

    return level < ARRAY_SIZE(chain->storage) &&
           odyssey_texture_read(&chain->storage[level], x, y, rgba);
}

/*
 * The observed L8 path filters its byte texels first, then uses each
 * normalized RGB component as an independent 8-bit index. A half index
 * rounds upward. The fourth table field is retained in raw state but has no
 * established alpha meaning.
 */
static void odyssey_texture_colour_table(void *opaque, double rgba[4])
{
    const OdysseyTextureVolume *texture = opaque;
    const SGIOdysseyState *s = texture->s;

    for (unsigned channel = 0; channel < 3; channel++) {
        uint32_t index = floor(MAX(0.0, MIN(rgba[channel], 1.0)) * 255.0 +
                               0.5);
        unsigned plane = channel == 2;
        uint32_t word = s->tex_colour_table[plane][index];
        uint32_t component = channel == 1 ? word & 0xfff :
                                            (word >> 12) & 0xfff;

        rgba[channel] = component / 4095.0;
    }
}

/* Classify register state before looking up possibly incomplete storage. */
static bool odyssey_texture_setup(SGIOdysseyState *s, SGIGLTexImage *image,
                                   OdysseyTextureChain *chain,
                                   const SGIGLTexImage **resolved)
{
    uint32_t width_word = s->tex_sampler[1], height_word = s->tex_sampler[2];
    uint32_t width = width_word, height = height_word;
    uint32_t depth = s->tex_sampler[3];
    uint32_t word = s->tex_sampler[4], components = word & 0x0f800000;
    bool three_d = s->tex_mode & ODSY_TEX_MODE_3D;
    uint32_t ws = (word >> 12) & 3, wt = (word >> 10) & 3;
    bool clamp = ws == 2 || wt == 2;
    bool border = ws == 3 || wt == 3;
    uint32_t filter = word & 0xe2, min = filter & 0xe0;
    bool colour_table = word == 0x11500001 || word == 0x11500003;
    bool colour_table_disabled = word == 0x11500000;
    uint32_t first = s->tex_level0 & 0x3f, count, terminal;
    int32_t lo = sextract32(s->tex_lod[2], 0, 14);
    int32_t hi = sextract32(s->tex_lod[3], 0, 14);
    static const uint32_t min_filters[] = {
        SGI_GL_NEAREST, SGI_GL_LINEAR,
        SGI_GL_NEAREST_MIPMAP_NEAREST, SGI_GL_LINEAR_MIPMAP_NEAREST,
        SGI_GL_NEAREST_MIPMAP_LINEAR, SGI_GL_LINEAR_MIPMAP_LINEAR,
    };
    uint64_t end;

    *resolved = NULL;
    if (border) {
        width &= 0xffff;
        height &= 0xffff;
    }
    if (odyssey_gl_index_mode(s)) {
        goto unsupported;
    }
    switch (components) {
    case 0x01000000:
        image->base_format = SGI_GL_LUMINANCE;
        break;
    case 0x01800000:
        image->base_format = SGI_GL_LUMINANCE_ALPHA;
        break;
    case 0x00800000:
        image->base_format = SGI_GL_ALPHA;
        break;
    case 0x02000000:
        image->base_format = SGI_GL_INTENSITY;
        break;
    case 0x02800000:
        image->base_format = SGI_GL_RGB;
        break;
    case 0x03000000:
        image->base_format = SGI_GL_RGBA;
        break;
    default:
        goto unsupported;
    }
    if (!width || !height || width > INT32_MAX || height > INT32_MAX ||
        !is_power_of_2(width) || !is_power_of_2(height) ||
        (three_d && (!depth || depth > SGI_ODYSSEY_OBJECTS ||
                     !is_power_of_2(depth) ||
                     (s->tex_mode & (ODSY_TEX_MODE_1D | ODSY_TEX_MODE_2D)))) ||
        (!three_d && depth != 1) ||
        s->tex_sampler[0] != (0x08000000 | (ctz32(width) << 10) |
                              (ctz32(height) << 5) |
                              (three_d ? ctz32(depth) : 0)) ||
        ws > 3 || wt > 3 ||
        (!border && (s->tex_sampler[5] || s->tex_sampler[6])) ||
        (border && (ws != 3 || wt != 3 ||
                    width_word != (0x00fc0000 | width) ||
                    height_word != (0x00fc0000 | height) ||
                    ((s->tex_sampler[5] | s->tex_sampler[6]) &
                     0xff000000))) ||
        ((clamp || border) &&
         ((clamp && (ws != 2 || wt != 2)) ||
          (s->tex_mode & (ODSY_TEX_MODE_1D | ODSY_TEX_MODE_2D |
                          ODSY_TEX_MODE_3D)) != ODSY_TEX_MODE_2D ||
          image->base_format != SGI_GL_RGBA || filter)) ||
        (min != 0 && min != 0x20 && min != 0x40 && min != 0x60 &&
         min != 0x80 && min != 0xa0) ||
        (!colour_table && !colour_table_disabled &&
         word != (0x10000000 | components | (ws << 12) | (wt << 10) |
                  filter)) ||
        s->tex_lod[0] || s->tex_lod[1] ||
        (s->tex_lod[2] | s->tex_lod[3]) & ~0x3fffu ||
        lo < -15 * 256 || lo > 15 * 256 ||
        hi < -15 * 256 || hi > 15 * 256 ||
        (min >= 0x40 && lo > hi) ||
        (colour_table &&
         (image->base_format != SGI_GL_LUMINANCE || three_d ||
          (s->tex_mode & (ODSY_TEX_MODE_1D | ODSY_TEX_MODE_2D |
                          ODSY_TEX_MODE_3D)) != ODSY_TEX_MODE_2D ||
          s->tex_colour_table_valid[0] != 0xffff ||
          s->tex_colour_table_valid[1] != 0xffff)) ||
        (colour_table_disabled &&
         (image->base_format != SGI_GL_LUMINANCE || three_d ||
          (s->tex_mode & (ODSY_TEX_MODE_1D | ODSY_TEX_MODE_2D |
                          ODSY_TEX_MODE_3D)) != ODSY_TEX_MODE_2D)) ||
        s->tex_env[0] < 1 || s->tex_env[0] > 4 ||
        (s->tex_env[0] == 3 &&
         (image->base_format == SGI_GL_ALPHA ||
          image->base_format == SGI_GL_LUMINANCE ||
          image->base_format == SGI_GL_LUMINANCE_ALPHA ||
          image->base_format == SGI_GL_INTENSITY)) ||
        (s->tex_env[0] == 4 &&
         (((s->tex_env[1] | s->tex_env[2]) & 0xff000000) ||
          s->tex_env[3] || s->tex_env[4]))) {
        goto unsupported;
    }
    image->width = width;
    image->height = height;
    image->wrap_s = border ? SGI_GL_CLAMP_TO_BORDER :
                    ws == 2 ? SGI_GL_CLAMP :
                    ws ? SGI_GL_CLAMP_TO_EDGE : SGI_GL_REPEAT;
    image->wrap_t = border ? SGI_GL_CLAMP_TO_BORDER :
                    wt == 2 ? SGI_GL_CLAMP :
                    wt ? SGI_GL_CLAMP_TO_EDGE : SGI_GL_REPEAT;
    if (border) {
        image->border_color[0] = ((s->tex_sampler[5] >> 12) & 0xfff) /
                                 4095.0f;
        image->border_color[1] = (s->tex_sampler[5] & 0xfff) / 4095.0f;
        image->border_color[2] = ((s->tex_sampler[6] >> 12) & 0xfff) /
                                 4095.0f;
        image->border_color[3] = (s->tex_sampler[6] & 0xfff) / 4095.0f;
    }
    image->mag_filter = filter & 2 ? SGI_GL_LINEAR : SGI_GL_NEAREST;
    image->min_filter = min_filters[min >> 5];

    if (three_d) {
        uint32_t *entry = &s->tex_levels[first * 3];
        uint32_t stride_units = entry[1] & 0xfff;
        uint32_t expected_units = ((width + 15) / 16 + 1) & ~1u;
        uint32_t base = entry[0] & 0x1fffffffu;
        uint64_t slice_stride;

        if (first || s->tex_level0 || s->tex_lod[0] || s->tex_lod[1] ||
            s->tex_sampler[4] != 0x13000000 || s->tex_env[0] > 2 ||
            entry[1] != (0x41100000 | stride_units) ||
            entry[2] == 0 || stride_units != expected_units || !base ||
            s->obj_count > SGI_ODYSSEY_OBJECTS ||
            umul64_overflow(stride_units, entry[2], &slice_stride) ||
            umul64_overflow(slice_stride, 64, &slice_stride)) {
            goto unsupported;
        }
        for (uint32_t z = 0; z < depth; z++) {
            uint64_t offset, slice_base, storage_end;
            int object;

            if (umul64_overflow(z, slice_stride, &offset) ||
                uadd64_overflow(base, offset, &slice_base) ||
                slice_base > UINT32_MAX) {
                goto unsupported;
            }
            object = odyssey_object_find(s, slice_base);
            if (object < 0 || s->obj_width[object] != stride_units * 16 ||
                s->obj_rows[object] < height ||
                umul64_overflow(height - 1, s->obj_width[object],
                                &storage_end) ||
                uadd64_overflow(storage_end, s->obj_offset[object],
                                &storage_end) ||
                uadd64_overflow(storage_end, width, &storage_end) ||
                storage_end > s->obj_used ||
                storage_end > s->obj_mem.bytes_len / 4) {
                goto unsupported;
            }
        }
        chain->volume = (OdysseyTextureVolume) {
            .s = s, .base = base, .stride_units = stride_units,
            .level_word2 = entry[2], .width = width, .height = height,
            .depth = depth, .base_format = image->base_format,
            .words = s->obj_used,
        };
        image->depth = depth;
        image->wrap_r = SGI_GL_REPEAT;
        image->read_texel_3d = odyssey_texture_read_3d;
        image->opaque = &chain->volume;
        *resolved = image;
        return true;
    }

    terminal = MAX((s->tex_sampler[0] >> 10) & 31,
                   (s->tex_sampler[0] >> 5) & 31);
    count = min >= 0x40 && hi > 0 ?
            MIN(terminal, (hi + 255) / 256) + 1 : 1;
    if (count > SGI_GL_TEX_LEVELS_MAX ||
        first + count > SGI_ODYSSEY_TEX_LEVELS) {
        goto unsupported;
    }
    memset(chain, 0, sizeof(*chain));
    chain->volume.s = s;
    image->opaque = &chain->volume;
    if (colour_table) {
        image->post_filter = odyssey_texture_colour_table;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t *entry = &s->tex_levels[(first + i) * 3];
        uint32_t level_width = MAX(1, width >> i);
        uint32_t level_height = MAX(1, height >> i);
        uint32_t stride_units = entry[1] & 0xfff;
        uint32_t expected_units = ((level_width + 15) / 16 + 1) & ~1u;
        uint32_t base = entry[0] & 0x1fffffffu;
        int object;

        if ((entry[1] & 0x3c0ff000u) || entry[2] ||
            (i && (entry[1] & 0xc3f00000u) !=
                  (s->tex_levels[first * 3 + 1] & 0xc3f00000u)) ||
            (base && stride_units != expected_units &&
             !(count == 1 && stride_units == 0))) {
            goto unsupported;
        }
        chain->levels[i] = (SGIGLTexLevel) {
            .level = i, .width = level_width, .height = level_height,
            .base_format = image->base_format,
            .internal_format = image->base_format,
            .opaque = chain,
        };
        if (!base || s->obj_count > SGI_ODYSSEY_OBJECTS) {
            continue;
        }
        object = odyssey_object_find(s, base);
        if (object < 0 || s->obj_width[object] < level_width ||
            (stride_units && s->obj_width[object] != stride_units * 16) ||
            s->obj_rows[object] < level_height ||
            umul64_overflow(level_height - 1, s->obj_width[object], &end) ||
            uadd64_overflow(end, s->obj_offset[object], &end) ||
            uadd64_overflow(end, level_width, &end) || end > s->obj_used ||
            end > s->obj_mem.bytes_len / 4) {
            continue;
        }
        chain->storage[i] = (OdysseyTexture) {
            .s = s, .offset = s->obj_offset[object],
            .stride = s->obj_width[object], .width = level_width,
            .height = level_height, .base_format = image->base_format,
            .words = s->obj_used,
        };
        chain->levels[i].read_texel = odyssey_texture_read_level;
    }
    chain->pyramid = (SGIGLTexPyramid) {
        .levels = chain->levels, .count = count, .base_level = 0,
        .max_level = count - 1, .min_lod = lo / 256.0,
        .max_lod = min >= 0x40 ? hi / 256.0 : 15,
    };
    image->pyramid = &chain->pyramid;
    *resolved = image;
    return true;

unsupported:
    qemu_log_mask(LOG_UNIMP, "sgi-odyssey: unsupported texture state "
                  "sampler 0x%x environment %u%s, draw dropped\n",
                  word, s->tex_env[0],
                  odyssey_gl_index_mode(s) ? " colour-index" : "");
    return false;
}

/* Rebuild shared state from family words; no borrowed image survives a draw. */
static void odyssey_texture_vertex(SGIOdysseyState *s)
{
    const uint32_t environments[] = {
        SGI_GL_REPLACE, SGI_GL_REPLACE, SGI_GL_MODULATE, SGI_GL_DECAL,
        SGI_GL_BLEND,
    };
    uint32_t strq[4] = { s->tex_coord[0], s->tex_coord[1],
                         s->tex_coord_r, ODSY_F32_ONE };
    bool one_d = (s->tex_mode & (ODSY_TEX_MODE_1D | ODSY_TEX_MODE_2D)) ==
                 ODSY_TEX_MODE_1D;
    float matrix[16];

    s->gl_ctx.cur.texture_2d = !!(s->tex_mode &
                                  (ODSY_TEX_MODE_1D | ODSY_TEX_MODE_2D));
    s->gl_ctx.cur.texture_3d = !!(s->tex_mode & ODSY_TEX_MODE_3D);
    /* Excluded environments are rejected by the family before rasterization. */
    s->gl_ctx.cur.texture_env_mode = SGI_GL_REPLACE;
    if (s->tex_env[0] < ARRAY_SIZE(environments)) {
        s->gl_ctx.cur.texture_env_mode = environments[s->tex_env[0]];
    }
    odyssey_gl_transpose(s->gl_texture_matrix, matrix);
    /* Input T may transform S, but transformed T cannot affect 1D sampling. */
    if (one_d) {
        for (unsigned i = 0; i < 4; i++) {
            matrix[i * 4 + 1] = 0;
        }
    }
    sgi_gl_set_matrix(&s->gl_ctx, SGI_GL_TEXTURE, matrix);
    odyssey_gl_feed(s, SGI_GL_OP_TEXCOORD4F, strq, ARRAY_SIZE(strq));
}

/*
 * The context emits one assembled primitive here; rasterize it into the
 * plane. Shared between the fire-and-forget and checked sink callbacks
 * below; only the out-of-memory result differs in how it is reported back.
 */
static SGIGLDrawResult odyssey_gl_draw_result(void *opaque,
                                              const SGIGLDraw *d)
{
    SGIOdysseyState *s = opaque;
    SGIRasterTarget t = odyssey_gl_target(s);
    SGIRasterRules rules = odyssey_gl_operation_rules(s);
    SGIGLDrawState state = *d->state;
    SGIGLDraw draw = *d;
    SGIGLTexImage image = { 0 };
    OdysseyTextureChain texture;
    SGIGLDrawResult result;

    if ((d->state->texture_2d || d->state->texture_3d) &&
        !odyssey_texture_setup(s, &image, &texture, &t.texture)) {
        return SGI_GL_DRAW_OK;
    }
    if ((state.texture_2d || state.texture_3d) && s->tex_env[0] == 4) {
        /* The family words pack two normalized 12-bit components each. */
        state.texture_env_color[0] = ((s->tex_env[1] >> 12) & 0xfff) /
                                     4095.0f;
        state.texture_env_color[1] = (s->tex_env[1] & 0xfff) / 4095.0f;
        state.texture_env_color[2] = ((s->tex_env[2] >> 12) & 0xfff) /
                                     4095.0f;
        state.texture_env_color[3] = (s->tex_env[2] & 0xfff) / 4095.0f;
    }
    if (!odyssey_gl_index_mode(s)) {
        state.colour_mask = (s->gl_write_mask >> ODSY_WRITE_COLOUR_SHIFT) &
                            ODSY_WRITE_COLOUR_MASK;
    }
    state.fog_color[0] = ((s->gl_fog[0] >> 12) & 0xfff) / 4096.0f;
    state.fog_color[1] = (s->gl_fog[0] & 0xfff) / 4096.0f;
    state.fog_color[2] = ((s->gl_fog[1] >> 12) & 0xfff) / 4096.0f;
    state.fog_color[3] = (s->gl_fog[1] & 0xfff) / 4096.0f;
    state.fog_index = (s->gl_fog[0] >> 12) & 0xfff;
    draw.state = &state;
    result = sgi_soft_draw(&rules, &t, &draw);
    sgi_scanout_invalidate_all(&s->scanout);
    return result;
}

static void odyssey_gl_draw(void *opaque, const SGIGLDraw *d)
{
    odyssey_gl_draw_result(opaque, d);
}

/*
 * The checked path the shared context requires once per-face polygon modes
 * are activated (odyssey_reset_registers): it relays sgi_soft_draw()'s
 * out-of-memory signal from GL_POLYGON's dynamic vertex assembly back to
 * the shared context synchronously instead of dropping it.
 */
static SGIGLDrawResult odyssey_gl_draw_checked(void *opaque,
                                               const SGIGLDraw *d)
{
    return odyssey_gl_draw_result(opaque, d);
}

/* glClear's raster op: not on the first-triangle path, wired for later. */
static void odyssey_gl_raster(void *opaque, const SGIRasterOp *op)
{
    SGIOdysseyState *s = opaque;
    SGIRasterTarget t = odyssey_gl_target(s);
    SGIRasterRules rules = odyssey_gl_operation_rules(s);

    sgi_soft_raster(&rules, &t, op);
    sgi_scanout_invalidate_all(&s->scanout);
}

static SGIGLSink odyssey_gl_sink(SGIOdysseyState *s)
{
    return (SGIGLSink) {
        .draw = odyssey_gl_draw,
        .draw_checked = odyssey_gl_draw_checked,
        .raster = odyssey_gl_raster,
        .fence = NULL,
        .opaque = s,
    };
}

/*
 * Point the shared context at the current window: its origin from 0x841 and
 * the kept 0x204b rectangles as the pixels it owns, in plane coordinates.
 * Called before every command, so a change to either register takes effect
 * at the next one. With no rectangle kept, one inverted rectangle keeps the
 * shared rasterizer drawing nothing rather than logging an unbound window.
 * The shared context's front and back targets carry buffers 0 and 1, the
 * raster target callbacks' buffer numbers, and its draw buffers follow the
 * 0x0424 drawing mask. The kernel has already turned the client's front and
 * back into buffer numbers (ODSY_REG_DRAW_MASK), so the shared names are
 * slots here, not the client's roles: a client drawing its front after one
 * swap draws the context's "back", buffer 1.
 *
 * The shared rasterizer adds window rows to the origin as they come; the
 * client's combined matrix already counts them down from the window's top
 * row, the one 0x841 names (odyssey_gl_load_combined()), so the origin,
 * rectangles and id plane all stay in the registers' top-down plane
 * coordinates.
 */
static void odyssey_gl_bind(SGIOdysseyState *s)
{
    SGIGLDrawable d = {
        .bound = true,
        .origin = { s->win_origin[0], s->win_origin[1] },
        .front = 0,
        .back = 1,
    };
    unsigned buffers = odyssey_draw_buffers(s);
    uint32_t draw = (buffers & 1 ? SGI_GL_DRAW_FRONT : 0) |
                    (buffers & 2 ? SGI_GL_DRAW_BACK : 0);
    unsigned int i;

    for (i = 0; i < s->clip_kept; i++) {
        d.clip.r[i] = s->clip_rect[i];
    }
    d.clip.n = s->clip_kept;
    if (!d.clip.n) {
        /* An all-empty clip; SGIClipList.empty replaces this at rebase. */
        d.clip.r[0] = (SGIClipRect) { 1, 1, 0, 0 };
        d.clip.n = 1;
    }
    sgi_gl_set_drawable(&s->gl_ctx, &d);
    if (s->gl_ctx.cur.draw_buffers != draw) {
        SGIGLSink sink = odyssey_gl_sink(s);
        SGIGLCmd cmd = { .op = SGI_GL_OP_DRAW_BUFFERS, .nargs = 1,
                         .args = &draw };

        sgi_gl_command(&s->gl_ctx, &sink, &cmd);
    }
}

/* Decode one command into the shared context. */
static void odyssey_gl_feed(SGIOdysseyState *s, uint16_t op,
                            const uint32_t *args, uint16_t nargs)
{
    SGIGLSink sink = odyssey_gl_sink(s);
    SGIGLCmd cmd = { .op = op, .nargs = nargs, .args = args };

    odyssey_gl_bind(s);
    sgi_gl_command(&s->gl_ctx, &sink, &cmd);
}

/*
 * 0x404 bits 10:9 to the shared layer's cull-face enum: GL_BACK 01, GL_FRONT
 * 10, GL_FRONT_AND_BACK 11. Returns UINT32_MAX for 00, which no probe word
 * has sent; the shared layer's own glCullFace validation then rejects it,
 * leaving the previous mode in place.
 */
static uint32_t odyssey_cull_face_mode(uint32_t code)
{
    switch (code) {
    case 1:
        return SGI_GL_BACK;
    case 2:
        return SGI_GL_FRONT;
    case 3:
        return SGI_GL_FRONT_AND_BACK;
    default:
        return UINT32_MAX;
    }
}

/*
 * 0x404 bit 23 to the shared layer's glFrontFace sense, inverted -- see the
 * ODSY_MODE_FRONT_FACE_CW comment for why.
 */
static uint32_t odyssey_front_face(uint32_t pattern_mode)
{
    return pattern_mode & ODSY_MODE_FRONT_FACE_CW ? SGI_GL_CCW : SGI_GL_CW;
}

/*
 * Hand the GL state register 0x0404 carries to the shared context: bit 12,
 * the shade model, bit 16, the depth test, bit 14, the alpha test, bit 15,
 * the stencil test, bit 17, blending, bit 19, index logic op, bit 20,
 * colour logic op, bit 8, GL_CULL_FACE, bits 10:9, the face it culls, and
 * bit 23, front face (fed inverted; see the ODSY_MODE_FRONT_FACE_CW
 * comment), and bit 3, float GL line stipple. The shared rasterizer fills a
 * flat triangle from its provoking vertex, the last one for triangles and
 * strips as GL specifies.
 * Nothing in the stream says which vertex the board takes; the library
 * never reorders or repeats colours to steer it, so GL's own rule is
 * INFERRED. Called on every 0x0404, including the X server's pattern-mode
 * writes, which also update the separate integer 2D interpreter.
 *
 * This model has no stencil planes, so the shared rasterizer's stencil
 * pipeline stays permanently unset and the enable below never gates a
 * pixel: with no stencil buffer, OpenGL defines the stencil test to always
 * pass, which is exactly the behaviour a model with no stencil planes
 * already has. Feeding the enable keeps the decoded GL state faithful
 * without drawing a stencil test that could never do anything.
 */
static void odyssey_gl_apply_mode(SGIOdysseyState *s)
{
    uint32_t shade = s->pattern_mode & ODSY_MODE_FLAT_SHADE ?
                     SGI_GL_FLAT : SGI_GL_SMOOTH;
    uint32_t depth_test = SGI_GL_DEPTH_TEST;
    uint32_t alpha_test = SGI_GL_ALPHA_TEST;
    uint32_t stencil_test = SGI_GL_STENCIL_TEST;
    uint32_t blend = SGI_GL_BLEND;
    uint32_t index_logic_op = SGI_GL_INDEX_LOGIC_OP;
    uint32_t color_logic_op = SGI_GL_COLOR_LOGIC_OP;
    uint32_t offset_point = SGI_GL_POLYGON_OFFSET_POINT;
    uint32_t offset_line = SGI_GL_POLYGON_OFFSET_LINE;
    uint32_t offset_fill = SGI_GL_POLYGON_OFFSET_FILL;
    uint32_t cull_face = SGI_GL_CULL_FACE;
    uint32_t cull_mode = odyssey_cull_face_mode((s->pattern_mode >>
                                                 ODSY_MODE_CULL_MODE_SHIFT) &
                                                ODSY_MODE_CULL_MODE_MASK);
    uint32_t front_face = odyssey_front_face(s->pattern_mode);
    uint32_t line_stipple = SGI_GL_LINE_STIPPLE;
    uint32_t fog = SGI_GL_FOG;

    odyssey_gl_feed(s, SGI_GL_OP_SHADE_MODEL, &shade, 1);
    /*
     * The shared layer has no dither rule yet, so the bit is kept as family
     * state and reported at the first draw it would affect
     * (odyssey_gl_vertex_colour()). Once the shared rule lands this feeds
     * SGI_GL_OP_ENABLE/DISABLE of SGI_GL_DITHER instead. Derived from the
     * migrated 0x0404 word, so it survives migration through post_load.
     */
    s->gl_dither = s->pattern_mode & ODSY_MODE_DITHER;
    odyssey_gl_feed(s, s->pattern_mode & ODSY_MODE_DEPTH_TEST ?
                    SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, &depth_test, 1);
    odyssey_gl_feed(s, s->pattern_mode & ODSY_MODE_ALPHA_TEST ?
                    SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, &alpha_test, 1);
    odyssey_gl_feed(s, s->pattern_mode & ODSY_MODE_STENCIL_TEST ?
                    SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, &stencil_test, 1);
    odyssey_gl_feed(s, s->pattern_mode & ODSY_MODE_BLEND ?
                    SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, &blend, 1);
    odyssey_gl_feed(s, s->pattern_mode & ODSY_MODE_FOG ?
                    SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, &fog, 1);
    odyssey_gl_feed(s, s->pattern_mode & ODSY_MODE_INDEX_LOGIC_OP ?
                    SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, &index_logic_op, 1);
    odyssey_gl_feed(s, s->pattern_mode & ODSY_MODE_COLOR_LOGIC_OP ?
                    SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, &color_logic_op, 1);
    odyssey_gl_feed(s, s->pattern_mode & ODSY_MODE_OFFSET_POINT ?
                    SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, &offset_point, 1);
    odyssey_gl_feed(s, s->pattern_mode & ODSY_MODE_OFFSET_LINE ?
                    SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, &offset_line, 1);
    odyssey_gl_feed(s, s->pattern_mode & ODSY_MODE_OFFSET_FILL ?
                    SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, &offset_fill, 1);
    odyssey_gl_feed(s, s->pattern_mode & ODSY_MODE_CULL_FACE ?
                    SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, &cull_face, 1);
    odyssey_gl_feed(s, s->pattern_mode & ODSY_MODE_GL_LINE_STIPPLE ?
                    SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, &line_stipple, 1);
    odyssey_gl_feed(s, SGI_GL_OP_CULL_FACE, &cull_mode, 1);
    odyssey_gl_feed(s, SGI_GL_OP_FRONT_FACE, &front_face, 1);
}

static void odyssey_gl_apply_line_stipple(SGIOdysseyState *s)
{
    /* Word 1 remains the integer 2D path's 32-bit rotated pattern. */
    uint32_t args[] = {
        (s->line_stipple[2] & 0xff) + 1,
        s->line_stipple[0] & 0xffff,
    };

    odyssey_gl_feed(s, SGI_GL_OP_LINE_STIPPLE, args, ARRAY_SIZE(args));
}

/*
 * Push the family-shadowed clip box (ODSY_REG_CLIP_BOX) to the shared
 * context's scissor rectangle (see the macro's own comment for the
 * encoding and the always-on rationale). min > max -- including the
 * library's own 0x1000, 0x1000 empty encoding -- clips every fragment,
 * matched here with a zero-area box rather than a negative width or
 * height, which SGI_GL_OP_SCISSOR rejects.
 */
static void odyssey_gl_apply_clip_box(SGIOdysseyState *s)
{
    unsigned x_min = (s->gl_clip_box[0] >> ODSY_CLIP_BOX_MIN_SHIFT) &
                     ODSY_CLIP_BOX_FIELD_MASK;
    unsigned x_max = s->gl_clip_box[0] & ODSY_CLIP_BOX_FIELD_MASK;
    unsigned y_min = (s->gl_clip_box[1] >> ODSY_CLIP_BOX_MIN_SHIFT) &
                     ODSY_CLIP_BOX_FIELD_MASK;
    unsigned y_max = s->gl_clip_box[1] & ODSY_CLIP_BOX_FIELD_MASK;
    uint32_t box[4] = { x_min, y_min, 0, 0 };

    if (x_min <= x_max && y_min <= y_max) {
        box[2] = x_max - x_min + 1;
        box[3] = y_max - y_min + 1;
    }
    odyssey_gl_feed(s, SGI_GL_OP_SCISSOR, box, 4);
}

/*
 * Recompute which of the shared context's six clip-plane slots are tested,
 * from 0x0405 bits 2:0's thermometer count n (see ODSY_TEX_MODE_CLIP_COUNT_
 * MASK's comment): slots 0..n-1 enabled, the rest disabled. Called on every
 * 0x0405 write and after migration; does not touch the loaded coefficients
 * themselves, which a separate load (ODSY_GL_CLIP_PLANE_LOAD) feeds.
 */
static void odyssey_gl_apply_clip_planes(SGIOdysseyState *s)
{
    unsigned bits = s->tex_mode & ODSY_TEX_MODE_CLIP_COUNT_MASK;
    unsigned n = ctpop32(bits);

    for (unsigned i = 0; i < SGI_GL_CLIP_PLANES; i++) {
        uint32_t plane = SGI_GL_CLIP_PLANE0 + i;

        odyssey_gl_feed(s, i < n ? SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE,
                        &plane, 1);
    }
}

/*
 * 0x0c21's 6-bit blend factor code to the shared layer's enum
 * (docs/reviews/odyssey-fragment-state-evidence-20261001.md, established):
 * 0 GL_ZERO, 0x08 GL_ONE, 0x10 "the other colour" (GL_DST_COLOR as a source
 * factor, GL_SRC_COLOR as a destination factor -- the board never needs both
 * names at the same position, so one code covers both), 0x18 GL_SRC_ALPHA,
 * 0x20 GL_DST_ALPHA, 0x28 GL_CONSTANT_COLOR, 0x30 GL_CONSTANT_ALPHA, 0x38 the
 * "one minus" of the 0x10 pairing, 0x39 through 0x3c the "one minus" of
 * 0x18/0x20/0x28/0x30 in turn, and 0x40, a source-only code, GL_SRC_ALPHA_
 * SATURATE. Returns UINT32_MAX for a code with no factor at the requested
 * position.
 */
static uint32_t odyssey_blend_factor(uint32_t code, bool dst)
{
    switch (code) {
    case 0x00:
        return 0;                       /* GL_ZERO */
    case 0x08:
        return 1;                       /* GL_ONE */
    case 0x10:
        return dst ? SGI_GL_SRC_COLOR : SGI_GL_DST_COLOR;
    case 0x18:
        return SGI_GL_SRC_ALPHA;
    case 0x20:
        return SGI_GL_DST_ALPHA;
    case 0x28:
        return SGI_GL_CONSTANT_COLOR;
    case 0x30:
        return SGI_GL_CONSTANT_ALPHA;
    case 0x38:
        return dst ? SGI_GL_ONE_MINUS_SRC_COLOR : SGI_GL_ONE_MINUS_DST_COLOR;
    case 0x39:
        return SGI_GL_ONE_MINUS_SRC_ALPHA;
    case 0x3a:
        return SGI_GL_ONE_MINUS_DST_ALPHA;
    case 0x3b:
        return SGI_GL_ONE_MINUS_CONSTANT_COLOR;
    case 0x3c:
        return SGI_GL_ONE_MINUS_CONSTANT_ALPHA;
    case 0x40:
        return dst ? UINT32_MAX : SGI_GL_SRC_ALPHA_SATURATE;
    default:
        return UINT32_MAX;
    }
}

/*
 * 0x041f's three 3-bit stencil-op fields (fail, depth-fail, depth-pass) to
 * the shared layer's enum: KEEP 0, ZERO 1, REPLACE 2, INCR 3, DECR 4,
 * INVERT 5. Returns UINT32_MAX for the two codes the board leaves unused.
 */
static uint32_t odyssey_stencil_op(uint32_t code)
{
    switch (code) {
    case 0:
        return SGI_GL_KEEP;
    case 1:
        return 0;                       /* GL_ZERO */
    case 2:
        return SGI_GL_REPLACE;
    case 3:
        return SGI_GL_INCR;
    case 4:
        return SGI_GL_DECR;
    case 5:
        return SGI_GL_INVERT;
    default:
        return UINT32_MAX;
    }
}

/*
 * 0x0415's 2-bit face code to the shared layer's polygon-mode enum, GL_POINT
 * 0 to GL_FILL 2. Returns UINT32_MAX for the one code the board leaves
 * undefined, which the shared layer's own glPolygonMode validation then
 * rejects, leaving the previous mode in place.
 */
static uint32_t odyssey_polygon_mode(uint32_t code)
{
    switch (code) {
    case 0:
        return SGI_GL_POINT;
    case 1:
        return SGI_GL_LINE;
    case 2:
        return SGI_GL_FILL;
    default:
        return UINT32_MAX;
    }
}

/*
 * 0x0c21 word 0 bits 15:13 to the shared layer's blend equation. Codes 3
 * and 4, the SGIX alpha min/max extension, have no shared equivalent
 * (sgi_gl_blend_equation_valid() accepts only the other six), so the
 * dispatch case below reports and skips them rather than feeding a code
 * the shared layer would reject.
 */
static const uint32_t odyssey_blend_equation[8] = {
    SGI_GL_FUNC_ADD, SGI_GL_MIN, SGI_GL_MAX, 0, 0,
    SGI_GL_LOGIC_OP, SGI_GL_FUNC_SUBTRACT, SGI_GL_FUNC_REVERSE_SUBTRACT,
};

/* 0x0420 and 0x0452 to the shared context's comparison and depth mask. */
static void odyssey_gl_apply_depth(SGIOdysseyState *s)
{
    uint32_t func = SGI_GL_NEVER_ENUM +
                    (s->gl_depth_func & ODSY_DEPTH_FUNC_MASK);
    uint32_t mask = !!(s->gl_write_mask & ODSY_WRITE_DEPTH);

    odyssey_gl_feed(s, SGI_GL_OP_DEPTH_FUNC, &func, 1);
    odyssey_gl_feed(s, SGI_GL_OP_DEPTH_MASK, &mask, 1);
}

/* The lighting words matched by value (ODSY_GL_NORMAL_3F and the rest). */
static const uint32_t odyssey_gl_light_words[] = {
    ODSY_GL_NORMAL_3F,
    ODSY_GL_MATERIAL_AMBIENT, ODSY_GL_MATERIAL_DIFFUSE,
    ODSY_GL_MATERIAL_SPECULAR, ODSY_GL_MATERIAL_EMISSION,
    ODSY_GL_BACK_AMBIENT, ODSY_GL_BACK_DIFFUSE,
    ODSY_GL_BACK_SPECULAR, ODSY_GL_BACK_EMISSION,
    ODSY_GL_SHININESS_SQRT, ODSY_GL_BACK_SHININESS_SQRT,
    ODSY_GL_SHININESS_DERIVED, ODSY_GL_LIGHT_COLOUR_LOAD,
    ODSY_GL_LIGHT_VEC_LOAD, ODSY_GL_CLIP_PLANE_LOAD, ODSY_PROG_RUN_ONCE,
};

/*
 * The texture-coordinate and generation fragments
 * (docs/analysis/odyssey-gl-geometry-program.md, section 6 item 2).
 */
static const uint32_t odyssey_ucode_texgen[] = {
    0x033, 0x04d, 0x052, 0x057, 0x05c, 0x061, 0x066, 0x06b, 0x06c, 0x077,
    0x07b, 0x07f, 0x091, 0x09f,
};

static uint32_t odyssey_prog_value(uint32_t word)
{
    return (word & ODSY_PROG_LOW_MASK) |
           (((word >> ODSY_PROG_HIGH_SHIFT) & 1) << 10);
}

static bool odyssey_is_program_word(uint32_t word)
{
    uint32_t cls = word >> ODSY_PROG_CLASS_SHIFT;

    if ((word & ODSY_PROG_SHAPE) != ODSY_PROG_TAG) {
        return false;
    }
    /*
     * A program-memory halfword is 16 bits, so its value reaches bits 28:23
     * and its class byte runs 0x40 to 0x5f (0x43808320 is halfword 0x1f20).
     */
    if (cls >= ODSY_PROG_PROGRAM_HALFWORD &&
        cls <= ODSY_PROG_PROGRAM_HALFWORD_LAST) {
        return true;
    }
    switch (cls) {
    case ODSY_PROG_COLOUR_MODE:
    case ODSY_PROG_COLOUR_MODE_B:
    case ODSY_PROG_SET_UCODE:
    case ODSY_PROG_PROGRAM_ADDR:
    case ODSY_PROG_CONST_SELECT:
    case ODSY_PROG_ENTRY_29:
    case ODSY_PROG_ENTRY_2A:
    case ODSY_PROG_ENTRY_2C:
    case ODSY_PROG_LIGHT_VEC_SELECT:
    case ODSY_PROG_LIGHT_COLOUR_SELECT:
    case ODSY_PROG_LIST_INDEX:
    case ODSY_PROG_CONST_STORE:
        return true;
    default:
        return false;
    }
}

static bool odyssey_ucode_is_texgen(uint32_t a)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(odyssey_ucode_texgen); i++) {
        if (a == odyssey_ucode_texgen[i]) {
            return true;
        }
    }
    return false;
}

/* What a program asks for, as far as the model recognises it. */
typedef struct OdysseyProgram {
    bool known, lit, local_viewer, two_sided, texgen, clip, texture;
} OdysseyProgram;

/*
 * Walk the list the engine continues through, constants[k] up to the
 * terminator. With strict, every word must be a routine this model names
 * (an assembled lit list); otherwise only the texgen and clip-plane stages
 * are picked out (a precompiled program's list, which its entries already
 * identify, so the result is not needed). False when the terminator is not
 * reached or, when strict, a word is not recognised.
 */
static bool odyssey_gl_walk_list(SGIOdysseyState *s, uint32_t k, bool strict,
                                 OdysseyProgram *p)
{
    for (unsigned int n = 0; n < ODSY_PROG_LIST_MAX; n++) {
        uint32_t a = s->gl_const[(k + n) % SGI_ODYSSEY_GL_CONSTANTS];
        uint32_t next = s->gl_const[(k + n + 1) % SGI_ODYSSEY_GL_CONSTANTS];

        if (a == ODSY_UCODE_END) {
            return true;
        }
        if (a == 0 && (next == ODSY_UCODE_CLIP ||
                       next == ODSY_UCODE_CLIP_TEXTURED ||
                       next == ODSY_UCODE_CLIP_TEXGEN)) {
            p->clip = true;
            n++;
            continue;
        }
        if (odyssey_ucode_is_texgen(a)) {
            p->texgen = true;
            continue;
        }
        switch (a) {
        case ODSY_UCODE_FINISH_A_TWO_SIDED:
        case ODSY_UCODE_FINISH_TWO_SIDED:
        case ODSY_UCODE_END_TWO_SIDED:
            p->two_sided = true;
            break;
        case ODSY_UCODE_FINISH_A_TEXTURE:
        case ODSY_UCODE_FINISH_TEXTURE:
        case ODSY_UCODE_UNLIT_TEXTURE:
            p->texture = true;
            break;
        case ODSY_UCODE_NORMAL:
        case ODSY_UCODE_NORMALIZE:
        case ODSY_UCODE_LIGHT_DIR:
        case ODSY_UCODE_LIGHT_POS:
        case ODSY_UCODE_LIGHT_PLAIN:
        case ODSY_UCODE_LIGHT_ATTEN:
        case ODSY_UCODE_FINISH_A:
        case ODSY_UCODE_FINISH_ONE_SIDED:
        case ODSY_UCODE_END_ONE_SIDED:
        case ODSY_UCODE_TAIL_ONE_SHOT:
        case ODSY_UCODE_TAIL:
        case ODSY_UCODE_UNLIT_FINISH:
            break;
        default:
            if (strict) {
                return false;
            }
            break;
        }
    }
    return false;
}

/*
 * One program-memory halfword (ODSY_PROG_SHAPE): halfwords 5 down to 0 of
 * the selected entry, then the next entry, kept in the image's order.
 */
static void odyssey_gl_program_halfword(SGIOdysseyState *s, uint32_t word)
{
    uint16_t h = (word & ODSY_PROG_LOW_MASK) |
                 ((word >> ODSY_PROG_HIGH_SHIFT) &
                  ODSY_PROG_HALFWORD_HIGH_MASK) << 10;

    if (s->gl_program_addr >= SGI_ODYSSEY_GL_PROGRAM_ENTRIES) {
        if (!s->gl_program_mem_reported) {
            s->gl_program_mem_reported = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: program-memory store past "
                          "entry 0x%x is not modeled\n",
                          SGI_ODYSSEY_GL_PROGRAM_ENTRIES - 1);
        }
        return;
    }
    s->gl_program_mem[s->gl_program_addr * ODSY_PROG_ENTRY_HALFWORDS +
                      ODSY_PROG_ENTRY_HALFWORDS - 1 - s->gl_program_half] = h;
    if (++s->gl_program_half == ODSY_PROG_ENTRY_HALFWORDS) {
        s->gl_program_half = 0;
        s->gl_program_addr++;
    }
}

/*
 * Image entries a relocated program can start from, known by the FNV-1a
 * digest of their twelve bytes as the geometry-engine microcode image
 * IRIX 6.5.30 ships holds them (its identity is recorded in the
 * datasheet), big-endian halfword 0 first. The model carries no
 * microcode, only these digests, computed from the shipped image; each
 * body occurs once in it. Only the program the probe selects is listed.
 */
static const struct {
    uint64_t digest;
    uint32_t entry;
} odyssey_ucode_bodies[] = {
    { 0x3e97b9b9e8252570ull, ODSY_UCODE_LIT_POS2_TEX },
};

static bool odyssey_prog_in_window(uint32_t a)
{
    return a >= ODSY_PROG_WINDOW_BASE &&
           a < ODSY_PROG_WINDOW_BASE + ODSY_PROG_WINDOW_ENTRIES;
}

/*
 * Move a program's entries e[0..2] back to the image addresses the library
 * relocated them from. An entry below the window is resident and stays. For
 * the first entry in the window, the body uploaded there is looked up in
 * the image by content (odyssey_ucode_bodies); the library moves every
 * relocated address by the same distance, so the others follow
 * (docs/analysis/odyssey-gl-geometry-program.md, "Program memory"). False
 * when that body is not one the model knows. List words in the window are
 * not moved: a precompiled program is recognised from its entries alone.
 */
static bool odyssey_gl_resident(SGIOdysseyState *s, uint32_t e[3])
{
    unsigned int i, j, k;

    for (i = 0; i < 3; i++) {
        const uint16_t *body;
        uint64_t digest = 0xcbf29ce484222325ull;

        if (!odyssey_prog_in_window(e[i])) {
            continue;
        }
        body = &s->gl_program_mem[e[i] * ODSY_PROG_ENTRY_HALFWORDS];
        for (k = 0; k < ODSY_PROG_ENTRY_HALFWORDS; k++) {
            digest = (digest ^ (body[k] >> 8)) * 0x100000001b3ull;
            digest = (digest ^ (body[k] & 0xff)) * 0x100000001b3ull;
        }
        for (k = 0; k < ARRAY_SIZE(odyssey_ucode_bodies); k++) {
            if (odyssey_ucode_bodies[k].digest == digest) {
                break;
            }
        }
        if (k == ARRAY_SIZE(odyssey_ucode_bodies)) {
            return false;
        }
        for (j = 3; j-- > i;) {
            if (odyssey_prog_in_window(e[j])) {
                e[j] += odyssey_ucode_bodies[k].entry - e[i];
            }
        }
        return true;
    }
    return true;
}

/*
 * Recognise the program the library just set (on every 0x2c write, the last
 * word of each program set, and after migration). APPROXIMATION, see
 * ODSY_UCODE_END: a precompiled program is recognised by its three entries,
 * an assembled one (0x2a and 0x2c both the terminator) by the routines in
 * its list, the head (0x29) giving the viewer and saying lighting is on, or,
 * for the unlit head 0x4ae, that it is off.
 * Which lights are enabled and of what kind is not taken from the program:
 * the library zeroes a disabled light's colours, so lighting all eight
 * blocks gives the same colour, and how the engine maps a sparse set of
 * enabled lights onto the list is unknown. All-zero entries are the state
 * the GL library's context default-state setup pushes before any program
 * exists.
 */
static void odyssey_gl_program(SGIOdysseyState *s)
{
    uint32_t r[3] = {
        s->gl_entry[ODSY_ENTRY_29], s->gl_entry[ODSY_ENTRY_2A],
        s->gl_entry[ODSY_ENTRY_2C],
    };
    uint32_t e29 = r[0], e2a = r[1], e2c = r[2];
    uint32_t k = s->gl_entry[ODSY_ENTRY_33];
    OdysseyProgram p = { 0 };
    bool resident = odyssey_gl_resident(s, r);
    unsigned int i;

    if (!resident) {
        /* A relocated body the model does not know: reported below. */
    } else if (!r[0] && !r[1] && !r[2]) {
        p.known = true;
    } else if (r[0] == ODSY_UCODE_UNLIT && r[1] == ODSY_UCODE_UNLIT_2A &&
               r[2] == ODSY_UCODE_UNLIT_2C) {
        odyssey_gl_walk_list(s, k, false, &p);
        p.known = true;
    } else if (r[0] == ODSY_UCODE_UNLIT_CLIP &&
               r[1] == ODSY_UCODE_UNLIT_CLIP_2A &&
               r[2] == ODSY_UCODE_UNLIT_CLIP_2C) {
        odyssey_gl_walk_list(s, k, false, &p);
        p.known = true;
    } else if (r[0] == ODSY_UCODE_LIT_DIR1 && r[1] == ODSY_UCODE_LIT_DIR1_2A &&
               r[2] == ODSY_UCODE_LIT_DIR1_2C) {
        odyssey_gl_walk_list(s, k, false, &p);
        p.known = true;
        p.lit = true;
    } else if (r[0] == ODSY_UCODE_LIT_POS2_TEX &&
               r[1] == ODSY_UCODE_LIT_POS2_TEX_2A &&
               r[2] == ODSY_UCODE_LIT_POS2_TEX_2C) {
        /*
         * The lights, their kinds and GL_NORMALIZE are taken from the light
         * blocks as for every lit program (odyssey_gl_light()); the texture
         * the record also asks for is reported below.
         */
        odyssey_gl_walk_list(s, k, false, &p);
        p.known = true;
        p.lit = true;
        p.texture = true;
    } else if (r[1] == ODSY_UCODE_END && r[2] == ODSY_UCODE_END) {
        switch (r[0]) {
        case ODSY_UCODE_HEAD_LOCAL_DIR:
        case ODSY_UCODE_HEAD_LOCAL_POS:
            p.local_viewer = true;
            /* fall through */
        case ODSY_UCODE_HEAD_INF_DIR:
        case ODSY_UCODE_HEAD_INF_POS:
            p.known = odyssey_gl_walk_list(s, k, true, &p);
            p.lit = true;
            break;
        case ODSY_UCODE_HEAD_UNLIT:
            /* Lighting off: unlit is the correct rendering, not a guess. */
            p.known = odyssey_gl_walk_list(s, k, true, &p);
            break;
        default:
            break;
        }
    }

    if (!p.known) {
        p.lit = false;
        for (i = 0; i < s->gl_program_logged_count; i++) {
            if (s->gl_program_logged[i * 3] == e29 &&
                s->gl_program_logged[i * 3 + 1] == e2a &&
                s->gl_program_logged[i * 3 + 2] == e2c) {
                break;
            }
        }
        if (i == s->gl_program_logged_count &&
            i < SGI_ODYSSEY_GL_PROGRAM_REPORTS) {
            const uint32_t *c = s->gl_const;
            uint32_t m = SGI_ODYSSEY_GL_CONSTANTS - 1;

            s->gl_program_logged[i * 3] = e29;
            s->gl_program_logged[i * 3 + 1] = e2a;
            s->gl_program_logged[i * 3 + 2] = e2c;
            s->gl_program_logged_count++;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: geometry program 0x%x "
                          "0x%x 0x%x from list index %u (0x%x 0x%x 0x%x 0x%x "
                          "0x%x 0x%x 0x%x 0x%x) is not recognised%s; drawn "
                          "unlit\n", e29, e2a, e2c, k, c[k & m],
                          c[(k + 1) & m], c[(k + 2) & m], c[(k + 3) & m],
                          c[(k + 4) & m], c[(k + 5) & m], c[(k + 6) & m],
                          c[(k + 7) & m], resident ? "" :
                          " (its program-window body is not a known image "
                          "entry)");
        }
    }
    if (p.two_sided && !s->gl_two_sided_logged) {
        s->gl_two_sided_logged = true;
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: two-sided lighting is not "
                      "modeled; back faces are lit with the front material\n");
    }
    if (p.texgen && !s->gl_texgen_logged) {
        s->gl_texgen_logged = true;
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: texture-coordinate generation "
                      "in geometry program 0x%x is not modeled\n", e29);
    }
    if (p.clip && !s->gl_clip_logged) {
        s->gl_clip_logged = true;
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: clip planes in geometry "
                      "program 0x%x are not modeled; geometry is not clipped "
                      "to them\n", e29);
    }
    if (p.texture && !s->gl_texture_stage_logged) {
        s->gl_texture_stage_logged = true;
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: texture-mode lighting finish in "
                      "geometry program 0x%x is not modeled\n", e29);
    }
    s->gl_lit = p.lit;
    s->gl_local_viewer = p.local_viewer;
}

/*
 * A colour-mode word (ODSY_PROG_COLOUR_MODE). Only the captured pairs are
 * known: 0x22 = 0 or 1, 0x23 = 2 or 3 (0 from the kernel before a context
 * has drawn). Anything else, or a pair whose index bits disagree once
 * 0x23, which follows 0x22 in every capture, has arrived, is kept and
 * reported once.
 */
static void odyssey_colour_mode(SGIOdysseyState *s, uint32_t cls, uint32_t v)
{
    uint32_t known = cls == ODSY_PROG_COLOUR_MODE ? 0x1 : 0x3;
    bool disagree;

    s->colour_mode[cls - ODSY_PROG_COLOUR_MODE] = v;
    disagree = cls == ODSY_PROG_COLOUR_MODE_B &&
               ((s->colour_mode[0] ^ s->colour_mode[1]) &
                ODSY_COLOUR_MODE_INDEX);
    if (((v & ~known) || disagree) && !s->colour_mode_reported) {
        s->colour_mode_reported = true;
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: colour mode word class 0x%x "
                      "value 0x%x (0x22 value 0x%x) is not modeled\n", cls,
                      v, s->colour_mode[0]);
    }
}

/* One program word (odyssey_is_program_word()). */
static void odyssey_gl_program_word(SGIOdysseyState *s, uint32_t word)
{
    uint32_t v = odyssey_prog_value(word);

    switch (word >> ODSY_PROG_CLASS_SHIFT) {
    case ODSY_PROG_COLOUR_MODE:
    case ODSY_PROG_COLOUR_MODE_B:
        odyssey_colour_mode(s, word >> ODSY_PROG_CLASS_SHIFT, v);
        break;
    case ODSY_PROG_CONST_SELECT:
        s->gl_const_index = v;
        break;
    case ODSY_PROG_CONST_STORE:
        s->gl_const[s->gl_const_index % SGI_ODYSSEY_GL_CONSTANTS] = v;
        s->gl_const_index = (s->gl_const_index + 1) % SGI_ODYSSEY_GL_CONSTANTS;
        break;
    case ODSY_PROG_ENTRY_29:
        /*
         * Alone, 0x29 selects the stream for light loads (0x19a) or pixel
         * transfers (0x12f) and runs the one-shot: not a program change. It
         * is kept for the next 0x2c.
         */
        s->gl_entry[ODSY_ENTRY_29] = v;
        break;
    case ODSY_PROG_ENTRY_2A:
        s->gl_entry[ODSY_ENTRY_2A] = v;
        break;
    case ODSY_PROG_ENTRY_2C:
        s->gl_entry[ODSY_ENTRY_2C] = v;
        odyssey_gl_program(s);
        break;
    case ODSY_PROG_LIST_INDEX:
        s->gl_entry[ODSY_ENTRY_33] = v;
        break;
    case ODSY_PROG_LIGHT_VEC_SELECT:
        s->gl_light_vec_sel = v;
        break;
    case ODSY_PROG_LIGHT_COLOUR_SELECT:
        s->gl_light_colour_sel = v;
        break;
    case ODSY_PROG_PROGRAM_ADDR:
        s->gl_program_addr = v;
        s->gl_program_half = 0;
        break;
    default:
        if ((word >> ODSY_PROG_CLASS_SHIFT) >= ODSY_PROG_PROGRAM_HALFWORD) {
            odyssey_gl_program_halfword(s, word);
        }
        /* Class 0x25: accepted, see ODSY_PROG_SHAPE. */
        break;
    }
}

static double odyssey_f32(uint32_t bits)
{
    float f;

    memcpy(&f, &bits, sizeof(f));
    return f;
}

static uint32_t odyssey_f32_bits(float f)
{
    uint32_t bits;

    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static void odyssey_normalize3(double v[3])
{
    double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

    if (n > 0) {
        v[0] /= n;
        v[1] /= n;
        v[2] /= n;
    }
}

/*
 * OpenGL 1.2.1's lighting equation (section 2.13.1, equation 2.5) at one
 * vertex, front material, from the kept light, material and matrix words.
 * APPROXIMATION: the board's geometry engine computes this in microcode the
 * model does not run; the equation is the one that reproduces both lighting
 * captures offline (docs/analysis/odyssey-gl-shading-depth-lighting.md,
 * section 3). The eye position is the model-view (slot 0x24) applied to
 * the vertex, the normal the upper 3x3 of slot 0x34 applied to the current
 * normal and normalised. A light block is directional when its precomputed
 * half vector (floats 3-5) is not zero, positional otherwise (INFERRED from
 * the values; a directional light straight down -z would have a zero half
 * vector and is then read as positional, which is unknown). A light with
 * zero ambient, diffuse and specular colour -- every disabled one, as the
 * library sends it -- adds nothing and is skipped.
 *
 * Two deviations from the GL text, neither exercised by a capture. The
 * transformed normal is always normalised, so programs with the plain
 * normal transform (0x2b6) and with GL_NORMALIZE (0x29c) draw alike, where
 * GL without GL_NORMALIZE under a scaling model-view does not renormalise
 * (INFERRED to be harmless: the captures' normal matrix is the identity).
 * A zero shininess gives a specular factor of 1 at any n.h, by C's
 * pow(0, 0) = 1, a case the GL text leaves open.
 */
static void odyssey_gl_light(SGIOdysseyState *s, const double obj[4],
                             float out[4])
{
    double eye[3], n[3], c[3], ma[4], md[4], ms[4], me[4], ga[4], shin;
    unsigned int i, k, r;

    for (r = 0; r < 3; r++) {
        eye[r] = 0;
        for (k = 0; k < 4; k++) {
            eye[r] += odyssey_f32(s->gl_modelview[r * 4 + k]) * obj[k];
        }
        n[r] = 0;
        for (k = 0; k < 3; k++) {
            n[r] += odyssey_f32(s->gl_normal_matrix[r * 4 + k]) *
                    odyssey_f32(s->gl_normal[k]);
        }
    }
    odyssey_normalize3(n);
    for (k = 0; k < 4; k++) {
        ma[k] = odyssey_f32(s->gl_material[k]);
        md[k] = odyssey_f32(s->gl_material[4 + k]);
        ms[k] = odyssey_f32(s->gl_material[8 + k]);
        me[k] = odyssey_f32(s->gl_material[12 + k]);
        ga[k] = odyssey_f32(s->gl_light_colour[ODSY_GL_LIGHT_MODEL_AMBIENT * 4 +
                                               k]);
    }
    shin = odyssey_f32(s->gl_shininess_sqrt);
    shin *= shin;
    for (k = 0; k < 3; k++) {
        c[k] = me[k] + ga[k] * ma[k];
    }
    for (i = 0; i < SGI_ODYSSEY_GL_LIGHTS; i++) {
        const uint32_t *blk = &s->gl_light_vec[i * ODSY_GL_LIGHT_VEC_FLOATS];
        const uint32_t *col = &s->gl_light_colour[i * 3 * 4];
        double la[3], ld[3], ls[3], L[3], h[3], atten = 1, spot = 1;
        double nl, nh, fs;
        bool any = false;

        for (k = 0; k < 3; k++) {
            la[k] = odyssey_f32(col[k]);
            ld[k] = odyssey_f32(col[4 + k]);
            ls[k] = odyssey_f32(col[8 + k]);
            any |= la[k] != 0 || ld[k] != 0 || ls[k] != 0;
        }
        if (!any) {
            continue;
        }
        if (odyssey_f32(blk[ODSY_LIGHT_HALF]) != 0 ||
            odyssey_f32(blk[ODSY_LIGHT_HALF + 1]) != 0 ||
            odyssey_f32(blk[ODSY_LIGHT_HALF + 2]) != 0) {
            for (k = 0; k < 3; k++) {
                L[k] = odyssey_f32(blk[ODSY_LIGHT_POS + k]);
            }
            odyssey_normalize3(L);
        } else {
            double d, denom, cut, cs;

            for (k = 0; k < 3; k++) {
                L[k] = odyssey_f32(blk[ODSY_LIGHT_POS + k]) - eye[k];
            }
            d = sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            odyssey_normalize3(L);
            denom = odyssey_f32(blk[ODSY_LIGHT_ATTEN]) +
                    odyssey_f32(blk[ODSY_LIGHT_ATTEN + 1]) * d +
                    odyssey_f32(blk[ODSY_LIGHT_ATTEN + 2]) * d * d;
            atten = denom > 0 ? 1 / denom : 1;
            cut = -odyssey_f32(blk[ODSY_LIGHT_SPOT_COS]);
            if (cut > -1) {
                /* The spot direction is minus floats 10-12. */
                cs = 0;
                for (k = 0; k < 3; k++) {
                    cs += L[k] * odyssey_f32(blk[ODSY_LIGHT_SPOT_DIR + k]);
                }
                spot = cs >= cut ?
                       pow(cs, -odyssey_f32(blk[ODSY_LIGHT_SPOT_EXP])) : 0;
            }
        }
        nl = MAX(n[0] * L[0] + n[1] * L[1] + n[2] * L[2], 0.0);
        for (k = 0; k < 3; k++) {
            h[k] = L[k];
        }
        if (s->gl_local_viewer) {
            double v[3] = { -eye[0], -eye[1], -eye[2] };

            odyssey_normalize3(v);
            for (k = 0; k < 3; k++) {
                h[k] += v[k];
            }
        } else {
            h[2] += 1;
        }
        odyssey_normalize3(h);
        nh = MAX(n[0] * h[0] + n[1] * h[1] + n[2] * h[2], 0.0);
        fs = nl > 0 ? pow(nh, shin) : 0;
        for (k = 0; k < 3; k++) {
            c[k] += atten * spot * (la[k] * ma[k] + nl * ld[k] * md[k] +
                                    fs * ls[k] * ms[k]);
        }
    }
    for (k = 0; k < 3; k++) {
        out[k] = MIN(MAX(c[k], 0.0), 1.0);
    }
    out[3] = md[3];
}

/*
 * The colour a GL vertex takes, handed to the shared context just before
 * the vertex: in index mode the current index (glIndex, not glColor); under
 * a lit program the lit colour; otherwise the current glColor, restored if
 * an index or lit vertex replaced it.
 */
static void odyssey_gl_vertex_colour(SGIOdysseyState *s, bool has_z)
{
    uint32_t words[4];

    if (s->gl_dither && !s->gl_dither_reported) {
        s->gl_dither_reported = true;
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: dither not modelled; drawing "
                      "undithered\n");
    }
    if (odyssey_gl_index_mode(s)) {
        float rgba[4];

        if (s->gl_lit && !s->gl_index_lit_reported) {
            s->gl_index_lit_reported = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: colour-index lighting is "
                          "not modeled; drawn with the current index\n");
        }
        odyssey_gl_index_rgba(s->gl_color_index, rgba);
        memcpy(words, rgba, sizeof(words));
        odyssey_gl_feed(s, SGI_GL_OP_COLOR4F, words, 4);
        s->gl_colour_overridden = true;
    } else if (s->gl_lit) {
        double obj[4] = {
            odyssey_f32(s->cmd_operand[0]), odyssey_f32(s->cmd_operand[1]),
            has_z ? odyssey_f32(s->cmd_operand[2]) : 0, 1,
        };
        float rgba[4];

        odyssey_gl_light(s, obj, rgba);
        memcpy(words, rgba, sizeof(words));
        odyssey_gl_feed(s, SGI_GL_OP_COLOR4F, words, 4);
        s->gl_colour_overridden = true;
    } else if (s->gl_colour_overridden) {
        odyssey_gl_feed(s, SGI_GL_OP_COLOR4F, s->gl_colour, 4);
        s->gl_colour_overridden = false;
    }
}

/*
 * The combined matrix (slot 0x00): the client's model-view and projection
 * composed with its window map, so it takes object coordinates straight to
 * window pixels with row 0 at the window's top. That it carries the
 * model-view is ESTABLISHED from glfalls' capture (docs/analysis/odyssey-gl-
 * demos.md): its fourth row is minus the third row of the model-view it
 * loads into slot 0x24, as a frustum times that model-view gives, and its
 * vertices land in its window through the combined matrix alone and nowhere
 * in it when the model-view is applied first. So the model-view (slot 0x24)
 * is kept only for lighting's eye position (odyssey_gl_light()), and the
 * shared context's own model-view stays the identity. For a 128x128 window
 * under an identity model-view and projection it
 * is x' = 64x + 64, y' = -64y + 63.996, and glOrtho(-2, 2, -2, 2, -1, 1)
 * halves the x and y scales (docs/reviews/odyssey-datasheet-fold-
 * 8eaf559842.md, claim 12). The two cannot be pulled apart, and nothing else
 * says where glViewport put the viewport, so the model applies the product
 * whole, as the board must.
 *
 * The shared context's pipeline is projection, divide, then viewport map. It
 * is given a fixed viewport of +-ODSY_GL_WINDOW_REACH about the window
 * origin, xw = R x_ndc and yw = R y_ndc, and as its projection the combined
 * matrix with the x and y rows divided by R, so its output is the combined
 * matrix's window position unchanged, the library's 1/256 row bias included.
 * The rows run top-down like the window origin, rectangles and id plane, so
 * no flip is needed. Clipping is then the window's own rectangles; where the
 * board clips geometry that leaves the window is unknown.
 *
 * Depth: the retained block-16 fourth float scales the combined depth
 * row. In native orthographic 1/5, 1/10 and 1/20 controls, dividing the
 * signed row by that float gives the GL window depth. This is an
 * experimental model policy, not a measured board contract. Malformed zero
 * or non-finite words retain the previous sign-only fallback. The shared
 * pipeline's window depth is (z_c / w_c + 1) / 2, so its z row is
 * row2 / (block16[3] * S) - row3, with S = ODSY_GL_DEPTH_S.
 * A depth outside [0, 1] is then dropped by the shared rasterizer, which is
 * the near and far clip. The other three block-16 floats have no observed
 * role. A non-default glDepthRange changes the row in
 * a way this rule does not fit (docs/analysis/odyssey-gl-shading-depth-
 * lighting.md, "Depth range"): it is drawn with the default [0, 1] range,
 * and the near and far clip planes the library adds for it are reported as
 * a program stage (odyssey_gl_program()).
 *
 * A matrix whose x or y row is zero -- the library's load before its window
 * exists, or the kernel's zero restore at a context switch -- leaves an empty
 * viewport, so nothing is drawn.
 */
static void odyssey_gl_load_combined(SGIOdysseyState *s, const float m[16])
{
    int32_t *vp = s->gl_ctx.cur.viewport;
    float p[16], sign_word;
    double depth_scale;
    unsigned int col;

    memset(s->gl_ctx.cur.viewport, 0, sizeof(s->gl_ctx.cur.viewport));
    if ((m[0] == 0 && m[4] == 0 && m[8] == 0) ||
        (m[1] == 0 && m[5] == 0 && m[9] == 0)) {
        return;
    }
    memcpy(&sign_word, &s->gl_block16[ODSY_GL_BLOCK16_SIGN],
           sizeof(sign_word));
    if (isfinite(sign_word) && sign_word != 0) {
        depth_scale = sign_word;
    } else {
        depth_scale = sign_word < 0 ? -1.0 : 1.0;
    }
    for (col = 0; col < 4; col++) {
        p[col * 4 + 0] = m[col * 4 + 0] / ODSY_GL_WINDOW_REACH;
        p[col * 4 + 1] = m[col * 4 + 1] / ODSY_GL_WINDOW_REACH;
        p[col * 4 + 2] = m[col * 4 + 2] /
                         (depth_scale * ODSY_GL_DEPTH_S) -
                         m[col * 4 + 3];
        p[col * 4 + 3] = m[col * 4 + 3];
    }
    sgi_gl_set_matrix(&s->gl_ctx, SGI_GL_PROJECTION, p);
    vp[0] = -ODSY_GL_WINDOW_REACH;
    vp[1] = -ODSY_GL_WINDOW_REACH;
    vp[2] = 2 * ODSY_GL_WINDOW_REACH;
    vp[3] = 2 * ODSY_GL_WINDOW_REACH;
}

/*
 * A board-side matrix load: 16 IEEE-754 floats, ROW-major on the wire
 * (docs/analysis/odyssey-gl-triangle-stream.md, confirmed against the traced
 * glRotatef(30) load). sgi_gl_set_matrix() takes column-major, so transpose
 * before the call. gl_matrix_slot is the slot the last matrix-select chose;
 * the normal slot (ODSY_GL_SLOT_NORMAL) is one the context refuses, and its
 * false return is expected. The texture matrix is kept as sent and applied
 * to the current ST by the shared context.
 */
static void odyssey_gl_transpose(const uint32_t *rows, float m[16])
{
    unsigned int row, col;

    for (row = 0; row < 4; row++) {
        for (col = 0; col < 4; col++) {
            uint32_t bits = rows[row * 4 + col];

            memcpy(&m[col * 4 + row], &bits, sizeof(bits));
        }
    }
}

/* Apply the kept slot-0x00 matrix under the kept block-16 sign. */
static void odyssey_gl_reload_combined(SGIOdysseyState *s)
{
    float m[16];

    odyssey_gl_transpose(s->gl_combined, m);
    odyssey_gl_load_combined(s, m);
}

static void odyssey_gl_load_matrix(SGIOdysseyState *s)
{
    float m[16];

    if (s->gl_matrix_slot == ODSY_GL_SLOT_TEXTURE) {
        memcpy(s->gl_texture_matrix, s->gl_matrix,
               sizeof(s->gl_texture_matrix));
        return;
    }
    if (s->gl_matrix_slot == ODSY_GL_SLOT_NORMAL_MATRIX) {
        memcpy(s->gl_normal_matrix, s->gl_matrix,
               sizeof(s->gl_normal_matrix));
        return;
    }
    if (s->gl_matrix_slot == ODSY_GL_SLOT_COMBINED) {
        memcpy(s->gl_combined, s->gl_matrix, sizeof(s->gl_combined));
        odyssey_gl_reload_combined(s);
        return;
    }
    if (s->gl_matrix_slot == SGI_GL_MODELVIEW) {
        /*
         * The combined matrix already carries the model-view
         * (odyssey_gl_load_combined()), so the shared context keeps its
         * identity model-view; this copy serves lighting's eye position.
         */
        memcpy(s->gl_modelview, s->gl_matrix, sizeof(s->gl_modelview));
        return;
    }
    odyssey_gl_transpose(s->gl_matrix, m);
    sgi_gl_set_matrix(&s->gl_ctx, s->gl_matrix_slot, m);
}

/*
 * The four floats behind index 16, gathered in cmd_operand[]. Their sign
 * word orients the combined matrix's depth row, which arrived just before
 * them, so the projection is rebuilt. A four-float load after any other
 * select has no known meaning and is reported.
 */
static void odyssey_gl_load_block16(SGIOdysseyState *s)
{
    if (s->gl_matrix_slot != ODSY_GL_SLOT_BLOCK16) {
        odyssey_cmd_unimplemented(s, ODSY_GL_LOAD_4F);
        return;
    }
    memcpy(s->gl_block16, s->cmd_operand, sizeof(s->gl_block16));
    odyssey_gl_reload_combined(s);
}

/*
 * The screen-aligned rectangle (ODSY_GL_RECT): its corners are window pixels,
 * so add the window origin, then fill the pixels whose centres fall inside the
 * half-open box between the two corners -- sx0 <= p + 0.5 < sx1 -- each
 * through the pixel test. It goes through the shared rasterizer's clear so
 * the float colour is quantized the same way a triangle's is; the clear's
 * rectangle is the box within the plane and its clip list the kept 0x204b
 * rectangles.
 *
 * glClear of the depth buffer is the same rectangle
 * (docs/analysis/odyssey-gl-shading-depth-lighting.md): the library sets the
 * 0x0452 depth write bit and puts the clear depth, d x 2S, in both corners'
 * z, and for a depth-only clear also zeroes the colour write mask, leaving
 * the colour operands uninitialised. So the rectangle writes depth, from the
 * first corner's z scaled to the 24-bit plane, when the depth write bit is
 * set, and writes colour only when the colour mask is not zero
 * (ESTABLISHED for the words). The library forces GL_ALWAYS around every
 * rectangle it sends; how the board would test a rectangle under another
 * comparison is not known, so that case is reported and the rectangle
 * written unconditionally. In index mode the first float is the clear index,
 * not red (glClearIndex(3) sends 3.0 there, ESTABLISHED from the colour-index
 * probe's capture), and the rectangle stores it like any index fragment.
 */
static void odyssey_gl_rect(SGIOdysseyState *s)
{
    float f[ODSY_GL_RECT_FLOATS];
    double x0, y0, x1, y1, d;
    unsigned int i, anchor_x = 0, anchor_y = 0;
    bool patterned = s->pattern_mode & ODSY_GL_RECT_PATTERN_ENABLE;
    SGIRasterTarget target;
    SGIRasterRules rules = odyssey_gl_operation_rules(s);
    SGIRasterOp op = {
        .kind = SGI_RASTER_CLEAR,
        .clear_colour_mask = UINT32_MAX,
    };

    memcpy(f, s->gl_rect, sizeof(f));
    if (patterned) {
        if (s->pattern_shape != ODSY_PATTERN_SHAPE_32X32 ||
            !isfinite(f[4]) || !isfinite(f[5]) ||
            !isfinite(f[7]) || !isfinite(f[8]) ||
            truncf(f[4]) != f[4] || truncf(f[5]) != f[5] ||
            truncf(f[7]) != f[7] || truncf(f[8]) != f[8]) {
            if (!s->gl_rect_pattern_logged) {
                s->gl_rect_pattern_logged = true;
                qemu_log_mask(LOG_UNIMP, "sgi-odyssey: patterned rectangle "
                              "requires shape 4 and finite integral corners\n");
            }
            return;
        }
        /* Reduce before conversion: integral floats can exceed int range. */
        anchor_x = (s->win_origin[0] % 32 + (int)fmod(f[4], 32)) & 31;
        anchor_y = (s->win_origin[1] % 32 + (int)fmod(f[5], 32)) & 31;
    }
    if ((s->gl_write_mask >> ODSY_WRITE_COLOUR_SHIFT) &
        ODSY_WRITE_COLOUR_MASK) {
        op.clear_buffers |= SGI_GL_COLOR_BUFFER_BIT;
    }
    if (s->gl_write_mask & ODSY_WRITE_DEPTH) {
        d = f[6] / (2 * ODSY_GL_DEPTH_S);
        d = MIN(MAX(d, 0.0), 1.0);
        op.clear_buffers |= SGI_GL_DEPTH_BUFFER_BIT;
        op.clear_depth24 = lround(d * ODSY_DEPTH24_MAX);
    }
    if ((s->pattern_mode & ODSY_MODE_DEPTH_TEST) &&
        (s->gl_depth_func & ODSY_DEPTH_FUNC_MASK) != SGI_GL_ALWAYS &&
        !s->gl_rect_depth_logged) {
        s->gl_rect_depth_logged = true;
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: depth-tested rectangle under "
                      "comparison %u is not modeled; written unconditionally"
                      "\n", s->gl_depth_func & ODSY_DEPTH_FUNC_MASK);
    }
    /*
     * No kept rectangle means every 0x204b rectangle is empty: the window is
     * fully covered and nothing may be drawn. The shared clear reads a clip
     * list with n = 0 as "no clip", so the rectangle is skipped here. (Reset
     * is not this case: rectangle 0 then spans the whole range.) This skip
     * becomes the shared SGIClipList.empty when the branch next rebases.
     */
    if (!s->clip_kept || !op.clear_buffers) {
        return;
    }
    x0 = (double)s->win_origin[0] + f[4];
    y0 = (double)s->win_origin[1] + f[5];
    x1 = (double)s->win_origin[0] + f[7];
    y1 = (double)s->win_origin[1] + f[8];
    if (x0 > x1) {
        double t = x0;

        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        double t = y0;

        y0 = y1;
        y1 = t;
    }
    /* Pixel p is covered when x0 <= p + 0.5 < x1, clamped to the plane. */
    x0 = MAX(ceil(x0 - 0.5), 0);
    y0 = MAX(ceil(y0 - 0.5), 0);
    x1 = MIN(ceil(x1 - 0.5), SGI_ODYSSEY_WIDTH) - 1;
    y1 = MIN(ceil(y1 - 0.5), SGI_ODYSSEY_HEIGHT) - 1;
    if (x0 > x1 || y0 > y1) {
        return;
    }
    /* The buffers the drawing mask selects; none leaves only depth. */
    op.ntargets = 0;
    for (unsigned b = 0; b < ODSY_BUFFERS; b++) {
        if (odyssey_draw_buffers(s) & (1u << b)) {
            op.targets[op.ntargets++] = b;
        }
    }
    op.x0 = x0;
    op.y0 = y0;
    op.x1 = x1;
    op.y1 = y1;
    for (i = 0; i < s->clip_kept; i++) {
        op.clip.r[i] = s->clip_rect[i];
    }
    op.clip.n = s->clip_kept;
    if (!odyssey_gl_index_mode(s)) {
        op.clear_colour_mask = (s->gl_write_mask >> ODSY_WRITE_COLOUR_SHIFT) &
                               ODSY_WRITE_COLOUR_MASK;
    }
    if (op.clear_buffers & SGI_GL_COLOR_BUFFER_BIT) {
        if (odyssey_gl_index_mode(s)) {
            /* glClearIndex travels as the first float (the probe's 3.0). */
            odyssey_gl_index_rgba(odyssey_float_index(s->gl_rect[0]),
                                  op.clear_rgba);
        } else {
            memcpy(op.clear_rgba, f, sizeof(op.clear_rgba));
        }
    }
    if (!patterned) {
        odyssey_gl_raster(s, &op);
        return;
    }
    target = odyssey_gl_target(s);
    /*
     * Proposed shape-4 policy: the first corner anchors a repeating bitmap,
     * independent of corner order. Admit runs through the existing clear so
     * clear bits preserve both colour and depth, with clipping unchanged.
     */
    for (int y = y0; y <= y1; y++) {
        uint32_t row = s->pattern_buffer[(y - anchor_y) & 31];
        int x = x0;

        op.y0 = op.y1 = y;
        while (x <= x1) {
            while (x <= x1 && !((row >> (31 - ((x - anchor_x) & 31))) & 1)) {
                x++;
            }
            if (x > x1) {
                break;
            }
            op.x0 = x;
            do {
                x++;
            } while (x <= x1 && ((row >> (31 - ((x - anchor_x) & 31))) & 1));
            op.x1 = x - 1;
            sgi_soft_raster(&rules, &target, &op);
        }
    }
    sgi_scanout_invalidate_all(&s->scanout);
}

/*
 * One QUADS rectangle: the bounding box of the four vertices from first,
 * painted through the id-plane, pattern or solid path.
 */
static void odyssey_gl_quad(SGIOdysseyState *s, uint32_t first)
{
    int32_t minx, maxx, miny, maxy;
    int64_t x0, x1, y0, y1;
    unsigned int i;

    minx = maxx = (int32_t)s->gl_vertex_x[first];
    miny = maxy = (int32_t)s->gl_vertex_y[first];
    for (i = first + 1; i < first + 4; i++) {
        int32_t vx = (int32_t)s->gl_vertex_x[i];
        int32_t vy = (int32_t)s->gl_vertex_y[i];

        minx = MIN(minx, vx);
        maxx = MAX(maxx, vx);
        miny = MIN(miny, vy);
        maxy = MAX(maxy, vy);
    }

    x0 = MAX((int64_t)minx, 0);
    x1 = MIN((int64_t)maxx, SGI_ODYSSEY_WIDTH);
    y0 = MAX((int64_t)miny, 0);
    y1 = MIN((int64_t)maxy, SGI_ODYSSEY_HEIGHT);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    /*
     * The X server's drawable-id plane write, marked by the 0x1017
     * destination word: it updates the did plane under the rectangle and
     * does not touch the colour framebuffer. The id value is the raw
     * COLOR_INDEX operand (id << 3), the mask the 0x1023 operand shifted
     * down from its << 12 position
     * (docs/analysis/odyssey-account-panel-black-band.md).
     */
    if (s->dest_buffer_word == ODSY_DEST_ID_PLANES) {
        odyssey_clear_planes(s, x0, y0, x1 - x0, y1 - y0,
                             s->gl_color_index,
                             (s->plane_write_mask >> ODSY_PLANE_MASK_SHIFT) &
                             0xff);
        return;
    }

    if (odyssey_pattern_enabled(s)) {
        odyssey_pattern_fill(s, x0, y0, x1 - x0, y1 - y0, x0 - minx,
                             y0 - miny, s->gl_color_is_index,
                             s->gl_color_is_index ?
                             s->gl_color_index : s->gl_color);
    } else {
        odyssey_fill_rect(s, x0, y0, x1 - x0, y1 - y0, s->gl_color_is_index,
                          s->gl_color_is_index ?
                          s->gl_color_index : s->gl_color);
    }
}

/* One line or polygon pixel in the current colour, clipped to the plane. */
static void odyssey_gl_plot(SGIOdysseyState *s, int64_t x, int64_t y)
{
    if (odyssey_pixel_allowed(s, x, y)) {
        odyssey_paint_pixel(s, x, y,
                            odyssey_fill_pixel(s->gl_color_is_index,
                                               s->gl_color_is_index ?
                                               s->gl_color_index :
                                               s->gl_color));
    }
}

/*
 * One line segment (docs/analysis/odyssey-account-icons.md). The DDX pushes an
 * X span (x, y, w) as the vertices (x, y), (x + w, y) for a solid span and in
 * the reverse order for a stippled one, and both are right only if a line
 * paints the pixels whose centres lie strictly between its endpoints along the
 * major axis -- [min, max) -- whichever way it runs (INFERRED from the two
 * DDX routines; the same is what lets the X server's line routine add a
 * one-pixel stub to draw a polyline's last point). Along the major axis
 * pixel i is painted at the minor-axis position of its centre, rounded
 * down. With the stipple enabled
 * (0x0404 bits 3 and 4) the k-th covered pixel from the first vertex, k = 1
 * next to it, is painted only where bit (k & 31) of the 0x0c48 pattern is set.
 */
static void odyssey_gl_segment(SGIOdysseyState *s, int32_t x1, int32_t y1,
                               int32_t x2, int32_t y2, bool stippled)
{
    uint32_t pattern = s->line_stipple[ODSY_LINE_STIPPLE_PATTERN];
    double dx = (double)x2 - x1, dy = (double)y2 - y1;
    int64_t lo, hi, i;

    if (stippled && s->line_stipple[ODSY_LINE_STIPPLE_CONTROL] !=
        ODSY_LINE_STIPPLE_CONTROL_32 && !s->line_stipple_reported) {
        s->line_stipple_reported = true;
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: integer line stipple control "
                      "0x%x is not modeled; drawn as a 32-pixel stipple\n",
                      s->line_stipple[ODSY_LINE_STIPPLE_CONTROL]);
    }

    if (dx == 0 && dy == 0) {
        return;
    }
    if (fabs(dx) >= fabs(dy)) {
        lo = MAX(MIN(x1, x2), 0);
        hi = MIN(MAX(x1, x2), SGI_ODYSSEY_WIDTH);
        for (i = lo; i < hi; i++) {
            double c = i + 0.5;
            uint32_t k = fabs(c - x1) + 0.5;

            if (!stippled || (pattern >> (k & 31)) & 1) {
                odyssey_gl_plot(s, i, floor(y1 + (c - x1) * dy / dx));
            }
        }
    } else {
        lo = MAX(MIN(y1, y2), 0);
        hi = MIN(MAX(y1, y2), SGI_ODYSSEY_HEIGHT);
        for (i = lo; i < hi; i++) {
            double c = i + 0.5;
            uint32_t k = fabs(c - y1) + 0.5;

            if (!stippled || (pattern >> (k & 31)) & 1) {
                odyssey_gl_plot(s, floor(x1 + (c - y1) * dx / dy), i);
            }
        }
    }
}

/*
 * A POLYGON is filled by the even-odd rule, the pixels whose centres are
 * inside. It streams like every other integer primitive, so instead of a
 * vertex list it keeps crossing marks (poly_marks): each edge toggles, on
 * every row whose centre lies in [its lower y, its upper y) -- so a shared
 * vertex is crossed once -- the mark at the first pixel whose centre is at or
 * right of the crossing. At END a pixel is inside when an odd number of marks
 * lie at or left of it, which is the same pixel set as pairing the sorted
 * crossings of each row. INFERRED: the two traced polygons are four-vertex
 * convex trapezoids, for which every inside rule agrees.
 */
static void odyssey_gl_polygon_edge(SGIOdysseyState *s, int32_t x1,
                                    int32_t y1, int32_t x2, int32_t y2)
{
    int64_t row, lo = MAX(MIN(y1, y2), 0);
    int64_t hi = MIN(MAX(y1, y2), SGI_ODYSSEY_HEIGHT);

    for (row = lo; row < hi; row++) {
        double c = row + 0.5;
        double px = ceil(x1 + (c - y1) * ((double)x2 - x1) /
                         ((double)y2 - y1) - 0.5);
        uint32_t col;

        /* A crossing right of the plane leaves every plane pixel outside. */
        if (px >= SGI_ODYSSEY_WIDTH) {
            continue;
        }
        col = px > 0 ? px : 0;
        s->poly_marks[row * SGI_ODYSSEY_WIDTH + col] ^= 1;
        s->poly_row_lo = MIN(s->poly_row_lo, row);
        s->poly_row_hi = MAX(s->poly_row_hi, row + 1);
        s->poly_col_lo = MIN(s->poly_col_lo, col);
    }
}

/* Clear the polygon's marks; with paint, first fill what they enclose. */
static void odyssey_gl_polygon_close(SGIOdysseyState *s, bool paint)
{
    uint32_t row, col;

    for (row = s->poly_row_lo; row < s->poly_row_hi; row++) {
        uint8_t *m = &s->poly_marks[row * SGI_ODYSSEY_WIDTH];
        uint8_t inside = 0;

        for (col = s->poly_col_lo; col < SGI_ODYSSEY_WIDTH; col++) {
            inside ^= m[col];
            m[col] = 0;
            if (paint && inside) {
                odyssey_gl_plot(s, col, row);
            }
        }
    }
    s->poly_row_lo = SGI_ODYSSEY_HEIGHT;
    s->poly_row_hi = 0;
    s->poly_col_lo = SGI_ODYSSEY_WIDTH;
}

static void odyssey_gl_hold(SGIOdysseyState *s, unsigned int slot, uint32_t x,
                            uint32_t y)
{
    s->gl_vertex_x[slot] = x;
    s->gl_vertex_y[slot] = y;
}

/*
 * One integer vertex, window origin already added. The command FIFO is a
 * stream with no batch size (docs/analysis/odyssey-gl-demos.md, failure 4),
 * so each piece is drawn as its last vertex arrives, the way the shared GL
 * context assembles the float path, holding only what a later piece needs:
 * a QUADS rectangle's corners, a LINES span's first vertex, a strip's first
 * and previous vertex (slots 0 and 1). A trailing partial piece -- a QUADS
 * remainder, an odd LINES vertex -- is never drawn, as glEnd drops it. A
 * primitive this path does not draw is reported at END.
 *
 * A POINTS vertex paints pixel (x, y). INFERRED: a line's pixels lie at the
 * minor-axis position rounded down (odyssey_gl_segment()), so an integer
 * vertex names the top-left corner of its pixel, and a size-one point lands
 * on the pixel GL's rule (floor of each coordinate) picks. The point size is
 * not modeled.
 */
static void odyssey_gl_vertex(SGIOdysseyState *s, uint32_t x, uint32_t y)
{
    uint32_t n = s->gl_vertex_count++;
    bool stippled = (s->pattern_mode & ODSY_PATTERN_MODE_ENABLE) ==
                    ODSY_PATTERN_MODE_ENABLE;

    /* The (0, 0) vertex a BEGIN-0xa image carries is not geometry. */
    if (s->image_active) {
        return;
    }
    switch (s->gl_primitive) {
    case ODSY_GL_POINTS:
        odyssey_gl_plot(s, (int32_t)x, (int32_t)y);
        break;
    case ODSY_GL_QUADS:
        /* One rectangle per four vertices (the greeter's frame fills). */
        odyssey_gl_hold(s, n % 4, x, y);
        if (n % 4 == 3) {
            odyssey_gl_quad(s, 0);
        }
        break;
    case ODSY_GL_LINES:
        if (n & 1) {
            odyssey_gl_segment(s, s->gl_vertex_x[0], s->gl_vertex_y[0], x, y,
                               stippled);
        } else {
            odyssey_gl_hold(s, 0, x, y);
        }
        break;
    case ODSY_GL_LINE_STRIP:
    case ODSY_GL_LINE_LOOP:
        if (n == 0) {
            odyssey_gl_hold(s, 0, x, y);
        } else {
            odyssey_gl_segment(s, s->gl_vertex_x[1], s->gl_vertex_y[1], x, y,
                               stippled);
        }
        odyssey_gl_hold(s, 1, x, y);
        break;
    case ODSY_GL_POLYGON:
        if (n == 0) {
            odyssey_gl_hold(s, 0, x, y);
        } else {
            odyssey_gl_polygon_edge(s, s->gl_vertex_x[1], s->gl_vertex_y[1],
                                    x, y);
        }
        odyssey_gl_hold(s, 1, x, y);
        break;
    default:
        break;
    }
}

static void odyssey_gl_end(SGIOdysseyState *s)
{
    uint32_t count = s->gl_vertex_count;
    bool stippled = (s->pattern_mode & ODSY_PATTERN_MODE_ENABLE) ==
                    ODSY_PATTERN_MODE_ENABLE;

    /*
     * END always closes the current primitive, the same as a real
     * immediate-mode glEnd(): the next BEGIN starts a fresh vertex count,
     * and a stray END with nothing newly accumulated since -- exactly
     * what odyssey_putchar() issues to close out its per-row pixel runs,
     * which never call BEGIN again -- paints nothing.
     */
    s->gl_vertex_count = 0;

    /*
     * END also closes a BEGIN-0xa image. The box and bursts already painted
     * as they streamed, so there is nothing more to do but leave image mode.
     */
    if (s->image_active) {
        s->image_active = false;
        s->image_bytes = 0;
        s->image_nbytes = 0;
        return;
    }

    /* Every piece but a loop's closing edge was drawn as it streamed. */
    switch (s->gl_primitive) {
    case ODSY_GL_POINTS:
    case ODSY_GL_QUADS:
    case ODSY_GL_LINES:
    case ODSY_GL_LINE_STRIP:
        break;
    case ODSY_GL_LINE_LOOP:
        if (count >= 2) {
            odyssey_gl_segment(s, s->gl_vertex_x[1], s->gl_vertex_y[1],
                               s->gl_vertex_x[0], s->gl_vertex_y[0],
                               stippled);
        }
        break;
    case ODSY_GL_POLYGON:
        /* Fewer than three vertices enclose nothing. */
        if (count >= 3) {
            odyssey_gl_polygon_edge(s, s->gl_vertex_x[1], s->gl_vertex_y[1],
                                    s->gl_vertex_x[0], s->gl_vertex_y[0]);
        }
        odyssey_gl_polygon_close(s, count >= 3);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: command FIFO OpenGL "
                      "primitive 0x%x is not modeled\n", s->gl_primitive);
        return;
    }
    sgi_scanout_invalidate_all(&s->scanout);
}

/* The table's physical address, unpacked from the descriptor (see above). */
static uint64_t odyssey_dma_desc_half(uint32_t word)
{
    return (word & ODSY_DMA_DESC_ADDR_LOW_MASK) |
           (((word >> ODSY_DMA_DESC_ADDR_HIGH_SHIFT) &
             ODSY_DMA_DESC_ADDR_HIGH_MASK) << 17);
}

static uint64_t odyssey_dma_table_address(uint32_t w1, uint32_t w3)
{
    return (odyssey_dma_desc_half(w1) << 24) | odyssey_dma_desc_half(w3);
}

/*
 * The hard DMA-done interrupt, the routed source whose vector is byte 15:8 of
 * ODSY_ROUTE_VECTORS (the kernel driver's DMA-done interrupt handler calls
 * its DMA-done wakeup in the shipped kernel). A DMA the driver asks to be
 * told about is held on two completions: the fence (the kernel driver's
 * DMA completion wait clears bit 0 of the region's state) and this
 * interrupt (the kernel driver's DMA-done wakeup pops the oldest region
 * off the driver's waiting list and clears bit 1); only when both have
 * happened does the waiter wake. Without this source such a DMA never
 * completes and its caller -- the X server, part-way through painting the
 * login greeter -- sleeps for good.
 *
 * The driver asks for it by setting bit 31 of the table header's word 7 in the
 * last block of an op it queues on its waiting list (the kernel driver's
 * DMA submission), and only then; the model raises it for exactly those
 * descriptors, once the transfer is finished, so no waiting region can be
 * released early. Of the login
 * greeter's three transfers only the first asks. Nothing is delivered before
 * the driver programs the routing.
 */
static void odyssey_dma_done(SGIOdysseyState *s)
{
    if (s->route_dest_upper || s->route_dest_lower) {
        odyssey_deliver_routed(s, ODSY_ROUTE_VECTOR_DMA_SHIFT, "DMA-done");
    }
}

/* The kernel's bytes-per-pixel table, indexed by word 7's bits 18:16. */
static const uint8_t odyssey_dma_bpp[ODSY_DMA_BPP_MASK + 1] = {
    2, 4, 8, 16, 12, 1, 6, 3,
};

/* One translation-table entry: a source address and its pixel-byte count. */
typedef struct OdysseyDMAEntry {
    uint64_t addr;
    uint32_t len;
} OdysseyDMAEntry;

/*
 * The table's entries, up to the zero terminator, into e[]. Returns how many,
 * or -1 (logged) when a read fails or no terminator comes within the budget.
 */
static int odyssey_dma_entries(const SGIDMABus *bus, uint64_t table,
                               uint64_t seq, OdysseyDMAEntry *e)
{
    int n;

    for (n = 0; n < ODSY_DMA_MAX_PAGES; n++) {
        uint8_t raw[8];
        uint64_t pte;

        if (!sgi_dma_read(bus, table + ODSY_DMA_TABLE_HEADER + n * 8,
                          raw, sizeof(raw), true, seq)) {
            return -1;
        }
        pte = ldq_be_p(raw);
        if (!(pte & ODSY_DMA_PTE_LEN_MASK)) {
            return n;
        }
        e[n].addr = pte >> ODSY_DMA_PTE_ADDR_SHIFT;
        e[n].len = pte & ODSY_DMA_PTE_LEN_MASK;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: DMA page table at 0x%"
                  PRIx64 " has no terminator within %u entries\n",
                  table, ODSY_DMA_MAX_PAGES);
    return -1;
}

/*
 * One pixel of a transfer at (x, y) of object obj, or of the colour buffers
 * the drawing mask selects when obj is -1, decoded by the
 * header's source format exactly as an image transfer's pixel is
 * (odyssey_decode_pixel()), a colour index 0 left unpainted. Clipped to the
 * plane only: the placement note names no other test for this path, and the
 * X server's 0x204b clip in force at each greeter transfer covers the
 * rectangle anyway, so no capture tells the two apart.
 */
static void odyssey_dma_put(SGIOdysseyState *s, int obj, int64_t x,
                            int64_t y, const uint8_t *src, uint32_t code)
{
    uint32_t pixel;
    bool paint;

    odyssey_decode_pixel(s, code, src, &pixel, &paint);
    if (obj >= 0) {
        if (paint && x >= 0 && y >= 0) {
            odyssey_object_store(s, obj, x, y, pixel);
        }
        return;
    }
    if (x < 0 || y < 0 || x >= SGI_ODYSSEY_WIDTH || y >= SGI_ODYSSEY_HEIGHT) {
        return;
    }
    if (!paint) {
        return;
    }
    for (unsigned b = 0; b < ODSY_BUFFERS; b++) {
        if (odyssey_draw_buffers(s) & (1u << b)) {
            odyssey_buffer_store(s, b, x, y, code == ODSY_IMAGE_CODE_CI ?
                                 pixel : (pixel & ~0xffu) | 0xff);
        }
    }
}

/*
 * The source format of a DMA: word 7's low byte, the same code as an image
 * transfer header's. A header with another code keeps the rule this model
 * used before the code was read, 4 bytes as 0x00RRGGBB and 1 byte as a
 * colour index, reported once.
 */
static uint32_t odyssey_dma_code(SGIOdysseyState *s, const uint32_t *hdr,
                                 uint32_t bpp)
{
    uint32_t code = hdr[ODSY_DMA_HDR_CONTROL] & ODSY_IMAGE_CODE_MASK;

    if (odyssey_pixel_bytes(code) == bpp) {
        return code;
    }
    if (!s->dma_code_reported) {
        s->dma_code_reported = true;
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: DMA source format 0x%x at %u "
                      "bytes a pixel is not modeled\n", code, bpp);
    }
    return bpp == 4 ? ODSY_IMAGE_CODE_XRGB : ODSY_IMAGE_CODE_CI;
}

/*
 * A transfer into the colour buffer the screen shows (obj -1) or into
 * board-memory object obj: paint the header's rectangle from the source
 * (docs/analysis/odyssey-dma-image-header.md). The
 * source is a stream of pixel bytes spread over the entries: each row takes
 * rowbytes bytes, moving to the next entry whenever the current one's count
 * runs out, then the cursor steps over skip bytes that are not counted. A skip
 * that runs past an entry's end is overtaken by the next entry, whose address
 * the driver already moved past the gap. No byte outside the entries is read.
 */
static void odyssey_dma_to_buffer(SGIOdysseyState *s, const SGIDMABus *bus,
                                  uint64_t table, uint64_t seq,
                                  const uint32_t *hdr, int obj)
{
    OdysseyDMAEntry e[ODSY_DMA_MAX_PAGES];
    uint32_t x = hdr[ODSY_DMA_HDR_ORIGIN] & 0xffff;
    uint32_t y = hdr[ODSY_DMA_HDR_ORIGIN] >> 16;
    uint32_t w = hdr[ODSY_DMA_HDR_SIZE] & 0xffff;
    uint32_t h = hdr[ODSY_DMA_HDR_SIZE] >> 16 ?: 0x10000;
    uint32_t skip = hdr[ODSY_DMA_HDR_SKIP];
    uint32_t rowbytes = hdr[ODSY_DMA_HDR_ROWBYTES] & ODSY_DMA_ROWBYTES_MASK;
    uint32_t bpp = odyssey_dma_bpp[(hdr[ODSY_DMA_HDR_CONTROL] >>
                                    ODSY_DMA_BPP_SHIFT) & ODSY_DMA_BPP_MASK];
    uint32_t code;
    g_autofree uint8_t *row = NULL;
    uint64_t payload = 0, pos = 0;
    uint32_t left, r, c;
    int n, i = 0;

    if (bpp != 4 && bpp != 3 && bpp != 1) {
        if (!s->dma_bpp_reported) {
            s->dma_bpp_reported = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: DMA to buffer 0x%x at %u "
                          "bytes a pixel is not modeled\n",
                          hdr[ODSY_DMA_HDR_DEST], bpp);
        }
        return;
    }
    code = odyssey_dma_code(s, hdr, bpp);
    n = odyssey_dma_entries(bus, table, seq, e);
    if (n < 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        payload += e[i].len;
    }
    if (rowbytes != (uint64_t)w * bpp || payload != (uint64_t)rowbytes * h) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: DMA header at 0x%" PRIx64
                      " disagrees with itself: %ux%u at %u bytes a pixel, "
                      "%u bytes a row, %" PRIu64 " bytes in its entries\n",
                      table, w, h, bpp, rowbytes, payload);
        return;
    }
    row = g_malloc(rowbytes);
    i = 0;
    left = n ? e[0].len : 0;
    for (r = 0; r < h; r++) {
        uint32_t got = 0;

        while (got < rowbytes) {
            uint32_t take;

            if (!left) {
                /* Checked above: the entries hold exactly h rows. */
                i++;
                pos = 0;
                left = e[i].len;
            }
            take = MIN(left, rowbytes - got);
            if (!sgi_dma_read(bus, e[i].addr + pos, row + got, take, true,
                              seq)) {
                goto done;
            }
            got += take;
            pos += take;
            left -= take;
        }
        if (left) {
            pos += skip;
        }
        for (c = 0; c < w; c++) {
            odyssey_dma_put(s, obj, (int64_t)x + c, (int64_t)y + r,
                            row + c * bpp, code);
        }
    }
done:
    sgi_scanout_invalidate_all(&s->scanout);
}

static bool odyssey_dma_report(SGIOdysseyState *s, uint32_t reason);

/*
 * Stage the pixel bytes first so a failed source read cannot leave a
 * partially painted rectangle. Entry lengths count pixels; row gaps advance
 * the address cursor only while the current entry has bytes remaining.
 */
static void odyssey_dma_draw_sentinel_short(SGIOdysseyState *s,
                                             const SGIDMABus *bus,
                                             uint64_t table, uint64_t seq,
                                             const uint32_t *hdr)
{
    OdysseyDMAEntry e[ODSY_DMA_MAX_PAGES];
    uint32_t x = hdr[ODSY_DMA_HDR_ORIGIN] & 0xffff;
    uint32_t y = hdr[ODSY_DMA_HDR_ORIGIN] >> 16;
    uint32_t w = hdr[ODSY_DMA_HDR_SIZE] & 0xffff;
    uint32_t h = hdr[ODSY_DMA_HDR_SIZE] >> 16 ?: 0x10000;
    uint32_t skip = hdr[ODSY_DMA_HDR_SKIP];
    uint32_t rowbytes = hdr[ODSY_DMA_HDR_ROWBYTES] & ODSY_DMA_ROWBYTES_MASK;
    uint64_t payload_len = 0, pos = 0, out = 0;
    g_autofree uint8_t *payload = NULL;
    uint32_t left, r;
    int n, i;

    if (hdr[ODSY_DMA_HDR_FORMAT] != 0x02635000 ||
        hdr[ODSY_DMA_HDR_CONTROL] != 0x7a02006b) {
        if (odyssey_dma_report(s, ODSY_DMA_REPORT_UPLOAD_SENTINEL)) {
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: DMA upload to sentinel "
                          "0x%x with format 0x%x and control 0x%x is not "
                          "modeled\n", hdr[ODSY_DMA_HDR_DEST],
                          hdr[ODSY_DMA_HDR_FORMAT],
                          hdr[ODSY_DMA_HDR_CONTROL]);
        }
        return;
    }

    n = odyssey_dma_entries(bus, table, seq, e);
    if (n < 0) {
        return;
    }
    for (int j = 0; j < n; j++) {
        payload_len += e[j].len;
    }
    if (!w || !h || rowbytes != (uint64_t)w * 8 ||
        payload_len != (uint64_t)rowbytes * h) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: draw-sentinel DMA "
                      "header at 0x%" PRIx64 " disagrees with its payload\n",
                      table);
        return;
    }

    payload = g_malloc(payload_len);
    i = 0;
    left = n ? e[0].len : 0;
    for (r = 0; r < h; r++) {
        uint32_t got = 0;

        while (got < rowbytes) {
            uint32_t take;

            if (!left) {
                i++;
                pos = 0;
                left = e[i].len;
            }
            take = MIN(left, rowbytes - got);
            if (pos > UINT64_MAX - e[i].addr ||
                !sgi_dma_read(bus, e[i].addr + pos,
                              payload + out + got, take, true, seq)) {
                return;
            }
            got += take;
            pos += take;
            left -= take;
        }
        if (left) {
            pos += skip;
        }
        out += rowbytes;
    }

    for (r = 0; r < h; r++) {
        for (uint32_t c = 0; c < w; c++) {
            const uint8_t *src = payload + (uint64_t)r * rowbytes + c * 8;
            uint32_t rgba, red = lduw_be_p(src), green = lduw_be_p(src + 2);
            uint32_t blue = lduw_be_p(src + 4), alpha = lduw_be_p(src + 6);
            int64_t px = (int64_t)x + c + s->win_origin[0];
            int64_t py = (int64_t)y + h - 1 - r + s->win_origin[1];

            rgba = odyssey_pack_pixel(
                (red * 255 + UINT16_MAX / 2) / UINT16_MAX,
                (green * 255 + UINT16_MAX / 2) / UINT16_MAX,
                (blue * 255 + UINT16_MAX / 2) / UINT16_MAX);
            rgba = (rgba & ~0xffu) |
                   ((alpha * 255 + UINT16_MAX / 2) / UINT16_MAX);
            odyssey_paint_pixel(s, px, py, rgba);
        }
    }
    sgi_scanout_invalidate_all(&s->scanout);
}

/* True the first time reason (ODSY_DMA_REPORT_*) comes up: report it then. */
static bool odyssey_dma_report(SGIOdysseyState *s, uint32_t reason)
{
    if (s->dma_read_reported & reason) {
        return false;
    }
    s->dma_read_reported |= reason;
    return true;
}

/*
 * A read's host bytes for one pixel: how many, and for each byte the plane
 * word's channel it carries (0 red to 3 alpha of the 0xRRGGBBAA word), a
 * zero byte, or the word as a colour index.
 */
#define ODSY_DMA_BYTE_ZERO 4
#define ODSY_DMA_BYTE_INDEX 5

typedef struct OdysseyDMALayout {
    uint8_t code;
    uint8_t bytes;
    uint8_t from[8];
} OdysseyDMALayout;

/*
 * GL reads from a 12-bit-a-channel RGBA window (source class 3), by
 * transfer code, as the library defines the host bytes. The model keeps
 * eight bits a channel, so a byte read returns them as stored and a
 * 16-bit read replicates each byte. APPROXIMATION: how the board narrows a
 * 12-bit channel to eight bits is unknown; the stored byte stands for the
 * channel's value at the model's precision.
 */
static const OdysseyDMALayout odyssey_dma_gl_layouts[] = {
    { 1, 4, { 0, 1, 2, 3 } },   /* RGBA, UNSIGNED_BYTE: R, G, B, A */
    { 6, 4, { 3, 2, 1, 0 } },   /* A, B, G, R */
    { 2, 3, { 0, 1, 2 } },      /* RGB, UNSIGNED_BYTE */
    { 7, 3, { 2, 1, 0 } },      /* BGR, UNSIGNED_BYTE */
    { 9, 8, { 0, 0, 1, 1, 2, 2, 3, 3 } }, /* RGBA, UNSIGNED_SHORT */
};

/* An eight-bit RGBA visual uses code 8 for RGBA/UNSIGNED_BYTE. */
static const OdysseyDMALayout odyssey_dma_gl_rgba8_layout = {
    8, 4, { 0, 1, 2, 3 },
};

/*
 * X reads, by word 7's format byte: the upload decode
 * (odyssey_decode_pixel()) run backwards. The luminance code is not
 * invertible and no read of it has been seen, so it is refused.
 */
static const OdysseyDMALayout odyssey_dma_x_layouts[] = {
    { ODSY_IMAGE_CODE_XRGB, 4, { ODSY_DMA_BYTE_ZERO, 0, 1, 2 } },
    { ODSY_IMAGE_CODE_RGBA, 4, { 0, 1, 2, 3 } },
    { ODSY_IMAGE_CODE_RGB, 3, { 0, 1, 2 } },
    { ODSY_IMAGE_CODE_CI, 1, { ODSY_DMA_BYTE_INDEX } },
};

/* The read's layout, or NULL (reported once) when it is not modeled. */
static const OdysseyDMALayout *odyssey_dma_read_layout(SGIOdysseyState *s,
                                                       const uint32_t *hdr,
                                                       uint32_t bpp)
{
    uint32_t xcode = hdr[ODSY_DMA_HDR_CONTROL] & ODSY_IMAGE_CODE_MASK;
    uint32_t fmt = hdr[ODSY_DMA_HDR_FORMAT];
    uint32_t cls = fmt & ODSY_DMA_FMT_CLASS_MASK;
    uint32_t code = (fmt >> ODSY_DMA_FMT_CODE_SHIFT) & ODSY_DMA_FMT_CODE_MASK;
    const OdysseyDMALayout *l = NULL;
    unsigned int i;

    if (xcode) {
        for (i = 0; i < ARRAY_SIZE(odyssey_dma_x_layouts); i++) {
            if (odyssey_dma_x_layouts[i].code == xcode) {
                l = &odyssey_dma_x_layouts[i];
            }
        }
    } else if (cls == ODSY_DMA_GL_CLASS_RGBA12) {
        for (i = 0; i < ARRAY_SIZE(odyssey_dma_gl_layouts); i++) {
            if (odyssey_dma_gl_layouts[i].code == code) {
                l = &odyssey_dma_gl_layouts[i];
            }
        }
    } else if (cls == ODSY_DMA_GL_CLASS_RGBA8 && code == 8) {
        l = &odyssey_dma_gl_rgba8_layout;
    }
    if (l && l->bytes == bpp) {
        return l;
    }
    if (odyssey_dma_report(s, ODSY_DMA_REPORT_LAYOUT)) {
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: DMA read with format byte "
                      "0x%x, word 4 0x%x (class %u, code %u) at %u bytes a "
                      "pixel is not modeled\n", xcode, fmt, cls, code, bpp);
    }
    return NULL;
}

/*
 * Where a read comes from: object obj, or when obj is -1 the colour buffer
 * each pixel's drawable shows (front) or the other one, with the
 * rectangle's top-left at (x, y) in that buffer's coordinates.
 */
typedef struct OdysseyDMASource {
    int obj;
    bool front;
    int64_t x, y;
} OdysseyDMASource;

/*
 * Resolve word 0 to the read's source (the comment at ODSY_DMA_TO_HOST).
 * False, reported once, when it names nothing the model can read. A
 * sentinel never creates or names a board-memory object.
 */
static bool odyssey_dma_read_source(SGIOdysseyState *s, const uint32_t *hdr,
                                    OdysseyDMASource *src)
{
    uint32_t base = hdr[ODSY_DMA_HDR_DEST];
    uint32_t fmt = hdr[ODSY_DMA_HDR_FORMAT];
    uint32_t selector = (fmt >> ODSY_DMA_FMT_SELECTOR_SHIFT) &
                        ODSY_DMA_FMT_SELECTOR_MASK;

    src->obj = -1;
    /* Front and back per pixel (ODSY_DMA_TO_HOST, odyssey_dma_read_pixel()). */
    src->front = true;
    src->x = hdr[ODSY_DMA_HDR_ORIGIN] & 0xffff;
    src->y = hdr[ODSY_DMA_HDR_ORIGIN] >> 16;

    if (base & ODSY_DMA_SENTINEL) {
        if (base == ODSY_DMA_SENTINEL_DRAW &&
            selector == ODSY_DMA_FMT_SELECTOR_DRAW) {
            if (odyssey_dma_report(s, ODSY_DMA_REPORT_DRAW_SENTINEL)) {
                qemu_log_mask(LOG_UNIMP, "sgi-odyssey: DMA read of the draw "
                              "drawable (0x%x); never captured, read as the "
                              "read drawable is\n", base);
            }
        } else if (base != ODSY_DMA_SENTINEL_READ ||
                   selector != ODSY_DMA_FMT_SELECTOR_READ) {
            if (odyssey_dma_report(s, ODSY_DMA_REPORT_SENTINEL)) {
                qemu_log_mask(LOG_UNIMP, "sgi-odyssey: DMA read of sentinel "
                              "0x%x with word 4 0x%x is not modeled\n",
                              base, fmt);
            }
            return false;
        }
        /*
         * INFERRED: both sentinels are the current window, whose origin
         * the model tracks from 0x841; the model keeps no separate read and
         * draw drawable.
         */
        src->x += s->win_origin[0];
        src->y += s->win_origin[1];
        src->front = fmt & ODSY_DMA_FMT_FRONT;
        return true;
    }
    if (base && base == s->dma_colour_base) {
        return true;
    }
    src->obj = base ? odyssey_object_find(s, base) : -1;
    if (src->obj < 0) {
        if (odyssey_dma_report(s, ODSY_DMA_REPORT_SOURCE)) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: DMA read of board "
                          "base 0x%x, which holds no buffer\n", base);
        }
        return false;
    }
    return true;
}

/*
 * Source pixel (x, y) as the plane's 0xRRGGBBAA word, or a raw index. A
 * colour-buffer pixel comes from the buffer its main drawable shows, the
 * displayed-buffer bit scanout uses (odyssey_resolve_row()), or from the
 * other buffer for a back read. (x, y) is inside the plane: the caller
 * checked the rectangle.
 */
static uint32_t odyssey_dma_read_pixel(SGIOdysseyState *s,
                                       const OdysseyDMASource *src,
                                       int64_t x, int64_t y)
{
    unsigned int shown;

    if (src->obj >= 0) {
        return odyssey_object_load(s, src->obj, x, y);
    }
    shown = (s->displayed_buffer >> odyssey_main_id(s, x, y)) & 1;
    return odyssey_buffer_load(s, src->front ? shown : !shown, x, y);
}

/*
 * One piece of a read's host bytes: len bytes of the packed payload from
 * offset src, mapped at offset at of its own page list. Each piece is its
 * own map so a row gap between two pieces is never written back.
 */
typedef struct OdysseyDMAPiece {
    SGIDMAMap map;
    uint64_t at;
    uint64_t src;
    uint32_t len;
} OdysseyDMAPiece;

/* The 16 KiB pages an entry's longest piece can touch. */
#define ODSY_DMA_PIECE_PAGES \
    DIV_ROUND_UP(ODSY_DMA_PTE_LEN_MASK + ODSY_DMA_PAGE_SIZE, ODSY_DMA_PAGE_SIZE)

/*
 * Map piece p at guest address addr: an entry is one physical extent, so
 * the pages it runs over are consecutive.
 */
static int odyssey_dma_map_piece(const SGIDMABus *bus, OdysseyDMAPiece *p,
                                 uint64_t addr)
{
    uint64_t pages[ODSY_DMA_PIECE_PAGES];
    uint64_t first = addr & ~(uint64_t)(ODSY_DMA_PAGE_SIZE - 1);
    SGIDMAPageList pl = {
        .addr = pages,
        .npages = DIV_ROUND_UP(addr - first + p->len, ODSY_DMA_PAGE_SIZE),
        .page_size = ODSY_DMA_PAGE_SIZE,
    };

    assert(pl.npages <= ARRAY_SIZE(pages));
    for (uint32_t i = 0; i < pl.npages; i++) {
        pages[i] = first + (uint64_t)i * ODSY_DMA_PAGE_SIZE;
    }
    p->at = addr - first;
    return sgi_dma_map(bus, &p->map, &pl, p->at, p->len,
                       DMA_DIRECTION_FROM_DEVICE, false, 0);
}

/*
 * Pack the read's rectangle into out, host row by host row, rowbytes each.
 * A colour-index byte is the plane word itself, taken only where scanout
 * would take that word as an index: a colour-buffer pixel whose main
 * drawable's display mode is an index group (odyssey_resolve_row()), with a
 * word that fits eight bits. INFERRED as the scanout rule is. Anything else
 * refuses the whole read, reported once: a pixel of an RGB-mode drawable, a
 * 12-bit index above 0xff, the synthetic RGB word the screen-aligned GL
 * rectangle's index path stores, or any board-memory object, which records
 * no display mode. A colour layout over an index-mode pixel returns the raw
 * index word's bytes as colour and is reported once: no inverse colormap is
 * applied.
 */
static bool odyssey_dma_read_pack(SGIOdysseyState *s,
                                  const OdysseyDMASource *src,
                                  const OdysseyDMALayout *l, uint32_t w,
                                  uint32_t h, bool bottom_up, uint8_t *out)
{
    bool index_read = l->from[0] == ODSY_DMA_BYTE_INDEX;

    if (index_read && src->obj >= 0) {
        if (odyssey_dma_report(s, ODSY_DMA_REPORT_INDEX_OBJECT)) {
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: DMA index read of board "
                          "object 0x%x, which records no display mode\n",
                          s->obj_base[src->obj]);
        }
        return false;
    }
    for (uint32_t r = 0; r < h; r++) {
        uint32_t board_row = bottom_up ? h - 1 - r : r;

        for (uint32_t c = 0; c < w; c++) {
            int64_t x = src->x + c, y = src->y + board_row;
            uint32_t word = odyssey_dma_read_pixel(s, src, x, y);
            uint32_t group = ODSY_GAMMA_GROUP;

            if (src->obj < 0) {
                group = odyssey_display_group(s, odyssey_main_id(s, x, y));
            }
            if (index_read && (group >= ODSY_GAMMA_GROUP || word > 0xff)) {
                if (odyssey_dma_report(s, ODSY_DMA_REPORT_INDEX)) {
                    qemu_log_mask(LOG_UNIMP, "sgi-odyssey: DMA index read at "
                                  "(%" PRId64 ", %" PRId64 ") finds word "
                                  "0x%x in display-mode group %u, not an "
                                  "eight-bit index\n", x, y, word, group);
                }
                return false;
            }
            if (!index_read && group < ODSY_GAMMA_GROUP &&
                odyssey_dma_report(s, ODSY_DMA_REPORT_COLOUR_OVER_INDEX)) {
                qemu_log_mask(LOG_UNIMP, "sgi-odyssey: DMA colour read at "
                              "(%" PRId64 ", %" PRId64 ") finds an index "
                              "pixel (display-mode group %u); its word 0x%x "
                              "is returned as colour\n", x, y, group, word);
            }
            for (unsigned int b = 0; b < l->bytes; b++) {
                uint8_t from = l->from[b];

                *out++ = from == ODSY_DMA_BYTE_INDEX ? word :
                         from == ODSY_DMA_BYTE_ZERO ? 0 :
                         word >> (24 - 8 * from);
            }
        }
    }
    return true;
}

/*
 * Write the packed payload into the table's entries: rowbytes bytes a host
 * row, moving to the next entry when one is used up, then skipping skip
 * bytes that are not counted, exactly as an upload reads its source
 * (odyssey_dma_to_buffer()). Every piece is mapped before any byte is
 * written, so a table that fails to map writes nothing. No byte outside the
 * pieces is touched.
 */
static bool odyssey_dma_read_write(SGIOdysseyState *s, const SGIDMABus *bus,
                                   const OdysseyDMAEntry *e, int n,
                                   uint32_t rowbytes, uint32_t h,
                                   uint32_t skip, uint8_t *payload)
{
    g_autoptr(GArray) pieces = g_array_new(false, true,
                                           sizeof(OdysseyDMAPiece));
    uint64_t pos = 0, done = 0;
    uint32_t left = n ? e[0].len : 0;
    bool ok = true;
    int i = 0;

    for (uint32_t r = 0; r < h && ok; r++) {
        uint32_t got = 0;

        while (got < rowbytes) {
            OdysseyDMAPiece p = { 0 };

            if (!left) {
                /* Checked by the caller: the entries hold exactly h rows. */
                i++;
                pos = 0;
                left = e[i].len;
            }
            p.len = MIN(left, rowbytes - got);
            p.src = done;
            g_array_append_val(pieces, p);
            if (odyssey_dma_map_piece(bus, &g_array_index(pieces,
                                      OdysseyDMAPiece, pieces->len - 1),
                                      e[i].addr + pos)) {
                ok = false;
                break;
            }
            got += p.len;
            done += p.len;
            pos += p.len;
            left -= p.len;
        }
        if (left) {
            pos += skip;
        }
    }
    for (guint k = 0; k < pieces->len; k++) {
        OdysseyDMAPiece *p = &g_array_index(pieces, OdysseyDMAPiece, k);

        if (ok && !sgi_dma_map_copy(&p->map, p->at, payload + p->src,
                                    p->len, true)) {
            ok = false;
        }
        sgi_dma_unmap(&p->map);
    }
    if (!ok && odyssey_dma_report(s, ODSY_DMA_REPORT_WRITE)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: DMA read's host pages "
                      "could not all be written\n");
    }
    return ok;
}

/*
 * A board-to-host transfer (word 7 bit 24; the comment at
 * ODSY_DMA_TO_HOST): read the header's rectangle from its source and write
 * it, laid out as the header says, into the table's entries. Any refusal --
 * an unknown sentinel or base, an unmodeled layout, a header that
 * disagrees with its entries or a rectangle outside its source -- is
 * reported once and writes nothing to the guest; the caller still raises
 * DMA-done when bit 31 asks for it, since no completion record carries a
 * status and the kernel's own fence ends its wait regardless. The payload
 * is in guest memory before this returns, so before DMA-done and before the
 * 0x0a5a fence the driver queues behind the descriptor.
 */
static void odyssey_dma_from_board(SGIOdysseyState *s, const SGIDMABus *bus,
                                   uint64_t table, uint64_t seq,
                                   const uint32_t *hdr)
{
    OdysseyDMAEntry e[ODSY_DMA_MAX_PAGES];
    uint32_t w = hdr[ODSY_DMA_HDR_SIZE] & 0xffff;
    uint32_t h = hdr[ODSY_DMA_HDR_SIZE] >> 16 ?: 0x10000;
    uint32_t skip = hdr[ODSY_DMA_HDR_SKIP];
    uint32_t rowbytes = hdr[ODSY_DMA_HDR_ROWBYTES] & ODSY_DMA_ROWBYTES_MASK;
    uint32_t bpp = odyssey_dma_bpp[(hdr[ODSY_DMA_HDR_CONTROL] >>
                                    ODSY_DMA_BPP_SHIFT) & ODSY_DMA_BPP_MASK];
    const OdysseyDMALayout *l;
    OdysseyDMASource src;
    g_autofree uint8_t *payload = NULL;
    uint64_t total = 0;
    bool inside;
    int n, i;

    l = odyssey_dma_read_layout(s, hdr, bpp);
    if (!l || !odyssey_dma_read_source(s, hdr, &src)) {
        return;
    }
    n = odyssey_dma_entries(bus, table, seq, e);
    if (n < 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        total += e[i].len;
    }
    if (rowbytes != (uint64_t)w * bpp || total != (uint64_t)rowbytes * h) {
        if (odyssey_dma_report(s, ODSY_DMA_REPORT_SHAPE)) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: DMA read header at "
                          "0x%" PRIx64 " disagrees with itself: %ux%u at %u "
                          "bytes a pixel, %u bytes a row, %" PRIu64 " bytes "
                          "in its entries\n", table, w, h, bpp, rowbytes,
                          total);
        }
        return;
    }
    if (src.obj >= 0) {
        /*
         * Rows past an object's store were never written: the model holds
         * no value for them, so a read reaching them is refused rather
         * than answered with zeros.
         */
        inside = src.x + w <= s->obj_width[src.obj] &&
                 src.y + h <= s->obj_rows[src.obj];
    } else {
        inside = src.x >= 0 && src.y >= 0 &&
                 src.x + w <= SGI_ODYSSEY_WIDTH &&
                 src.y + h <= SGI_ODYSSEY_HEIGHT;
    }
    if (!inside) {
        if (odyssey_dma_report(s, ODSY_DMA_REPORT_BOUNDS)) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: DMA read of %ux%u at "
                          "(%" PRId64 ", %" PRId64 ") from 0x%x leaves its "
                          "source\n", w, h, src.x, src.y,
                          hdr[ODSY_DMA_HDR_DEST]);
        }
        return;
    }
    payload = g_malloc(total);
    if (!odyssey_dma_read_pack(s, &src, l, w, h,
                               hdr[ODSY_DMA_HDR_CONTROL] & ODSY_DMA_BOTTOM_UP,
                               payload) ||
        !odyssey_dma_read_write(s, bus, e, n, rowbytes, h, skip, payload)) {
        return;
    }
    trace_sgi_odyssey_dma_read(seq, hdr[ODSY_DMA_HDR_DEST], src.x, src.y, w, h,
                               total);
}

/*
 * A 0xe01/0xe03 descriptor: read the table's header, move the pixels, and
 * raise the DMA-done interrupt when the header asks for it. A header with
 * word 7 bit 24 set is a read (odyssey_dma_from_board()); any other names
 * its destination in word 0: the colour buffer the screen shows, or an
 * off-screen object -- the X server's 0x16a000 buffer, or a texture level
 * too large for the FIFO (docs/analysis/odyssey-gl-texturing.md) -- whose
 * row length is word 2's (16-pixel units), created when no 0x1017 named it
 * first. A zero word 0 names no buffer and is refused. An upload to a
 * sentinel (GL's DMA image write) is accepted only by the experimental
 * draw-sentinel SHORT adapter. The 0xe03 opcode, which only the diagnostic
 * region path emits, is reported and not performed.
 */
static void odyssey_dma_descriptor(SGIOdysseyState *s, bool readback)
{
    uint64_t table = odyssey_dma_table_address(s->cmd_operand[0],
                                               s->cmd_operand[1]);
    uint64_t seq = ++s->dma_seq;
    SGIDMABus bus = {
        .as = &address_space_memory,
        .ops = &odyssey_head_ops,
        .opaque = s,
    };
    uint8_t raw[ODSY_DMA_TABLE_HEADER];
    uint32_t hdr[ODSY_DMA_HDR_WORDS];
    unsigned int i;

    if (!sgi_dma_read(&bus, table, raw, sizeof(raw), true, seq)) {
        return;
    }
    for (i = 0; i < ODSY_DMA_HDR_WORDS; i++) {
        hdr[i] = ldl_be_p(&raw[i * 4]);
    }
    if (readback) {
        if (!s->dma_readback_reported) {
            s->dma_readback_reported = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: DMA opcode 0xe03 (buffer "
                          "0x%x) is not modeled\n", hdr[ODSY_DMA_HDR_DEST]);
        }
    } else if (hdr[ODSY_DMA_HDR_CONTROL] & ODSY_DMA_TO_HOST) {
        odyssey_dma_from_board(s, &bus, table, seq, hdr);
    } else if (hdr[ODSY_DMA_HDR_DEST] & ODSY_DMA_SENTINEL) {
        if (hdr[ODSY_DMA_HDR_DEST] == ODSY_DMA_SENTINEL_DRAW) {
            odyssey_dma_draw_sentinel_short(s, &bus, table, seq, hdr);
        } else if (odyssey_dma_report(s, ODSY_DMA_REPORT_UPLOAD_SENTINEL)) {
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: DMA upload to sentinel "
                          "0x%x with word 4 0x%x is not modeled\n",
                          hdr[ODSY_DMA_HDR_DEST], hdr[ODSY_DMA_HDR_FORMAT]);
        }
    } else if (s->dma_colour_base &&
               hdr[ODSY_DMA_HDR_DEST] == s->dma_colour_base) {
        odyssey_dma_to_buffer(s, &bus, table, seq, hdr, -1);
    } else if (!hdr[ODSY_DMA_HDR_DEST]) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: DMA header at 0x%" PRIx64
                      " names no destination buffer\n", table);
    } else {
        /* The header's row length is the object's, as a 0x1017 select's. */
        int obj = odyssey_object_select(s, hdr[ODSY_DMA_HDR_DEST],
                                        hdr[ODSY_DMA_HDR_WIDTH] *
                                        ODSY_DEST_WIDTH_UNIT);

        if (obj >= 0) {
            odyssey_dma_to_buffer(s, &bus, table, seq, hdr, obj);
        }
    }
    if (hdr[ODSY_DMA_HDR_CONTROL] & ODSY_DMA_IRQ_REQUEST) {
        odyssey_dma_done(s);
    }
}

/* odyssey_cmd_word()'s parser states, stored in cmd_state as a uint32_t. */
enum {
    ODSY_CMD_IDLE,
    ODSY_CMD_OPERANDS,
    ODSY_CMD_COPY_RECT,
    ODSY_CMD_PATTERN_LOAD,
    ODSY_CMD_IMAGE_BOX_LOAD, /* capturing the 10 raster-box floats */
    ODSY_CMD_IMAGE_BURST,    /* painting a pixel burst into the open image */
    ODSY_CMD_MATRIX_LOAD,    /* gathering a board-side matrix's 16 floats */
    ODSY_CMD_GL_RECT,        /* gathering a rectangle's colour and corners */
};

static const unsigned odyssey_query_offset[] = {
    0x900, 0x914, 0x928, 0x950, 0x978,
};

/* Public initial values: index 1, colour white, normal +Z, edge true. */
static const uint32_t odyssey_query_defaults[5][5] = {
    [ODSY_QUERY_INDEX] = { ODSY_GL_COLOR_INDEX, 1 },
    [ODSY_QUERY_COLOUR] = {
        ODSY_GL_COLOR_3F, ODSY_F32_ONE, ODSY_F32_ONE, ODSY_F32_ONE,
    },
    [ODSY_QUERY_NORMAL] = { ODSY_GL_NORMAL_3F, 0, 0, ODSY_F32_ONE },
    [ODSY_QUERY_TEXCOORD] = { ODSY_GL_TEXCOORD_2F },
    [ODSY_QUERY_EDGE] = { 0x2f00c02e },
};

static void odyssey_query_records_reset(SGIOdysseyState *s)
{
    memcpy(s->query_current, odyssey_query_defaults,
           sizeof(s->query_current));
    s->query_current_valid = 0x1f;
}

static void odyssey_query_cancel(SGIOdysseyState *s)
{
    if (s->query_stage) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-odyssey: interrupted current-query sequence\n");
    }
    s->query_epoch++;
    s->query_stage = 0;
    s->query_count = 0;
    s->query_sync_index = 0;
    s->query_base = 0;
    s->query_sync_base = 0;
    s->query_captured_epoch = 0;
    s->query_captured_valid = 0;
    memset(s->query_captured, 0, sizeof(s->query_captured));
    s->cmd_state = ODSY_CMD_IDLE;
    s->cmd_tag = 0;
    s->cmd_operand_count = 0;
    s->cmd_operand_index = 0;
    memset(s->cmd_operand, 0, sizeof(s->cmd_operand));
}

/*
 * Retain a completed setter in its public record: the whole command word and
 * its raw operands, never a converted value
 * (docs/reviews/odyssey-query-formats-7-13-evidence-20260930.md). The GL
 * consumer reads the format from the word's low six bits: 6 and 7 for the
 * index, 8, 13, 14 and 21 for the colour, 22 to 25 for the normal. The
 * 0xce80 twins of the index and colour words set the same records as their
 * 0xc580 siblings; that is a model choice, since whether the board updates
 * its queried value for them is unknown.
 *
 * The consumer indexes a 256-entry table with a byte colour's raw words and
 * no mask, so a byte operand above 255 has no defined answer. Such a record
 * is retained for the context save but makes queries fail observably until
 * the next colour setter. Texture-coordinate forms other than the two-float
 * one stay unsupported the same way.
 */
static void odyssey_query_update(SGIOdysseyState *s, uint32_t tag)
{
    unsigned record, i;
    bool supported = true;

    switch (tag) {
    case ODSY_GL_COLOR_INDEX:
    case ODSY_GL_REPLAY_INDEX:
    case ODSY_GL_COLOR_INDEX_F:
    case ODSY_GL_REPLAY_INDEX_F:
        record = ODSY_QUERY_INDEX;
        break;
    case ODSY_GL_COLOR_3UB:
    case ODSY_MODE_INDEX_ZEROS:
        record = ODSY_QUERY_COLOUR;
        for (i = 0; i < 3; i++) {
            supported &= s->cmd_operand[i] <= 0xff;
        }
        break;
    case ODSY_GL_COLOR_3I:
    case ODSY_GL_INDEX_MODE_COLOR_3I:
    case ODSY_GL_COLOR_3F:
    case ODSY_GL_INDEX_MODE_COLOR_3F:
    case ODSY_GL_COLOR_4F:
    case ODSY_GL_INDEX_MODE_COLOR_4F:
        record = ODSY_QUERY_COLOUR;
        break;
    case ODSY_GL_NORMAL_3B:
    case ODSY_GL_NORMAL_3S:
    case ODSY_GL_NORMAL_3I:
    case ODSY_GL_NORMAL_3F:
        record = ODSY_QUERY_NORMAL;
        break;
    case ODSY_GL_TEXCOORD_2F:
        record = ODSY_QUERY_TEXCOORD;
        break;
    case 0x2f00c02e:
    case 0x3000c02e:
        record = ODSY_QUERY_EDGE;
        break;
    default:
        if ((tag & ODSY_GL_TEXCOORD_SHAPE) != ODSY_GL_TEXCOORD_TAG) {
            return;
        }
        record = ODSY_QUERY_TEXCOORD;
        supported = false;
        break;
    }
    s->query_current_valid &= ~(1u << record);
    if (record != ODSY_QUERY_TEXCOORD || supported) {
        /* Every word mapped to a record above has at most four operands. */
        assert(odyssey_cmd_arity(tag) < ARRAY_SIZE(s->query_current[record]));
        memset(s->query_current[record], 0, sizeof(s->query_current[record]));
        s->query_current[record][0] = tag;
        memcpy(&s->query_current[record][1], s->cmd_operand,
               odyssey_cmd_arity(tag) * sizeof(uint32_t));
    }
    if (supported) {
        s->query_current_valid |= 1u << record;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-odyssey: unsupported current-query record %u "
                      "tag 0x%08x\n", record, tag);
    }
}

/*
 * A query's completion words must not report failure after changing guest
 * memory. Admit only a whole aligned scalar inside one direct writable RAM
 * section, then store it with release ordering so the payload stores before
 * it are visible first. Slow mappings, read-only sections and a cache that
 * extends into a neighbouring section all fail before the store.
 */
static int odyssey_publish_word(const SGIDMABus *bus, uint64_t addr,
                                uint32_t value, unsigned size)
{
    MemoryRegionCache cache = { 0 };
    hwaddr bus_addr = addr;
    int status = SGI_FAIL_BUS;

    if ((addr & (size - 1)) || addr > UINT64_MAX - size) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: invalid publication "
                      "address 0x%" PRIx64 "\n", addr);
        return SGI_FAIL_BOUNDS;
    }
    if (bus->ops && bus->ops->dma_address &&
        !bus->ops->dma_address(bus->opaque, addr, &bus_addr)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: publication address "
                      "0x%" PRIx64 " cannot be composed\n", addr);
        return SGI_FAIL_BUS;
    }
    if ((bus_addr & (size - 1)) || bus_addr > UINT64_MAX - size) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: invalid bus address "
                      "0x%" HWADDR_PRIx "\n", bus_addr);
        return SGI_FAIL_BOUNDS;
    }

    WITH_RCU_READ_LOCK_GUARD() {
        int64_t len = address_space_cache_init(&cache, bus->as, bus_addr,
                                              size, true);
        Int128 offset = int128_sub(
            int128_make64(cache.xlat),
            int128_make64(cache.mrs.offset_within_region));
        Int128 end = int128_add(offset, int128_make64(size));

        /* A slow cache can report the full length without a direct pointer. */
        if (len == size && cache.ptr && !cache.mrs.readonly &&
            !((uintptr_t)cache.ptr & (size - 1)) &&
            int128_ge(offset, int128_zero()) &&
            int128_le(end, cache.mrs.size)) {
            if (size == 2) {
                qatomic_store_release((uint16_t *)cache.ptr,
                                      cpu_to_be16(value));
            } else {
                qatomic_store_release((uint32_t *)cache.ptr,
                                      cpu_to_be32(value));
            }
            address_space_cache_invalidate(&cache, 0, size);
            status = 0;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: publication at "
                          "0x%" HWADDR_PRIx " is not aligned writable RAM\n",
                          bus_addr);
        }
        address_space_cache_destroy(&cache);
    }
    return status;
}

int sgi_odyssey_publish_be16(const SGIDMABus *bus, uint64_t addr,
                             uint16_t value)
{
    return odyssey_publish_word(bus, addr, value, sizeof(value));
}

int sgi_odyssey_publish_be32(const SGIDMABus *bus, uint64_t addr,
                             uint32_t value)
{
    return odyssey_publish_word(bus, addr, value, sizeof(value));
}

static void odyssey_query_begin(SGIOdysseyState *s)
{
    s->query_stage = 1;
    s->query_base = ((uint64_t)(s->ctx_shadow_addr_upper & 0xffff) << 32) |
                    s->ctx_shadow_addr_lower;
    s->query_sync_base = ((uint64_t)(s->host_write_addr_upper & 0xffff) << 32) |
                         s->host_write_addr_lower;
    s->query_captured_epoch = s->query_epoch;
    s->query_captured_valid = s->query_current_valid;
    memcpy(s->query_captured, s->query_current, sizeof(s->query_captured));
}

static void odyssey_query_finish(SGIOdysseyState *s, uint32_t sequence)
{
    SGIDMABus bus = {
        .as = &address_space_memory,
        .ops = &odyssey_head_ops,
        .opaque = s,
    };
    unsigned i, j;
    uint64_t sync_offset = 2 * (s->query_sync_index & ODSY_DMA_FENCE_SLOT_MASK);

    s->query_stage = 0;
    for (i = 0; i < ARRAY_SIZE(odyssey_query_offset); i++) {
        if (!(s->query_captured_valid & (1u << i))) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-odyssey: unsupported current-query record %u "
                          "in snapshot\n", i);
        }
    }
    if (!s->query_base || s->query_base > UINT64_MAX - 0x980 ||
        s->query_captured_epoch != s->query_epoch ||
        s->query_captured_valid != 0x1f) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-odyssey: unsupported current-query snapshot\n");
        return;
    }
    for (i = 0; i < ARRAY_SIZE(odyssey_query_offset); i++) {
        unsigned words = 1 + odyssey_cmd_arity(s->query_captured[i][0]);

        for (j = 0; j < words; j++) {
            uint64_t addr = s->query_base + odyssey_query_offset[i] + 4 * j;

            if (sgi_odyssey_publish_be32(&bus, addr, s->query_captured[i][j])) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-odyssey: current-query payload failed\n");
                return;
            }
        }
    }
    if (sgi_odyssey_publish_be32(&bus, s->query_base + 0x97c, s->query_count)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-odyssey: current-query counter failed\n");
        return;
    }
    if (s->query_sync_base > UINT64_MAX - sync_offset ||
        sgi_odyssey_publish_be16(&bus, s->query_sync_base + sync_offset,
                             sequence & 0xffff)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-odyssey: current-query sync failed\n");
        return;
    }
    if (s->query_sync_index & ODSY_DMA_FENCE_INTERRUPT) {
        odyssey_deliver_softsync(s);
    }
}

/* Match the shared rasterizer's eight-bit channel conversion. */
static uint32_t odyssey_float_channel(uint32_t bits)
{
    double c = odyssey_f32(bits);

    if (!(c > 0.0)) {
        return 0;
    }
    if (c >= 1.0) {
        return 255;
    }
    return (uint32_t)(c * 255 + 0.5);
}

/*
 * The GL library's conversion of a signed-integer colour or normal operand
 * to a float, in single precision as its query decoder does
 * (docs/reviews/odyssey-query-formats-7-13-evidence-20260930.md). The exact
 * scales are 1/255, 1/65535 and 1/(2^32 - 2304); ODSY_F32_SCALE_32 is the
 * single-precision quotient of that last one, which is exactly
 * 1/(2^32 - 2560). Formats 22 and 23 form 2v + 1 in
 * 32-bit integer arithmetic before converting; 13 and 24 convert v first,
 * then form v * 2 + 1 with a fused multiply-add.
 *
 * This is the software's expectation for what the current value means; the
 * board's own arithmetic for these words is unknown. The model renders with
 * it so that a replayed 0xc580cc0d draws the colour a query reports.
 */
#define ODSY_F32_SCALE_8 0x3b808081
#define ODSY_F32_SCALE_16 0x37800080
#define ODSY_F32_SCALE_32 0x2f800005

static uint32_t odyssey_signed_to_float(uint32_t word, unsigned format)
{
    uint32_t scale_bits, bits;
    float f, scale;

    switch (format) {
    case 22:
        scale_bits = ODSY_F32_SCALE_8;
        f = (float)(int32_t)(2 * word + 1);
        break;
    case 23:
        scale_bits = ODSY_F32_SCALE_16;
        f = (float)(int32_t)(2 * word + 1);
        break;
    default:
        scale_bits = ODSY_F32_SCALE_32;
        f = fmaf((float)(int32_t)word, 2.0f, 1.0f);
        break;
    }
    memcpy(&scale, &scale_bits, sizeof(scale));
    f *= scale;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

/*
 * True when base lies within the board memory the active profile reports
 * (the "memory-size" property, in megabytes), false and reported, once per
 * reset, otherwise. A destination select's base is a board address in
 * 16-byte units, not bytes (established above, for the off-screen
 * board-memory objects), so the reported capacity is converted into that
 * same unit before the comparison. A profile's reported size is a
 * register-answered capacity, not the model's own storage
 * (ODSY_OBJ_POOL_BYTES): a guest that addresses past it is a guest bug
 * against the board it was told it has, kept observable rather than
 * silently accepted or crashed on.
 */
static bool odyssey_board_address_valid(SGIOdysseyState *s, uint32_t base)
{
    uint64_t limit = (uint64_t)s->memory_size * MiB / 16;

    if (base < limit) {
        return true;
    }
    if (!s->board_oob_reported) {
        s->board_oob_reported = true;
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: destination base 0x%x "
                      "is beyond the %u MiB this board reports\n",
                      base, s->memory_size);
    }
    trace_sgi_odyssey_board_oob(base, s->memory_size);
    return false;
}

/* The reviewed construction template; its (2x, 2y) taps are a model policy. */
static bool odyssey_texture_build_level(SGIOdysseyState *s)
{
    const uint32_t *p = s->cmd_operand;
    const uint32_t *entry = &s->tex_levels[60 * 3];
    uint32_t ws = s->tex_sampler[1], hs = s->tex_sampler[2];
    uint32_t wd, hd, source_base, source_units, dest_units;
    uint64_t source_end;
    int source, dest;

    if (!ws || !hs || ws > INT32_MAX || hs > INT32_MAX ||
        !is_power_of_2(ws) || !is_power_of_2(hs)) {
        return false;
    }
    wd = MAX(1, ws / 2);
    hd = MAX(1, hs / 2);
    source_units = ((ws + 15) / 16 + 1) & ~1u;
    dest_units = ((wd + 15) / 16 + 1) & ~1u;
    if (s->gl_primitive != ODSY_GL_QUADS ||
        !(s->tex_mode & ODSY_TEX_MODE_2D) || s->tex_level0 != 60 ||
        entry[1] != (0x41100000 | source_units) || entry[2] ||
        s->tex_sampler[0] != ((ctz32(ws) << 10) | (ctz32(hs) << 5)) ||
        s->tex_sampler[3] || s->tex_sampler[4] != 0x13640000 ||
        s->tex_sampler[5] || s->tex_sampler[6] ||
        p[0] || p[1] || p[2] || p[3] || p[4] || p[5] || p[6] ||
        p[7] != odyssey_f32_bits((float)wd) ||
        p[8] != odyssey_f32_bits((float)hd) || p[9] || p[10] || p[11] ||
        p[12] != odyssey_f32_bits(1.0f - 0.5f / wd) ||
        p[13] != odyssey_f32_bits(1.0f - 0.5f / hd)) {
        return false;
    }
    source_base = entry[0] & 0x1fffffffu;
    if (!source_base || source_base == s->dest_base ||
        s->obj_count > SGI_ODYSSEY_OBJECTS) {
        return false;
    }
    source = odyssey_object_find(s, source_base);
    dest = odyssey_image_object(s);
    if (source < 0 || dest < 0 ||
        s->obj_width[source] != source_units * 16 ||
        s->obj_rows[source] < hs ||
        s->obj_width[dest] != dest_units * 16 ||
        umul64_overflow(hs - 1, s->obj_width[source], &source_end) ||
        uadd64_overflow(source_end, s->obj_offset[source], &source_end) ||
        uadd64_overflow(source_end, ws, &source_end) ||
        source_end > s->obj_used || source_end > s->obj_mem.bytes_len / 4 ||
        !odyssey_object_rows(s, dest, hd)) {
        return false;
    }
    for (uint32_t y = 0; y < hd; y++) {
        for (uint32_t x = 0; x < wd; x++) {
            odyssey_object_store(s, dest, x, y,
                                 odyssey_object_load(s, source, 2 * x, 2 * y));
        }
    }
    return true;
}

/*
 * Dispatch a fully gathered register-select word or tagged GL opcode.
 * Flush and glyph pixel-mode setup/teardown registers, and any other
 * register-select word whose side effect this model does not need, are
 * accepted no-ops: this model's drawing state has nothing for them to change
 * (docs/datasheets/odyssey.md qtest checklist item 25).
 */
static void odyssey_cmd_dispatch(SGIOdysseyState *s, uint32_t tag)
{
    if (odyssey_is_program_word(tag)) {
        odyssey_gl_program_word(s, tag);
        return;
    }
    if ((tag >= ODSY_REG_TEX_COLOUR_TABLE0 &&
         tag < ODSY_REG_TEX_COLOUR_TABLE0 + ODSY_TEX_COLOUR_TABLE_BLOCKS) ||
        (tag >= ODSY_REG_TEX_COLOUR_TABLE1 &&
         tag < ODSY_REG_TEX_COLOUR_TABLE1 + ODSY_TEX_COLOUR_TABLE_BLOCKS)) {
        unsigned plane = tag >= ODSY_REG_TEX_COLOUR_TABLE1;
        unsigned first_tag = plane ? ODSY_REG_TEX_COLOUR_TABLE1 :
                                     ODSY_REG_TEX_COLOUR_TABLE0;
        unsigned block = tag - first_tag;

        if (s->cmd_operand_count != ODSY_TEX_COLOUR_TABLE_BLOCK_WORDS) {
            odyssey_cmd_unimplemented(s, tag);
            return;
        }
        memcpy(&s->tex_colour_table[plane]
                                    [block * ODSY_TEX_COLOUR_TABLE_BLOCK_WORDS],
               s->cmd_operand,
               ODSY_TEX_COLOUR_TABLE_BLOCK_WORDS * sizeof(uint32_t));
        s->tex_colour_table_valid[plane] |= 1u << block;
        return;
    }
    if (tag >= ODSY_REG_TEX_LEVEL &&
        tag < ODSY_REG_TEX_LEVEL + SGI_ODYSSEY_TEX_LEVELS) {
        memcpy(&s->tex_levels[(tag - ODSY_REG_TEX_LEVEL) * 3], s->cmd_operand,
               3 * sizeof(uint32_t));
        return;
    }
    if (tag == ODSY_GL_TEXCOORD_3F) {
        memcpy(s->tex_coord, s->cmd_operand, sizeof(s->tex_coord));
        s->tex_coord_r = s->cmd_operand[2];
        return;
    }
    if ((tag & ODSY_GL_TEXCOORD_SHAPE) == ODSY_GL_TEXCOORD_TAG &&
        tag != ODSY_GL_TEXCOORD_2F) {
        /* The 3f, 4f and integer forms: consumed, not kept. */
        odyssey_cmd_unimplemented(s, tag);
        return;
    }
    if ((tag & ODSY_GL_MATRIX_SELECT_SHAPE) == ODSY_GL_MATRIX_SELECT_TAG) {
        /* Pick the shared matrix slot the following load targets. */
        switch (tag & 0xff) {
        case ODSY_GL_MATRIX_SELECT_COMBINED:
            s->gl_matrix_slot = ODSY_GL_SLOT_COMBINED;
            break;
        case ODSY_GL_MATRIX_SELECT_TEXTURE:
            s->gl_matrix_slot = ODSY_GL_SLOT_TEXTURE;
            break;
        case ODSY_GL_MATRIX_SELECT_MODELVIEW:
            s->gl_matrix_slot = SGI_GL_MODELVIEW;
            break;
        case ODSY_GL_MATRIX_SELECT_BLOCK16:
            s->gl_matrix_slot = ODSY_GL_SLOT_BLOCK16;
            break;
        case ODSY_GL_MATRIX_SELECT_NORMAL:
            s->gl_matrix_slot = ODSY_GL_SLOT_NORMAL_MATRIX;
            break;
        default:
            /* Any other block: no home. */
            s->gl_matrix_slot = ODSY_GL_SLOT_NORMAL;
            break;
        }
        return;
    }
    switch (tag) {
    case ODSY_GL_COLOR_3UB:
        s->gl_color = odyssey_pack_pixel(s->cmd_operand[0], s->cmd_operand[1],
                                         s->cmd_operand[2]);
        s->gl_color_is_index = false;
        if (!odyssey_gl_index_mode(s)) {
            uint32_t colour[SGI_GL_GET_WORDS];
            unsigned n;
            bool queried;

            odyssey_gl_feed(s, SGI_GL_OP_COLOR3UB, s->cmd_operand, 3);
            queried = sgi_gl_get(&s->gl_ctx, SGI_GL_CURRENT_COLOR, colour, &n);
            assert(queried && n == ARRAY_SIZE(s->gl_colour));
            memcpy(s->gl_colour, colour, sizeof(s->gl_colour));
            s->gl_colour_overridden = false;
        }
        break;
    case ODSY_GL_COLOR_INDEX:
    case ODSY_GL_REPLAY_INDEX:
        /* The user's index word, or the kernel's replay of a saved one. */
        s->gl_color_index = s->cmd_operand[0];
        s->gl_color_is_index = true;
        break;
    case ODSY_GL_COLOR_INDEX_F:
    case ODSY_GL_REPLAY_INDEX_F:
        s->gl_color_index = odyssey_float_index(s->cmd_operand[0]);
        s->gl_color_is_index = true;
        break;
    case ODSY_REG_DEST_SELECT:
        /*
         * Destination-buffer select: its third operand marks an id-plane
         * write when it is the ODSY_DEST_ID_PLANES constant. A colour
         * select's non-zero first operand names the colour buffer, the base
         * a DMA header's destination word is matched against (the all-zero
         * blocks are GL context setup, not a buffer).
         */
        s->dest_buffer_word = s->cmd_operand[ODSY_DEST_SELECT_VALUE_INDEX];
        s->dest_base = s->cmd_operand[ODSY_DEST_SELECT_BASE_INDEX];
        if (!s->dest_base || s->dest_buffer_word == ODSY_DEST_ID_PLANES) {
            break;
        }
        if (!odyssey_board_address_valid(s, s->dest_base)) {
            break;
        }
        if ((s->dest_buffer_word >> ODSY_DEST_FORMAT_SHIFT) ==
            ODSY_DEST_FORMAT_SCREEN) {
            s->dma_colour_base = s->dest_base;
        } else {
            /* An off-screen object: create it, or update its row length. */
            odyssey_object_select(s, s->dest_base,
                                  s->cmd_operand[ODSY_DEST_WIDTH_INDEX] *
                                  ODSY_DEST_WIDTH_UNIT);
        }
        break;
    case ODSY_REG_PLANE_MASK:
        /* Plane write mask (mask << 12), consumed by an id-plane write. */
        s->plane_write_mask = s->cmd_operand[0];
        break;
    case ODSY_MODE_INDEX_ZEROS:
    case ODSY_GL_INDEX_MODE_COLOR_3I:
    case ODSY_GL_INDEX_MODE_COLOR_3F:
    case ODSY_GL_INDEX_MODE_COLOR_4F:
        /* An RGBA colour sent in index mode: GL does not draw with it. */
        break;
    case ODSY_GL_VERTEX_2I:
        if (s->gl_begin_open) {
            s->gl_integer_prim = true;
        }
        if (s->gl_begin_open && !s->image_active &&
            (s->tex_mode & (ODSY_TEX_MODE_1D | ODSY_TEX_MODE_2D |
                            ODSY_TEX_MODE_3D))) {
            if (!s->gl_in_prim) {
                uint32_t mode = s->gl_primitive;

                odyssey_gl_feed(s, SGI_GL_OP_BEGIN, &mode, 1);
                s->gl_in_prim = true;
            }
            odyssey_gl_vertex_colour(s, false);
            odyssey_texture_vertex(s);
            odyssey_gl_feed(s, SGI_GL_OP_VERTEX2I, s->cmd_operand, 2);
        } else {
            /* The X path uses window-relative integer coordinates. */
            odyssey_gl_vertex(s, s->cmd_operand[0] + s->win_origin[0],
                              s->cmd_operand[1] + s->win_origin[1]);
        }
        break;
    case ODSY_GL_COLOR_3F:
    case ODSY_GL_COLOR_3I:
    case ODSY_GL_COLOR_4F: {
        /*
         * The current colour of the true-colour GL stream (glColor3f,
         * glColor3i, glColor4f), or the kernel's replay of one
         * odyssey_ctx_save() saved. Kept so an unlit vertex after lit ones
         * takes it back.
         */
        uint32_t rgba[4];
        unsigned i;

        memcpy(rgba, s->cmd_operand, sizeof(rgba));
        if (tag == ODSY_GL_COLOR_3I) {
            for (i = 0; i < 3; i++) {
                rgba[i] = odyssey_signed_to_float(s->cmd_operand[i], 13);
            }
        }
        if (tag != ODSY_GL_COLOR_4F) {
            rgba[3] = ODSY_F32_ONE;
        }
        memcpy(s->gl_colour, rgba, sizeof(s->gl_colour));
        s->gl_colour_overridden = false;
        if (tag == ODSY_GL_COLOR_4F) {
            odyssey_gl_feed(s, SGI_GL_OP_COLOR4F, rgba, 4);
        } else {
            odyssey_gl_feed(s, SGI_GL_OP_COLOR3F, rgba, 3);
        }
        if (!odyssey_gl_index_mode(s)) {
            s->gl_color = odyssey_pack_pixel(odyssey_float_channel(rgba[0]),
                                             odyssey_float_channel(rgba[1]),
                                             odyssey_float_channel(rgba[2]));
            s->gl_color_is_index = false;
        }
        break;
    }
    case ODSY_GL_NORMAL_3B:
    case ODSY_GL_NORMAL_3S:
    case ODSY_GL_NORMAL_3I: {
        unsigned i;

        for (i = 0; i < 3; i++) {
            s->gl_normal[i] = odyssey_signed_to_float(s->cmd_operand[i],
                                                      tag & 0x3f);
        }
        break;
    }
    case ODSY_GL_NORMAL_3F:
        memcpy(s->gl_normal, s->cmd_operand, sizeof(s->gl_normal));
        break;
    case ODSY_GL_MATERIAL_AMBIENT:
    case ODSY_GL_MATERIAL_DIFFUSE:
    case ODSY_GL_MATERIAL_SPECULAR:
    case ODSY_GL_MATERIAL_EMISSION:
        /* The low byte counts 0x2f, 0x30, 0x31, 0x32 through the four. */
        memcpy(&s->gl_material[((tag & 0xff) - (ODSY_GL_MATERIAL_AMBIENT &
                                                0xff)) * 4],
               s->cmd_operand, 4 * sizeof(uint32_t));
        break;
    case ODSY_GL_SHININESS_SQRT:
        s->gl_shininess_sqrt = s->cmd_operand[0];
        break;
    case ODSY_GL_BACK_AMBIENT:
    case ODSY_GL_BACK_DIFFUSE:
    case ODSY_GL_BACK_SPECULAR:
    case ODSY_GL_BACK_EMISSION:
        memcpy(&s->gl_back_material[((tag & 0xff) - (ODSY_GL_BACK_AMBIENT &
                                                     0xff)) * 4],
               s->cmd_operand, 4 * sizeof(uint32_t));
        break;
    case ODSY_GL_BACK_SHININESS_SQRT:
        s->gl_back_shininess_sqrt = s->cmd_operand[0];
        break;
    case ODSY_GL_SHININESS_DERIVED:
    case ODSY_PROG_RUN_ONCE:
        /*
         * Consumed, not kept: the derived shininess floats and the one-shot
         * run have no modelled effect.
         */
        break;
    case ODSY_GL_CLIP_PLANE_LOAD:
        /*
         * See ODSY_GL_CLIP_PLANE_BASE's comment: accept and pass through
         * loads into the shared layer's six slots, silently drop loads past
         * that budget (expected library traffic, not an error), and report
         * only a selector this model cannot place at all.
         */
        if (s->gl_light_vec_sel >= ODSY_GL_CLIP_PLANE_BASE &&
            (s->gl_light_vec_sel - ODSY_GL_CLIP_PLANE_BASE) %
            ODSY_GL_CLIP_PLANE_STRIDE == 0) {
            unsigned plane = (s->gl_light_vec_sel - ODSY_GL_CLIP_PLANE_BASE) /
                             ODSY_GL_CLIP_PLANE_STRIDE;

            if (plane < SGI_GL_CLIP_PLANES) {
                uint32_t args[5];

                args[0] = plane;
                memcpy(&args[1], s->cmd_operand, 4 * sizeof(uint32_t));
                odyssey_gl_feed(s, SGI_GL_OP_CLIP_PLANE, args, 5);
            }
        } else if (!s->gl_block_logged) {
            s->gl_block_logged = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: class 0x31 clip-plane "
                          "load at index 0x%x is not modeled\n",
                          s->gl_light_vec_sel);
        }
        break;
    case ODSY_GL_LIGHT_COLOUR_LOAD:
        if (s->gl_light_colour_sel < SGI_ODYSSEY_GL_LIGHT_COLOURS) {
            memcpy(&s->gl_light_colour[s->gl_light_colour_sel * 4],
                   s->cmd_operand, 4 * sizeof(uint32_t));
        } else if (!s->gl_block_logged) {
            s->gl_block_logged = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: class 0x32 load at index "
                          "0x%x is not modeled\n", s->gl_light_colour_sel);
        }
        break;
    case ODSY_GL_LIGHT_VEC_LOAD:
        /*
         * Index 16 i is light i. Context setup also loads blocks at 0x80
         * to 0x1e0 whose meaning is unknown; they are dropped, reported
         * once (not per index, which would exhaust the word budget).
         */
        if (s->gl_light_vec_sel % ODSY_GL_LIGHT_VEC_FLOATS == 0 &&
            s->gl_light_vec_sel / ODSY_GL_LIGHT_VEC_FLOATS <
            SGI_ODYSSEY_GL_LIGHTS) {
            memcpy(&s->gl_light_vec[s->gl_light_vec_sel], s->cmd_operand,
                   ODSY_GL_LIGHT_VEC_FLOATS * sizeof(uint32_t));
        } else if (!s->gl_block_logged) {
            s->gl_block_logged = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: class 0x31 load at index "
                          "0x%x is not modeled\n", s->gl_light_vec_sel);
        }
        break;
    case ODSY_GL_VERTEX_2F:
    case ODSY_GL_VERTEX_3F:
        /*
         * A float vertex uses the shared GL path. Open the primitive lazily
         * on its first vertex, as for a textured integer primitive above.
         */
        if (!s->gl_in_prim && !s->gl_restart_absorb) {
            uint32_t mode = s->gl_primitive;

            odyssey_gl_feed(s, SGI_GL_OP_BEGIN, &mode, 1);
            s->gl_in_prim = true;
        }
        if (s->gl_begin_open && !s->gl_integer_prim &&
            s->gl_primitive <= 8) {
            odyssey_open_record_vertex(s, tag);
        }
        odyssey_gl_vertex_colour(s, tag == ODSY_GL_VERTEX_3F);
        odyssey_texture_vertex(s);
        if (s->gl_restart_absorb) {
            unsigned slot = s->gl_restart_total - s->gl_restart_absorb;

            if (slot < ARRAY_SIZE(s->gl_restart_vertex)) {
                sgi_gl_capture_vertex(&s->gl_ctx, s->gl_restart_type,
                                      s->cmd_operand,
                                      tag == ODSY_GL_VERTEX_3F ? 3 : 2,
                                      s->gl_restart_vertex[slot]);
            }
            s->gl_restart_absorb--;
            odyssey_restart_finish(s);
            break;
        }
        if (tag == ODSY_GL_VERTEX_3F) {
            odyssey_gl_feed(s, SGI_GL_OP_VERTEX3F, s->cmd_operand, 3);
        } else {
            odyssey_gl_feed(s, SGI_GL_OP_VERTEX2F, s->cmd_operand, 2);
        }
        break;
    case ODSY_GL_LOAD_4F:
        odyssey_gl_load_block16(s);
        break;
    case ODSY_REG_DEPTH_FUNC:
        if (s->cmd_operand[0] & ~ODSY_DEPTH_FUNC_MASK) {
            /* Only bits 2:0 are known; every capture leaves the rest 0. */
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: depth function word 0x%x "
                          "has bits beyond 2:0, not modeled\n",
                          s->cmd_operand[0]);
        }
        s->gl_depth_func = s->cmd_operand[0];
        odyssey_gl_apply_depth(s);
        break;
    case ODSY_REG_WRITE_MASK: {
        uint32_t stencil_mask = (s->cmd_operand[0] >>
                                 ODSY_WRITE_STENCIL_SHIFT) &
                                ODSY_WRITE_STENCIL_MASK;

        s->gl_write_mask = s->cmd_operand[0];
        odyssey_gl_apply_depth(s);
        /*
         * Forced 0 without stencil planes, which this model has none of; fed
         * regardless, since it is still the GL state the library decoded.
         */
        odyssey_gl_feed(s, SGI_GL_OP_STENCIL_MASK, &stencil_mask, 1);
        break;
    }
    case ODSY_REG_ALPHA_FUNC: {
        uint32_t word = s->cmd_operand[0];
        uint32_t args[2];

        args[0] = SGI_GL_NEVER_ENUM + ((word >> ODSY_ALPHA_FUNC_SHIFT) &
                                       ODSY_ALPHA_FUNC_MASK);
        args[1] = odyssey_f32_bits((word & ODSY_ALPHA_REF_MASK) /
                                   ODSY_ALPHA_REF_SCALE);
        odyssey_gl_feed(s, SGI_GL_OP_ALPHA_FUNC, args, 2);
        break;
    }
    case ODSY_REG_STENCIL_FUNC: {
        uint32_t word = s->cmd_operand[0];
        uint32_t args[3];
        uint32_t fail, zfail, zpass;

        args[0] = SGI_GL_NEVER_ENUM + ((word >> ODSY_STENCIL_FUNC_SHIFT) &
                                       ODSY_STENCIL_FUNC_MASK);
        args[1] = word & ODSY_STENCIL_REF_MASK;
        args[2] = (word >> ODSY_STENCIL_VALUE_MASK_SHIFT) &
                  ODSY_STENCIL_VALUE_MASK_MASK;
        odyssey_gl_feed(s, SGI_GL_OP_STENCIL_FUNC, args, 3);

        fail = odyssey_stencil_op((word >> ODSY_STENCIL_OP_FAIL_SHIFT) &
                                  ODSY_STENCIL_OP_MASK);
        zfail = odyssey_stencil_op((word >> ODSY_STENCIL_OP_ZFAIL_SHIFT) &
                                   ODSY_STENCIL_OP_MASK);
        zpass = odyssey_stencil_op((word >> ODSY_STENCIL_OP_ZPASS_SHIFT) &
                                   ODSY_STENCIL_OP_MASK);
        if (fail == UINT32_MAX || zfail == UINT32_MAX || zpass == UINT32_MAX) {
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: stencil op word 0x%x is "
                          "not modeled\n", word);
        } else {
            args[0] = fail;
            args[1] = zfail;
            args[2] = zpass;
            odyssey_gl_feed(s, SGI_GL_OP_STENCIL_OP, args, 3);
        }
        break;
    }
    case ODSY_REG_BLEND: {
        uint32_t word;
        uint32_t eq, src, dst;
        uint32_t args[4];

        if (!(s->pattern_mode & ODSY_MODE_BLEND)) {
            /* The library parks this register at zero while blending is off. */
            break;
        }
        word = s->cmd_operand[0];
        eq = (word >> ODSY_BLEND_EQ_SHIFT) & ODSY_BLEND_EQ_MASK;
        src = odyssey_blend_factor((word >> ODSY_BLEND_SRC_SHIFT) &
                                   ODSY_BLEND_SRC_MASK, false);
        dst = odyssey_blend_factor(word & ODSY_BLEND_DST_MASK, true);
        if (src == UINT32_MAX || dst == UINT32_MAX) {
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: blend factor word 0x%x is "
                          "not modeled\n", word);
        } else {
            args[0] = src;
            args[1] = dst;
            odyssey_gl_feed(s, SGI_GL_OP_BLEND_FUNC, args, 2);
        }
        if (eq == 3 || eq == 4) {
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: blend equation %u (the "
                          "SGIX alpha min/max extension) is not modeled\n",
                          eq);
        } else {
            args[0] = odyssey_blend_equation[eq];
            odyssey_gl_feed(s, SGI_GL_OP_BLEND_EQUATION, args, 1);
        }
        args[0] = odyssey_f32_bits(((s->cmd_operand[1] >>
                                    ODSY_BLEND_COLOUR_SHIFT) &
                                    ODSY_BLEND_COLOUR_MASK) /
                                   ODSY_BLEND_COLOUR_SCALE);
        args[1] = odyssey_f32_bits((s->cmd_operand[1] &
                                    ODSY_BLEND_COLOUR_MASK) /
                                   ODSY_BLEND_COLOUR_SCALE);
        args[2] = odyssey_f32_bits(((s->cmd_operand[2] >>
                                    ODSY_BLEND_COLOUR_SHIFT) &
                                    ODSY_BLEND_COLOUR_MASK) /
                                   ODSY_BLEND_COLOUR_SCALE);
        args[3] = odyssey_f32_bits((s->cmd_operand[2] &
                                    ODSY_BLEND_COLOUR_MASK) /
                                   ODSY_BLEND_COLOUR_SCALE);
        odyssey_gl_feed(s, SGI_GL_OP_BLEND_COLOR, args, 4);
        break;
    }
    case ODSY_REG_LOGIC_OP: {
        uint32_t mode = SGI_GL_CLEAR + (s->cmd_operand[0] & ODSY_LOGIC_OP_MASK);

        odyssey_gl_feed(s, SGI_GL_OP_LOGIC_OP, &mode, 1);
        break;
    }
    case ODSY_REG_LINE_POINT_SIZE: {
        uint32_t args[2];

        args[0] = odyssey_f32_bits((s->cmd_operand[0] &
                                    ODSY_LINE_POINT_SIZE_MASK) /
                                   ODSY_LINE_POINT_SIZE_SCALE);
        odyssey_gl_feed(s, SGI_GL_OP_LINE_WIDTH, &args[0], 1);
        args[1] = odyssey_f32_bits((s->cmd_operand[1] &
                                    ODSY_LINE_POINT_SIZE_MASK) /
                                   ODSY_LINE_POINT_SIZE_SCALE);
        odyssey_gl_feed(s, SGI_GL_OP_POINT_SIZE, &args[1], 1);
        break;
    }
    case ODSY_REG_POLYGON_MODE: {
        uint32_t front[2], back[2];

        front[0] = SGI_GL_FRONT;
        front[1] = odyssey_polygon_mode((s->cmd_operand[0] >>
                                         ODSY_POLYGON_MODE_FRONT_SHIFT) &
                                        ODSY_POLYGON_MODE_MASK);
        odyssey_gl_feed(s, SGI_GL_OP_POLYGON_MODE, front, 2);
        back[0] = SGI_GL_BACK;
        back[1] = odyssey_polygon_mode((s->cmd_operand[0] >>
                                        ODSY_POLYGON_MODE_BACK_SHIFT) &
                                       ODSY_POLYGON_MODE_MASK);
        odyssey_gl_feed(s, SGI_GL_OP_POLYGON_MODE, back, 2);
        break;
    }
    case ODSY_REG_POLYGON_OFFSET: {
        uint32_t args[2];

        args[0] = odyssey_f32_bits(0.0f);
        args[1] = odyssey_f32_bits(sextract32(s->cmd_operand[0], 0,
                                              ODSY_POLYGON_OFFSET_BITS) *
                                   ODSY_POLYGON_OFFSET_SCALE);
        odyssey_gl_feed(s, SGI_GL_OP_POLYGON_OFFSET_EXT, args, 2);
        break;
    }
    case ODSY_REG_CLIP_BOX:
        s->gl_clip_box[0] = s->cmd_operand[0];
        s->gl_clip_box[1] = s->cmd_operand[1];
        odyssey_gl_apply_clip_box(s);
        break;
    case ODSY_REG_FOG:
        memcpy(s->gl_fog, s->cmd_operand, sizeof(s->gl_fog));
        break;
    case ODSY_REG_TEX_MODE:
        s->tex_mode = s->cmd_operand[0];
        odyssey_gl_apply_clip_planes(s);
        break;
    case ODSY_REG_TEX_SAMPLER:
        memcpy(s->tex_sampler, s->cmd_operand, sizeof(s->tex_sampler));
        break;
    case ODSY_REG_TEX_ENV:
        memcpy(s->tex_env, s->cmd_operand, sizeof(s->tex_env));
        break;
    case ODSY_REG_TEX_LEVEL0:
        s->tex_level0 = s->cmd_operand[0];
        break;
    case ODSY_REG_TEX_LOD:
        memcpy(s->tex_lod, s->cmd_operand, sizeof(s->tex_lod));
        break;
    case ODSY_GL_TEXCOORD_1F:
        s->tex_coord[0] = s->cmd_operand[0];
        s->tex_coord[1] = 0;
        s->tex_coord_r = 0;
        break;
    case ODSY_GL_TEXCOORD_2F:
        memcpy(s->tex_coord, s->cmd_operand, sizeof(s->tex_coord));
        s->tex_coord_r = 0;
        break;
    case ODSY_GL_TEX_RECT:
        if (!odyssey_texture_build_level(s) && !s->tex_rect_reported) {
            s->tex_rect_reported = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: textured rectangle into "
                          "buffer 0x%x has unsupported construction state "
                          "or missing backing\n", s->dest_base);
        }
        break;
    case ODSY_REG_GL_BEGIN:
        /*
         * Record the primitive and, for an image, open the raster path. The
         * shared GL path opens on a float or texture-enabled integer vertex;
         * untextured integer primitives stay on the 2D interpreter.
         */
        s->gl_primitive = s->cmd_operand[0];
        s->gl_vertex_count = 0;
        s->gl_begin_open = true;
        s->gl_integer_prim = false;
        s->gl_open_r = s->gl_open_c = 0;
        s->gl_open_m3 = s->gl_open_m4 = s->gl_open_held = 0;
        /* A polygon left open by a BEGIN without END encloses nothing. */
        odyssey_gl_polygon_close(s, false);
        s->image_active = s->cmd_operand[0] == ODSY_GL_PRIMITIVE_IMAGE;
        if (s->image_active) {
            s->image_x1 = s->image_y1 = s->image_x2 = s->image_y2 = 0;
            s->image_cur_x = s->image_cur_y = 0;
            s->image_fmt = 0;
            s->image_group_len = 0;
            s->image_group_painted = 0;
            s->image_bytes = 0;
            s->image_nbytes = 0;
        }
        break;
    case ODSY_REG_GL_END:
        /*
         * A GL geometry primitive closes on the shared context; the greeter's
         * QUADS fill (and odyssey_putchar()'s stray END) close on the 2D path.
         */
        s->gl_begin_open = false;
        s->gl_open_held = 0;
        if (s->gl_in_prim) {
            odyssey_gl_feed(s, SGI_GL_OP_END, NULL, 0);
            s->gl_in_prim = false;
        } else {
            odyssey_gl_end(s);
        }
        s->gl_integer_prim = false;
        break;
    case 0x0400:                 /* Pending primitive for context restore. */
        s->gl_restart_type = s->cmd_operand[0];
        break;
    case 0x0444:                 /* Drop an outgoing primitive, without draw. */
        if (s->cmd_operand[0] == 0 && !s->gl_integer_prim &&
            !(s->gl_begin_open && s->gl_primitive == 9)) {
            sgi_gl_discard_primitive(&s->gl_ctx);
            s->gl_in_prim = false;
            s->gl_begin_open = false;
            s->gl_open_held = 0;
            s->gl_restart_absorb = 0;
            s->gl_restart_armed = false;
        } else if (s->cmd_operand[0]) {
            odyssey_cmd_unimplemented(s, 0x10444);
        }
        break;
    case 0x0850: {
        uint32_t type = s->cmd_operand[0] & 0xf;

        if (type == 0xf) {
            if (!s->gl_integer_prim &&
                !(s->gl_begin_open && s->gl_primitive == 9)) {
                sgi_gl_discard_primitive(&s->gl_ctx);
                s->gl_in_prim = false;
                s->gl_begin_open = false;
                s->gl_open_held = 0;
                s->gl_restart_absorb = 0;
                s->gl_restart_armed = false;
            }
        } else if (type <= 8) {
            s->gl_primitive = type;
            s->gl_restart_type = type;
            s->gl_restart_absorb = (s->cmd_operand[0] >> 19) & 3;
            s->gl_restart_total = s->gl_restart_absorb;
            s->gl_restart_armed = true;
            s->gl_restart_select = 0;
            s->gl_begin_open = true;
            s->gl_integer_prim = false;
            s->gl_open_r = s->gl_open_c = 0;
            s->gl_open_m3 = s->gl_open_m4 = s->gl_open_held = 0;
        } else {
            odyssey_cmd_unimplemented(s, 0x10850);
        }
        break;
    }
    case 0x0740:
        s->gl_restart_select = s->cmd_operand[0];
        if (s->gl_restart_select != 0x1e0) {
            odyssey_cmd_unimplemented(s, 0x740);
        }
        break;
    case 0x0741:
        if (s->gl_restart_select == 0x1e0 && s->gl_restart_armed) {
            uint32_t word = s->cmd_operand[0];

            s->gl_restart_c = (word >> 21) & 7;
            s->gl_restart_m3 = (word >> 12) & 3;
            s->gl_restart_m4 = (word >> 14) & 3;
            odyssey_restart_finish(s);
        } else {
            odyssey_cmd_unimplemented(s, 0x741);
        }
        break;
    case ODSY_CMD_IMAGE_HEADER:
        /*
         * The transfer header opens a pixel group: its length (bits 23:20, 15
         * standing for 16, 0 meaning the group runs unbounded to the box edge)
         * bounds byte-format pixels or SHORT-format words. Reset the per-group
         * count and any partial pixel.
         */
    {
        uint32_t glen = (s->cmd_operand[0] >> ODSY_IMAGE_HEADER_GROUP_SHIFT) &
                        ODSY_IMAGE_HEADER_GROUP_MASK;

        s->image_group_len = glen == ODSY_IMAGE_HEADER_GROUP_MASK ?
                             ODSY_IMAGE_HEADER_GROUP_FULL : glen;
        s->image_group_painted = 0;
        s->image_code = s->cmd_operand[0] & ODSY_IMAGE_CODE_MASK;
        s->image_nbytes = 0;
        s->image_bytes = 0;
        break;
    }
    case ODSY_REG_PATTERN_SHAPE:
        s->pattern_shape = s->cmd_operand[0];
        break;
    case ODSY_REG_PATTERN_MODE:
        s->pattern_mode = s->cmd_operand[0];
        odyssey_gl_apply_mode(s);
        break;
    case ODSY_REG_WINDOW_ORIGIN:
        s->win_origin[0] = sextract32(s->cmd_operand[0], 0, ODSY_ORIGIN_BITS);
        s->win_origin[1] = sextract32(s->cmd_operand[1], 0, ODSY_ORIGIN_BITS);
        if (s->cmd_from_kernel) {
            memcpy(s->win_committed_origin, s->win_origin,
                   sizeof(s->win_origin));
        }
        break;
    case ODSY_REG_WINDOW_CLIP:
        memcpy(s->clip_words, s->cmd_operand, sizeof(s->clip_words));
        if (s->cmd_from_kernel) {
            memcpy(s->win_committed_clip, s->clip_words,
                   sizeof(s->clip_words));
        }
        odyssey_clip_update(s);
        break;
    case ODSY_CMD_WINDOW_RESTORE:
        /* The committed window back (ODSY_CMD_WINDOW_RESTORE; INFERRED). */
        memcpy(s->win_origin, s->win_committed_origin, sizeof(s->win_origin));
        memcpy(s->clip_words, s->win_committed_clip, sizeof(s->clip_words));
        s->did_compare = s->win_committed_did;
        odyssey_clip_update(s);
        break;
    case ODSY_REG_DID_COMPARE:
        /*
         * The first operand (0x142000 in every capture) is unknown and not
         * kept. The compare itself is implemented from the kernel's
         * construction, but no capture has enabled it yet: say so the first
         * time one does.
         */
        s->did_compare = s->cmd_operand[ODSY_DID_COMPARE_OPERAND];
        if (s->cmd_from_kernel) {
            s->win_committed_did = s->did_compare;
        }
        if ((s->did_compare & ODSY_DID_COMPARE_ENABLE) &&
            !s->did_compare_reported) {
            s->did_compare_reported = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: drawable-id compare 0x%x "
                          "enabled; modeled from the kernel's encoding, not "
                          "from a capture\n", s->did_compare);
        }
        break;
    case ODSY_REG_LINE_STIPPLE:
        memcpy(s->line_stipple, s->cmd_operand, sizeof(s->line_stipple));
        odyssey_gl_apply_line_stipple(s);
        if (s->line_stipple[ODSY_LINE_STIPPLE_CONTROL] !=
            ODSY_LINE_STIPPLE_CONTROL_32 &&
            (s->line_stipple[ODSY_LINE_STIPPLE_CONTROL] & ~0xffu) &&
            !s->line_stipple_reported) {
            s->line_stipple_reported = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: line stipple control 0x%x "
                          "has unclassified high bits\n",
                          s->line_stipple[ODSY_LINE_STIPPLE_CONTROL]);
        }
        break;
    case ODSY_REG_QUICK_FLUSH_ARG:
    case ODSY_REG_STROBE:
    case ODSY_REG_FULL_FLUSH_EXTRA:
    case ODSY_REG_PIXEL_MODE_EXT:
    case ODSY_REG_BLIT_PREFIX:
        break;
    case ODSY_SWAP_SYNC_A:
        /*
         * The bound drawing and reading drawable words, kept raw and
         * independently (ODSY_REG_DRAW_MASK): neither changes where the
         * model draws. The swap completes on the retrace hook
         * (odyssey_retrace()).
         */
        s->draw_word = s->cmd_operand[0];
        trace_sgi_odyssey_buffer_word(tag, s->draw_word);
        break;
    case ODSY_SWAP_SYNC_B:
        s->read_word = s->cmd_operand[0];
        trace_sgi_odyssey_buffer_word(tag, s->read_word);
        break;
    case ODSY_REG_READ_ORIGIN:
        s->read_origin[0] = sextract32(s->cmd_operand[0], 0, 16);
        s->read_origin[1] = sextract32(s->cmd_operand[0], 16, 16);
        break;
    case ODSY_REG_DRAW_MASK:
        /* The drawing mask: which colour buffers the next writes reach. */
        s->draw_mask = s->cmd_operand[0];
        trace_sgi_odyssey_draw_mask(s->draw_mask);
        if ((s->draw_mask & ~ODSY_DRAW_MASK_BITS) &&
            !s->draw_mask_high_reported) {
            s->draw_mask_high_reported = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: drawing mask 0x%x bits "
                          "above 3 are not modeled; ignored\n", s->draw_mask);
        }
        if ((s->draw_mask & ODSY_DRAW_MASK_RIGHT) &&
            !s->draw_mask_stereo_reported) {
            s->draw_mask_stereo_reported = true;
            qemu_log_mask(LOG_UNIMP, "sgi-odyssey: stereo is not modeled; "
                          "drawing mask 0x%x draws its right-eye buffers "
                          "into the left-eye ones\n", s->draw_mask);
        }
        break;
    case ODSY_REG_HOST_WRITE:
    {
        uint32_t op0 = s->cmd_operand[0];
        uint32_t op1 = s->cmd_operand[1];
        /*
         * The standalone host-write/sync command is the board's own DMA
         * completion signal (docs/analysis/odyssey-dma-submit-swap-path.md), so
         * it raises the same fence the DMA engine's completion does, through the
         * one fence implementation: op0's slot and interrupt bit and op1's token
         * become the completion cookie, published to odyssey_publish() ->
         * odyssey_fence(). op0's interrupt bit and op1's token already sit at the
         * fence word's own positions (ODSY_HOST_WRITE_INTERRUPT is the fence
         * word's bit 15).
         */
        uint32_t word = ((op1 & 0xffff) << ODSY_DMA_FENCE_TOKEN_SHIFT) |
                        (op0 & (ODSY_DMA_FENCE_INTERRUPT |
                                ODSY_DMA_FENCE_SLOT_MASK));

        sgi_head_publish(&odyssey_head_ops, s, 0, SGI_DONE_WRITTEN,
                         ODSY_FENCE_VALID | word, 0);
        break;
    }
    case ODSY_DMA_DESC_UPLOAD:
        odyssey_dma_descriptor(s, false);
        break;
    case ODSY_DMA_DESC_READBACK:
        odyssey_dma_descriptor(s, true);
        break;
    default:
        /*
         * A recognized register-select word (valid shape, field-derived
         * arity) whose side effect this model does not track -- a mode, mask
         * or state register. Consumed like the flush and pixel-mode registers
         * above rather than treated as impossible.
         */
        break;
    }
}

static void odyssey_cmd_start(SGIOdysseyState *s, uint32_t tag,
                              unsigned operands)
{
    s->cmd_tag = tag;
    s->cmd_operand_count = operands;
    s->cmd_operand_index = 0;
    if (operands == 0) {
        odyssey_query_update(s, tag);
        if (tag != 0x2f00c02e && tag != 0x3000c02e) {
            odyssey_cmd_dispatch(s, tag);
        }
    } else {
        s->cmd_state = ODSY_CMD_OPERANDS;
    }
}

/* Classify a word arriving while the parser is idle: a new command. */
static void odyssey_cmd_classify(SGIOdysseyState *s, uint32_t word)
{
    unsigned int i;

    if (word == 0x4c0) {
        odyssey_query_begin(s);
        return;
    }
    if (word == 0x2f00c02e || word == 0x3000c02e) {
        odyssey_cmd_start(s, word, 0);
        return;
    }
    if (word == 0x740 || word == 0x741) {
        odyssey_cmd_start(s, word, 1);
        return;
    }

    if (word == ODSY_CMD_COPY_RECT_TRIGGER) {
        s->cmd_state = ODSY_CMD_COPY_RECT;
        s->cmd_operand_index = 0;
        return;
    }
    /*
     * A page-table DMA descriptor: the opcode word, then three more words the
     * generic operand path gathers before odyssey_cmd_dispatch() runs the
     * transfer (the two-doubleword descriptor reaches here as four words).
     */
    if (word == ODSY_DMA_DESC_UPLOAD || word == ODSY_DMA_DESC_READBACK) {
        odyssey_cmd_start(s, word, ODSY_DMA_DESC_OPERANDS);
        return;
    }
    /*
     * The two buffer-select doublewords: each opcode word takes one operand
     * word. Consumed cleanly so the parser stays word-aligned; the swap
     * completion is delivered from the retrace hook, not here (see the
     * ODSY_SWAP_SYNC_* constants and odyssey_retrace()).
     */
    if (word == ODSY_SWAP_SYNC_A || word == ODSY_SWAP_SYNC_B) {
        odyssey_cmd_start(s, word, ODSY_SWAP_SYNC_OPERANDS);
        return;
    }
    if (word == ODSY_REG_READ_ORIGIN) {
        odyssey_cmd_start(s, word, 1);
        return;
    }
    if (word == ODSY_GL_COLOR_3UB) {
        odyssey_cmd_start(s, word, 3);
        return;
    }
    if (word == ODSY_GL_VERTEX_2I) {
        odyssey_cmd_start(s, word, 2);
        return;
    }
    if (word == ODSY_GL_TEXCOORD_1F ||
        word == ODSY_GL_TEXCOORD_3F ||
        (word & ODSY_GL_TEXCOORD_SHAPE) == ODSY_GL_TEXCOORD_TAG ||
        word == ODSY_GL_TEX_RECT) {
        odyssey_cmd_start(s, word, odyssey_cmd_arity(word));
        return;
    }
    if (word == ODSY_CMD_WINDOW_RESTORE) {
        odyssey_cmd_start(s, word, 0);
        return;
    }
    if (word == ODSY_GL_COLOR_INDEX || word == ODSY_GL_REPLAY_INDEX ||
        word == ODSY_GL_COLOR_INDEX_F || word == ODSY_GL_REPLAY_INDEX_F ||
        word == ODSY_GL_INDEX_MODE_COLOR_3I ||
        word == ODSY_GL_INDEX_MODE_COLOR_3F ||
        word == ODSY_GL_INDEX_MODE_COLOR_4F ||
        word == ODSY_GL_COLOR_3I || word == ODSY_GL_COLOR_4F ||
        word == ODSY_GL_NORMAL_3B || word == ODSY_GL_NORMAL_3S ||
        word == ODSY_GL_NORMAL_3I) {
        odyssey_cmd_start(s, word, odyssey_cmd_arity(word));
        return;
    }
    /*
     * The true-colour GL words, matched by value: the float colour and vertex
     * siblings, the three matrix target-selects (0 operands each) and the
     * matrix load (16 floats, more than cmd_operand[] holds, so it streams into
     * gl_matrix[] through its own state).
     */
    if (word == ODSY_GL_COLOR_3F) {
        odyssey_cmd_start(s, word, 3);
        return;
    }
    if (word == ODSY_GL_VERTEX_2F) {
        odyssey_cmd_start(s, word, 2);
        return;
    }
    if (word == ODSY_GL_VERTEX_3F) {
        odyssey_cmd_start(s, word, 3);
        return;
    }
    if (word == ODSY_GL_LOAD_4F) {
        odyssey_cmd_start(s, word, ODSY_GL_BLOCK16_FLOATS);
        return;
    }
    /*
     * The lighting words, matched by value; each carries its operand count
     * in bits 13:10 like every other word. The program words carry none and
     * are matched by class ahead of the image path's 0x29 state word, which
     * is the same class (odyssey_is_program_word()).
     */
    for (i = 0; i < ARRAY_SIZE(odyssey_gl_light_words); i++) {
        if (word == odyssey_gl_light_words[i]) {
            odyssey_cmd_start(s, word, odyssey_cmd_arity(word));
            return;
        }
    }
    if (odyssey_is_program_word(word)) {
        odyssey_cmd_start(s, word, 0);
        return;
    }
    if ((word & ODSY_GL_MATRIX_SELECT_SHAPE) == ODSY_GL_MATRIX_SELECT_TAG) {
        odyssey_cmd_start(s, word, 0);
        return;
    }
    if (word == ODSY_GL_MATRIX_LOAD) {
        /* 16 floats, more than cmd_operand[] holds, so gathered in gl_matrix[]. */
        s->cmd_state = ODSY_CMD_MATRIX_LOAD;
        s->cmd_operand_count = ODSY_GL_MATRIX_FLOATS;
        s->cmd_operand_index = 0;
        return;
    }
    if (word == ODSY_GL_RECT) {
        /* 10 floats, more than cmd_operand[] holds: gathered in gl_rect[]. */
        s->cmd_state = ODSY_CMD_GL_RECT;
        s->cmd_operand_count = ODSY_GL_RECT_FLOATS;
        s->cmd_operand_index = 0;
        return;
    }
    if (word == ODSY_MODE_INDEX_ZEROS) {
        odyssey_cmd_start(s, word, 3);
        return;
    }
    /*
     * The BEGIN-0xa image raster path's structural words, matched by value
     * rather than as register-selects. The raster-box floats need indices 4,
     * 5, 7 and 8 -- past what cmd_operand[] holds -- so they stream through a
     * dedicated state; the state word 0x29008000 | v and its teardown twin (0
     * operands), the transfer header (1) and its tail (0) run the generic
     * path, the header setting the group length as it dispatches.
     */
    if (word == ODSY_CMD_IMAGE_BOX) {
        s->cmd_state = ODSY_CMD_IMAGE_BOX_LOAD;
        s->cmd_operand_count = odyssey_cmd_arity(word);
        s->cmd_operand_index = 0;
        return;
    }
    if ((word & ODSY_CMD_STATE_WORD_MASK) == ODSY_CMD_STATE_WORD ||
        word == ODSY_CMD_IMAGE_HEADER || word == ODSY_CMD_IMAGE_HEADER_TAIL) {
        odyssey_cmd_start(s, word, odyssey_cmd_arity(word));
        return;
    }
    if ((word & ODSY_CMD_REGISTER_SHAPE) == ODSY_CMD_REGISTER_TAG) {
        uint32_t reg = word & ~ODSY_CMD_REGISTER_SHAPE;
        unsigned operands = odyssey_cmd_arity(reg);
        uint32_t fmt;

        /*
         * A pixel burst while an image is open: its data words paint into the
         * box rather than streaming through the generic operand path. Outside
         * an image the same register-select is an ordinary state word and
         * falls through to be consumed and dropped below.
         */
        if (s->image_active && operands > 0 &&
            odyssey_image_burst_fmt(reg, &fmt)) {
            s->image_fmt = fmt;
            if (!odyssey_image_pairing_supported(s->image_code, fmt)) {
                odyssey_cmd_unimplemented(s, word);
            }
            s->cmd_state = ODSY_CMD_IMAGE_BURST;
            s->cmd_operand_count = operands;
            s->cmd_operand_index = 0;
            return;
        }

        /*
         * The 32x32-bit pattern loads: a 16-word burst each, more than
         * cmd_operand[] holds, so they stream straight into the pattern buffer
         * (0x3c4e -> words 0-15, 0x3c4f -> words 16-31) rather than through the
         * generic operand path.
         */
        if (reg == ODSY_REG_PATTERN_LOW || reg == ODSY_REG_PATTERN_HIGH) {
            s->cmd_state = ODSY_CMD_PATTERN_LOAD;
            s->cmd_operand_count = operands;
            s->cmd_operand_index = 0;
            s->pattern_load_offset =
                reg == ODSY_REG_PATTERN_HIGH ? ODSY_PATTERN_WORDS : 0;
            return;
        }
        odyssey_cmd_start(s, reg, operands);
        return;
    }
    odyssey_cmd_unimplemented(s, word);
}

/* One command-FIFO word, wherever the parser currently is. */
static void odyssey_cmd_word(SGIOdysseyState *s, uint32_t word)
{
    switch (s->query_stage) {
    case 1:
        s->query_count = word;
        s->query_stage = 2;
        return;
    case 2:
        if (word == 0x10a5a) {
            s->query_stage = 3;
            return;
        }
        s->query_stage = 0;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-odyssey: unpaired current-query sequence\n");
        odyssey_cmd_classify(s, word);
        return;
    case 3:
        s->query_sync_index = word;
        s->query_stage = 4;
        return;
    case 4:
        odyssey_query_finish(s, word);
        return;
    }
    switch (s->cmd_state) {
    case ODSY_CMD_OPERANDS:
        /* cmd_operand[] holds a whole burst; the bound is belt and braces. */
        if (s->cmd_operand_index < ARRAY_SIZE(s->cmd_operand)) {
            s->cmd_operand[s->cmd_operand_index] = word;
        }
        if (++s->cmd_operand_index == s->cmd_operand_count) {
            odyssey_query_update(s, s->cmd_tag);
            odyssey_cmd_dispatch(s, s->cmd_tag);
            s->cmd_state = ODSY_CMD_IDLE;
        }
        break;
    case ODSY_CMD_COPY_RECT:
        s->cmd_operand[s->cmd_operand_index] = word;
        switch (s->cmd_operand_index) {
        case ODSY_CMD_COPY_RECT_SRC_INDEX:
            s->copy_src = word;
            break;
        case ODSY_CMD_COPY_RECT_SIZE_INDEX:
            s->copy_size = word;
            break;
        case ODSY_CMD_COPY_RECT_DST_INDEX:
            s->copy_dst = word;
            break;
        default:
            break;
        }
        if (++s->cmd_operand_index == ODSY_CMD_COPY_RECT_WORDS) {
            odyssey_copy_rect(s);
            s->cmd_state = ODSY_CMD_IDLE;
        }
        break;
    case ODSY_CMD_PATTERN_LOAD:
        /* A 16-word burst into the pattern buffer at the load's own offset. */
        if (s->cmd_operand_index < ODSY_PATTERN_WORDS) {
            s->pattern_buffer[s->pattern_load_offset + s->cmd_operand_index] =
                word;
        }
        if (++s->cmd_operand_index == s->cmd_operand_count) {
            s->cmd_state = ODSY_CMD_IDLE;
        }
        break;
    case ODSY_CMD_IMAGE_BOX_LOAD:
        /*
         * The ten raster-box floats. Only 4, 5, 7 and 8 carry the destination
         * corners; the rest are the zero the DDX's private was allocated with.
         * Set the cursor to the top-left corner once all ten have arrived.
         */
        switch (s->cmd_operand_index) {
        case ODSY_IMAGE_BOX_X1_INDEX:
            s->image_x1 = odyssey_float_to_u32(word);
            break;
        case ODSY_IMAGE_BOX_Y1_INDEX:
            s->image_y1 = odyssey_float_to_u32(word);
            break;
        case ODSY_IMAGE_BOX_X2_INDEX:
            s->image_x2 = odyssey_float_to_u32(word);
            break;
        case ODSY_IMAGE_BOX_Y2_INDEX:
            s->image_y2 = odyssey_float_to_u32(word);
            break;
        default:
            break;
        }
        if (++s->cmd_operand_index == s->cmd_operand_count) {
            s->image_cur_x = s->image_x2 < s->image_x1 ?
                             s->image_x1 - 1 : s->image_x1;
            s->image_cur_y = s->image_y2 < s->image_y1 ?
                             s->image_y1 - 1 : s->image_y1;
            s->cmd_state = ODSY_CMD_IDLE;
        }
        break;
    case ODSY_CMD_IMAGE_BURST:
        /* Each data word paints into the open image at the raster cursor. */
        odyssey_image_burst_word(s, word);
        if (++s->cmd_operand_index == s->cmd_operand_count) {
            s->cmd_state = ODSY_CMD_IDLE;
        }
        break;
    case ODSY_CMD_MATRIX_LOAD:
        /* A board-side matrix: 16 floats, loaded once all have arrived. */
        if (s->cmd_operand_index < ODSY_GL_MATRIX_FLOATS) {
            s->gl_matrix[s->cmd_operand_index] = word;
        }
        if (++s->cmd_operand_index == s->cmd_operand_count) {
            odyssey_gl_load_matrix(s);
            s->cmd_state = ODSY_CMD_IDLE;
        }
        break;
    case ODSY_CMD_GL_RECT:
        /* The rectangle's colour and corners, painted once all have arrived. */
        s->gl_rect[s->cmd_operand_index] = word;
        if (++s->cmd_operand_index == s->cmd_operand_count) {
            odyssey_gl_rect(s);
            s->cmd_state = ODSY_CMD_IDLE;
        }
        break;
    case ODSY_CMD_IDLE:
    default:
        odyssey_cmd_classify(s, word);
        break;
    }
}

/*
 * The command FIFO accepts both 32-bit and 64-bit stores
 * (docs/datasheets/odyssey.md qtest checklist item 21). An 8-byte store
 * carries two sequential 32-bit command words; which half is logically
 * first is not independently established by any allowed source, so it is
 * modeled by analogy to the backend-selector port's own
 * (selector << 32) | data convention and this device's big-endian byte
 * order, upper half first. Other widths are not established for this
 * port, so they are reported unimplemented rather than fed into the
 * command stream and risking desync.
 */
static void odyssey_cmd_fifo_write(SGIOdysseyState *s, uint64_t value,
                                   unsigned size)
{
    switch (size) {
    case 4:
        odyssey_cmd_word(s, (uint32_t)value);
        break;
    case 8:
        odyssey_cmd_word(s, value >> 32);
        odyssey_cmd_word(s, (uint32_t)value);
        break;
    default:
        odyssey_unimplemented(s, ODSY_CMD_FIFO, size, true);
        break;
    }
}

/*
 * The kernel driver's command-FIFO initialisation self-test preamble, in
 * order, all through the command FIFO: three 4-byte command words, five
 * 8-byte pushes (a 32-bit selector in the upper half, data in the lower),
 * and one closing 4-byte word. Seeing the whole sequence makes
 * ODSY_CFIFO_SELFTEST answer OK. (The real driver sends the five 8-byte
 * words to 0x110000, not to the backend-select port, confirmed by a boot
 * trace.)
 */
static const struct {
    hwaddr port;
    unsigned size;
    uint64_t value;
} odyssey_cfifo_preamble[] = {
    { ODSY_CMD_FIFO, 4, 0x00010854 },
    { ODSY_CMD_FIFO, 4, 0x00000000 },
    { ODSY_CMD_FIFO, 4, 0x0fffffff },
    { ODSY_CMD_FIFO, 8, 0x000006b000000000ULL },
    { ODSY_CMD_FIFO, 8, 0x000006b100000100ULL },
    { ODSY_CMD_FIFO, 8, 0x000006b300005e50ULL },
    { ODSY_CMD_FIFO, 8, 0x000006b200007dc0ULL },
    { ODSY_CMD_FIFO, 8, 0x000007b500000001ULL },
    { ODSY_CMD_FIFO, 4, 0x00010046 },
};

static void odyssey_selftest_track(SGIOdysseyState *s, hwaddr port,
                                   unsigned size, uint64_t value)
{
    unsigned next = s->selftest_progress;

    if (next < ARRAY_SIZE(odyssey_cfifo_preamble) &&
        odyssey_cfifo_preamble[next].port == port &&
        odyssey_cfifo_preamble[next].size == size &&
        odyssey_cfifo_preamble[next].value == value) {
        s->selftest_progress = next + 1;
        if (s->selftest_progress == ARRAY_SIZE(odyssey_cfifo_preamble)) {
            s->cfifo_selftest_ready = true;
            s->selftest_progress = 0;
        }
        return;
    }
    /* A break in the sequence restarts it, counting a fresh first word. */
    s->selftest_progress =
        (odyssey_cfifo_preamble[0].port == port &&
         odyssey_cfifo_preamble[0].size == size &&
         odyssey_cfifo_preamble[0].value == value) ? 1 : 0;
}

static void odyssey_write_dispatch(SGIOdysseyState *s, hwaddr addr,
                                   uint64_t value, unsigned size)
{
    trace_sgi_odyssey_mmio(addr, size, value, true);

    if (addr == ODSY_BACKEND_SELECT && (size == 8 || size == 4)) {
        odyssey_backend_write(s, value, size);
        return;
    }
    if (addr == ODSY_CTX_SWITCH_PORT && size == 8) {
        odyssey_ctx_switch(s, value);
        return;
    }
    if (addr == ODSY_CMD_FIFO) {
        odyssey_selftest_track(s, ODSY_CMD_FIFO, size, value);
        s->cmd_from_kernel = true;
        odyssey_cmd_fifo_write(s, value, size);
        /* The FIFO drained: signal the driver it may submit and credit more. */
        odyssey_raise_lowater(s);
        return;
    }
    if (addr == ODSY_USER_CMD_FIFO) {
        /* The user-mapped command FIFO: same grammar as the kernel port, but it
         * never carries the self-test preamble, so skip odyssey_selftest_track. */
        s->cmd_from_kernel = false;
        odyssey_cmd_fifo_write(s, value, size);
        odyssey_raise_lowater(s);
        return;
    }

    switch (odyssey_reg(addr, size)) {
    case ODSY_CONTROL:
        s->widget_control = value;
        break;
    case ODSY_XT_REQ_TIMEOUT:
        s->xt_req_timeout = value;
        break;
    case ODSY_INT_DEST_UPPER:
        s->int_dest_upper = value;
        break;
    case ODSY_INT_DEST_LOWER:
        s->int_dest_lower = value;
        break;
    case ODSY_ERROR_COMMAND:
        /* Written to acknowledge; the data itself is discarded. */
        s->error_command = 0;
        break;
    case ODSY_INTR_CLEAR:
        s->intr_flags &= ~(uint32_t)value;
        odyssey_update_irq(s);
        break;
    case ODSY_INTR_ENABLE_SET:
        s->intr_enable |= value;
        odyssey_update_irq(s);
        break;
    case ODSY_INTR_ENABLE_CLEAR:
        s->intr_enable &= ~(uint32_t)value;
        odyssey_update_irq(s);
        break;
    case ODSY_INTR_FLAGS:
        /* The driver writes here, to an effect no source settles. */
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: write of 0x%" PRIx64
                      " to the interrupt flags ignored; its effect is "
                      "unknown\n", value);
        break;
    case ODSY_RESET_CONTROL:
        /* The driver's six-write reset sequence; see the define's comment. */
        qemu_log_mask(LOG_UNIMP, "sgi-odyssey: write of 0x%" PRIx64
                      " to the reset control register accepted; its effect "
                      "is unknown\n", value);
        break;
    case ODSY_FLCTL_CONFIG:
        s->flctl_config = value;
        break;
    case ODSY_SW_SYNC:
        s->sw_sync = value;
        break;
    case ODSY_BUZZ_1:
        s->buzz[0] = value;
        break;
    case ODSY_BUZZ_2:
        s->buzz[1] = value;
        break;
    case ODSY_BUZZ_3:
        s->buzz[2] = value;
        break;
    case ODSY_BUZZ_4:
        s->buzz[3] = value;
        break;
    case ODSY_BUZZ_5:
        s->buzz[4] = value;
        break;
    case ODSY_BUZZ_6:
        s->buzz[5] = value;
        break;
    case ODSY_SWAP_ARM:
        /*
         * Queue flips for the next retrace (odyssey_retrace()). Writes
         * accumulate: the driver can issue several windows or swap groups
         * between two retraces, and its retrace ISR re-writes one window's
         * bit when a flip has not shown after two retraces. Whether the board
         * ORs or replaces is unknown; ORing loses no flip either way.
         */
        s->swap_arm |= value;
        break;
    case ODSY_SWAP_PENDING:
        s->swap_pending = value;
        break;
    case ODSY_SWAP_CONTROL:
        /*
         * The immediate-swap path: the kernel driver's buffer swap writes
         * the windows that must show buffer 1 here and those that must
         * show buffer 0 to the companion, then completes them without
         * waiting for a retrace, so the displayed-buffer bits follow at
         * once. Inferred from the driver; the kernel driver's board
         * reset's companion write of all ones leaves every window showing
         * buffer 0.
         */
        s->swap_control = value;
        s->displayed_buffer |= value;
        /* The windows now show their other buffer at once. */
        sgi_scanout_invalidate_all(&s->scanout);
        break;
    case ODSY_SWAP_CONTROL_COMPANION:
        s->swap_control_companion = value;
        s->displayed_buffer &= ~value;
        sgi_scanout_invalidate_all(&s->scanout);
        break;
    case ODSY_DISPLAYED_BUFFER:
        /*
         * Board-produced status; a guest write is not seen in the capture, but
         * store it rather than drop it, plain read/write like the Buzz set.
         */
        s->displayed_buffer = value;
        sgi_scanout_invalidate_all(&s->scanout);
        break;
    case ODSY_NIC_PROM:
    case ODSY_NIC_KERNEL:
        odyssey_nic_slot(s, value);
        break;
    case ODSY_ROUTE_DEST_UPPER:
        s->route_dest_upper = value;
        break;
    case ODSY_ROUTE_DEST_LOWER:
        s->route_dest_lower = value;
        break;
    case ODSY_ROUTE_VECTORS:
        s->route_vectors = value;
        break;
    case ODSY_HOST_WRITE_ADDR_UPPER:
        odyssey_query_cancel(s);
        s->host_write_addr_upper = value;
        break;
    case ODSY_HOST_WRITE_ADDR_LOWER:
        odyssey_query_cancel(s);
        s->host_write_addr_lower = value;
        break;
    case ODSY_CTX_SHADOW_ADDR_UPPER:
        odyssey_query_cancel(s);
        s->ctx_shadow_addr_upper = value;
        break;
    case ODSY_CTX_SHADOW_ADDR_LOWER:
        odyssey_query_cancel(s);
        s->ctx_shadow_addr_lower = value;
        break;
    case ODSY_IDENTITY:
    case ODSY_STATUS:
    case ODSY_ERROR_ADDR_UPPER:
    case ODSY_ERROR_ADDR_LOWER:
    case ODSY_MEMORY_CONFIG:
    case ODSY_I2C_MAIN_CMD_READ:
    case ODSY_I2C_MAIN_STATUS_READ:
    case ODSY_I2C_OPT_CMD_READ:
    case ODSY_I2C_OPT_STATUS_READ:
    case ODSY_BACKEND_VERSION:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: write of 0x%" PRIx64
                      " to the read-only register at 0x%" HWADDR_PRIx "\n",
                      value, addr);
        break;
    default:
        odyssey_unimplemented(s, addr, size, true);
        break;
    }
}

/* Take the one-shot mark before dispatch can make another memory access. */
static void odyssey_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    SGIOdysseyState *s = opaque;
    bool counted = sgi_pi_gfx_take_mark();

    odyssey_write_dispatch(s, addr, value, size);
    if (counted) {
        if (s->pi) {
            sgi_pi_gfx_return_credits(s->pi, s->parent_obj.slot, 1);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: counted graphics "
                          "store has no processor interface for its return\n");
        }
    }
}

static const MemoryRegionOps odyssey_ops = {
    .read = odyssey_read,
    .write = odyssey_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    /* Keep 64-bit register and FIFO words whole instead of splitting them. */
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/*
 * The board-identity record. Its field spelling is the one this project
 * captured off real SGI firmware (docs/library/boot-logs.md), and the
 * prologue -- a format byte, four spaces and a semicolon, so a scanning
 * reader meets ";Name:" and a positional reader finds the value at byte 11
 * -- is the convention hw/pci-host/sgi-bridge.c already writes.
 *
 * One field by default, because the stock IP35 PROM was observed reading
 * exactly one 32-byte page and stopping; a longer record is truncated
 * where the master stops clocking. The default value is the public owner
 * record of the board the Odyssey contract describes.
 */
static void odyssey_build_nic_rom(SGIOdysseyState *s,
                                  uint8_t rom[ODSY_NIC_ROM_SIZE])
{
    const char *fields = s->nic_record ?: ODSY_NIC_RECORD_DEFAULT;
    g_autofree char *record = g_strdup_printf("%c    ;%s;", 0x01, fields);
    size_t length = MIN(strlen(record), ODSY_NIC_ROM_SIZE);

    memset(rom, 0xff, ODSY_NIC_ROM_SIZE);
    memcpy(rom, record, length);
    if (length < ODSY_NIC_PAGE) {
        memset(&rom[length], ' ', ODSY_NIC_PAGE - length);
    }
}

static bool odyssey_attach_nic(SGIOdysseyState *s, Error **errp)
{
    uint8_t rom[ODSY_NIC_ROM_SIZE];
    DeviceState *part;

    if (s->nic_serial & ~ODSY_NIC_SERIAL_MASK) {
        error_setg(errp, "the sgi-odyssey one-wire serial 0x%" PRIx64
                   " does not fit in 48 bits", s->nic_serial);
        return false;
    }
    s->nic_bus = onewire_create_bus(DEVICE(s), "nic");
    odyssey_build_nic_rom(s, rom);
    part = qdev_new(TYPE_ONEWIRE_DS2505);
    object_property_add_child(OBJECT(s), "nic-part", OBJECT(part));
    qdev_prop_set_uint8(part, "family", ODSY_NIC_FAMILY);
    qdev_prop_set_uint64(part, "serial", s->nic_serial);
    ds2505_set_image(part, rom, sizeof(rom));
    return qdev_realize_and_unref(part, BUS(s->nic_bus), errp);
}

static bool odyssey_compose_memory_config(SGIOdysseyState *s, Error **errp)
{
    uint32_t code;

    switch (s->memory_size) {
    case 32:
        code = 0;
        break;
    case 64:
        code = 1;
        break;
    case 128:
        code = 2;
        break;
    default:
        error_setg(errp, "sgi-odyssey memory-size must be 32, 64 or 128 "
                   "megabytes, not %u", s->memory_size);
        return false;
    }
    if (s->memory_banks != 2 && s->memory_banks != 4) {
        error_setg(errp, "sgi-odyssey memory-banks must be 2 or 4, not %u",
                   s->memory_banks);
        return false;
    }
    if (s->cas_latency != 2 && s->cas_latency != 3) {
        error_setg(errp, "sgi-odyssey cas-latency must be 2 or 3, not %u",
                   s->cas_latency);
        return false;
    }
    /*
     * The outstanding-operation count and the data-FIFO level, which the
     * driver waits to reach zero, stay zero: neither is modeled.
     */
    s->memory_config = code << ODSY_CONFIG_SIZE_SHIFT |
                       (s->memory_banks == 2 ? ODSY_CONFIG_TWO_BANKS : 0) |
                       (s->cas_latency == 2 ? ODSY_CONFIG_LATENCY_2 : 0) |
                       ODSY_CONFIG_COPY_FIELD;
    return true;
}

static void odyssey_reset_registers(SGIOdysseyState *s)
{
    unsigned int i;

    /*
     * The driver reads its own widget number out of the low nibble before
     * anything has written the register. The rest of the reset value is
     * not established, so it is zero.
     */
    s->widget_control = SGI_XIO_BOARD(s)->slot & ODSY_CONTROL_WIDGET_MASK;
    s->xt_req_timeout = ODSY_XT_REQ_TIMEOUT_RESET;
    s->int_dest_upper = 0;
    s->int_dest_lower = 0;
    s->error_command = 0;
    /*
     * The command FIFO is empty out of reset, so its low-water condition holds
     * and the low-water interrupt is pending. The kernel driver's board
     * reset arms a watchdog and then tears the board down (the kernel
     * driver's FIFO watchdog) unless the driver sees this interrupt once,
     * which is what lets it create the direct rendering node Xsgi opens.
     * Latch it at reset -- coupled to the same reset that arms the
     * watchdog -- because the guest cannot reach the later FIFO-drain edge (a
     * command submission) until that node exists. Masked here until the driver
     * enables the source; enabling it then asserts.
     */
    s->intr_flags = ODSY_INTR_FIFO_LOW_WATER;
    s->intr_enable = 0;
    s->flctl_config = ODSY_FLCTL_CONFIG_RESET;
    s->sw_sync = 0;
    s->route_dest_upper = 0;
    s->route_dest_lower = 0;
    s->route_vectors = 0;
    s->host_write_addr_upper = 0;
    s->host_write_addr_lower = 0;
    s->ctx_shadow_addr_upper = 0;
    s->ctx_shadow_addr_lower = 0;
    memset(s->buzz, 0, sizeof(s->buzz));
    s->swap_arm = 0;
    s->swap_pending = 0;
    s->swap_control = 0;
    s->swap_control_companion = 0;
    s->displayed_buffer = 0;
    s->draw_mask = ODSY_DRAW_MASK_RESET;
    s->draw_word = 0;
    s->read_word = 0;
    s->draw_mask_stereo_reported = false;
    s->draw_mask_high_reported = false;
    s->nic = 0;
    s->i2c_main_cmd = 0;
    s->i2c_opt_cmd = 0;
    memset(s->lut, 0, sizeof(s->lut));
    memset(s->dispmode, 0, sizeof(s->dispmode));
    s->backend_index = 0;
    s->backend_count = 0;
    s->reported_addr_count = 0;
    s->reported_cmd_count = 0;
    s->line_stipple_reported = false;
    s->did_compare_reported = false;
    s->dma_bpp_reported = false;
    s->dma_readback_reported = false;
    s->dma_code_reported = false;
    s->dma_read_reported = 0;
    s->selftest_progress = 0;
    s->cfifo_selftest_ready = false;

    odyssey_query_cancel(s);
    odyssey_query_records_reset(s);
    s->win_origin[0] = 0;
    s->win_origin[1] = 0;
    s->read_origin[0] = 0;
    s->read_origin[1] = 0;
    for (i = 0; i < SGI_ODYSSEY_CLIP_RECTS; i++) {
        s->clip_words[2 * i] = i ? ODSY_CLIP_RESET_EMPTY : 0;
        s->clip_words[2 * i + 1] = i ? 0 : ODSY_CLIP_RESET_FULL;
    }
    odyssey_clip_update(s);
    s->did_compare = 0;
    memcpy(s->win_committed_origin, s->win_origin, sizeof(s->win_origin));
    memcpy(s->win_committed_clip, s->clip_words, sizeof(s->clip_words));
    s->win_committed_did = 0;
    s->cmd_from_kernel = false;
    s->gl_primitive = 0;
    s->gl_color = 0;
    /*
     * GL's initial index is 1 (OpenGL 1.2.1 section 2.7), the value the
     * index record's default 0xc580c406 1 saves and a replay restores, so a
     * context that never sets an index draws GL vertices in index 1 before
     * and after its first switch. The board's reset index is unknown; this
     * is a model default (the "Never-set index" verdict in
     * docs/reviews/odyssey-query-open-items-evidence-20261001.md).
     * Limitation: the integer rectangle fill takes the index only after an
     * index word, so with none since reset it draws the packed colour 0, and
     * index 1 after a replay. Only a register test reaches that: the X
     * server sends an index or a colour before every fill.
     */
    s->gl_color_index = 1;
    s->gl_color_is_index = false;
    /* Out of reset the mode is unknown; RGBA until a context says otherwise. */
    memset(s->colour_mode, 0, sizeof(s->colour_mode));
    s->colour_mode_reported = false;
    s->gl_index_lit_reported = false;
    s->dest_buffer_word = 0;
    s->plane_write_mask = 0;
    s->dma_colour_base = 0;
    s->dest_base = 0;
    s->obj_count = 0;
    s->obj_used = 0;
    memset(s->obj_base, 0, sizeof(s->obj_base));
    memset(s->obj_width, 0, sizeof(s->obj_width));
    memset(s->obj_offset, 0, sizeof(s->obj_offset));
    memset(s->obj_rows, 0, sizeof(s->obj_rows));
    s->obj_full_reported = false;
    s->board_oob_reported = false;
    s->tex_mode = 0;
    memset(s->tex_sampler, 0, sizeof(s->tex_sampler));
    memset(s->tex_colour_table, 0, sizeof(s->tex_colour_table));
    memset(s->tex_colour_table_valid, 0,
           sizeof(s->tex_colour_table_valid));
    memset(s->tex_env, 0, sizeof(s->tex_env));
    memset(s->tex_levels, 0, sizeof(s->tex_levels));
    s->tex_level0 = 0;
    s->tex_lod[0] = s->tex_lod[1] = 0;
    s->tex_lod[2] = 0x3100;
    s->tex_lod[3] = 0x0f00;
    memset(s->tex_coord, 0, sizeof(s->tex_coord));
    s->tex_coord_r = 0;
    s->tex_rect_reported = false;
    memset(s->gl_vertex_x, 0, sizeof(s->gl_vertex_x));
    memset(s->gl_vertex_y, 0, sizeof(s->gl_vertex_y));
    s->gl_vertex_count = 0;
    s->line_stipple[0] = s->line_stipple[1] = 0xffff;
    s->line_stipple[2] = 0;
    /* No polygon is open; reset_hold clears the marks themselves. */
    s->poly_row_lo = SGI_ODYSSEY_HEIGHT;
    s->poly_row_hi = 0;
    s->poly_col_lo = SGI_ODYSSEY_WIDTH;
    /*
     * The shared GL context to OpenGL's initial state (identity matrices,
     * white current colour, depth test off), unbound until the first GL
     * command binds the drawable. A guest reset can land mid-primitive, with
     * an open polygon's vertex buffer still allocated; sgi_gl_reset() frees
     * it before reinitializing, where sgi_gl_init() alone would leak it. It
     * is safe here even before realize allocates the colour plane. Its
     * viewport starts empty: nothing is drawn until the client's combined
     * matrix arrives (odyssey_gl_load_combined()).
     */
    sgi_gl_reset(&s->gl_ctx, ODSY_GL_CONTEXT_HW_ID);
    {
        /*
         * Opt into the shared context's per-face polygon modes (0x0415)
         * and line stipple: unset, it silently keeps every face at legacy
         * FILL no matter what 0x0415 decodes to. The sink built here always
         * carries the checked draw callback above, this call's one
         * precondition; sgi_gl_reset() above has already cleared in_begin,
         * satisfying the other precondition even when reset lands
         * mid-primitive.
         */
        SGIGLSink sink = odyssey_gl_sink(s);

        assert(sgi_gl_enable_polygon_modes(&s->gl_ctx, &sink));
        assert(sgi_gl_enable_line_stipple(&s->gl_ctx, &sink));
    }
    s->gl_in_prim = false;
    s->gl_begin_open = false;
    s->gl_integer_prim = false;
    s->gl_open_r = s->gl_open_c = 0;
    s->gl_open_m3 = s->gl_open_m4 = s->gl_open_held = 0;
    s->gl_restart_type = s->gl_restart_select = 0;
    s->gl_restart_absorb = s->gl_restart_total = 0;
    s->gl_restart_c = s->gl_restart_m3 = s->gl_restart_m4 = 0;
    s->gl_restart_armed = false;
    /*
     * The clip box and its scissor test (ODSY_REG_CLIP_BOX's comment):
     * sgi_gl_init() leaves the shared context's scissor box and test both
     * at zero, which would clip away every draw if the test were simply
     * switched on. Unlike this family's other 0x0404-derived state, there
     * is no reset pattern-mode value that already matches a usable scissor
     * default, so the full window is set here explicitly before the test
     * is enabled, once and for good.
     */
    s->gl_clip_box[0] = ODSY_CLIP_BOX_FIELD_MASK & (SGI_ODYSSEY_WIDTH - 1);
    s->gl_clip_box[1] = ODSY_CLIP_BOX_FIELD_MASK & (SGI_ODYSSEY_HEIGHT - 1);
    memset(s->gl_fog, 0, sizeof(s->gl_fog));
    odyssey_gl_apply_clip_box(s);
    {
        uint32_t scissor_test = SGI_GL_SCISSOR_TEST;

        odyssey_gl_feed(s, SGI_GL_OP_ENABLE, &scissor_test, 1);
    }
    s->gl_matrix_slot = SGI_GL_MODELVIEW;
    memset(s->gl_modelview, 0, sizeof(s->gl_modelview));
    for (i = 0; i < 4; i++) {
        s->gl_modelview[i * 5] = ODSY_F32_ONE;
    }
    memset(s->gl_matrix, 0, sizeof(s->gl_matrix));
    memset(s->gl_texture_matrix, 0, sizeof(s->gl_texture_matrix));
    for (i = 0; i < 4; i++) {
        s->gl_texture_matrix[i * 5] = ODSY_F32_ONE;
    }
    memset(s->gl_rect, 0, sizeof(s->gl_rect));
    s->gl_depth_func = ODSY_DEPTH_FUNC_RESET;
    s->gl_write_mask = ODSY_WRITE_MASK_RESET;
    memset(s->gl_combined, 0, sizeof(s->gl_combined));
    /* The identity projection's (-0, -0, -0, 1): the depth row unflipped. */
    s->gl_block16[0] = s->gl_block16[1] = s->gl_block16[2] = 0x80000000;
    s->gl_block16[3] = 0x3f800000;
    s->gl_rect_depth_logged = false;
    s->gl_rect_pattern_logged = false;
    s->gl_index_mask_logged = false;
    /*
     * Lighting out of reset. The hardware's values are unknown; the model
     * starts at GL's initial material, light-model ambient, normal and
     * colour, zero light colours and vectors (the library loads every light
     * before a lit draw), an identity normal matrix and no program.
     */
    memset(s->gl_const, 0, sizeof(s->gl_const));
    /* Program memory's reset content is unknown; the kernel loads it. */
    memset(s->gl_program_mem, 0, sizeof(s->gl_program_mem));
    s->gl_program_addr = 0;
    s->gl_program_half = 0;
    s->gl_program_mem_reported = false;
    s->gl_const_index = 0;
    memset(s->gl_entry, 0, sizeof(s->gl_entry));
    memset(s->gl_light_colour, 0, sizeof(s->gl_light_colour));
    for (i = 0; i < 3; i++) {
        s->gl_light_colour[ODSY_GL_LIGHT_MODEL_AMBIENT * 4 + i] =
            ODSY_F32_POINT_TWO;
        s->gl_material[i] = ODSY_F32_POINT_TWO;       /* ambient */
        s->gl_material[4 + i] = ODSY_F32_POINT_EIGHT; /* diffuse */
        s->gl_material[8 + i] = 0;                    /* specular */
        s->gl_material[12 + i] = 0;                   /* emission */
    }
    for (i = 0; i < 4; i++) {
        s->gl_material[i * 4 + 3] = ODSY_F32_ONE;
        s->gl_colour[i] = ODSY_F32_ONE;
    }
    s->gl_light_colour[ODSY_GL_LIGHT_MODEL_AMBIENT * 4 + 3] = ODSY_F32_ONE;
    memset(s->gl_light_vec, 0, sizeof(s->gl_light_vec));
    s->gl_light_colour_sel = 0;
    s->gl_light_vec_sel = 0;
    s->gl_shininess_sqrt = 0;
    odyssey_back_material_reset(s);
    s->gl_normal[0] = s->gl_normal[1] = 0;
    s->gl_normal[2] = ODSY_F32_ONE;
    memset(s->gl_normal_matrix, 0, sizeof(s->gl_normal_matrix));
    for (i = 0; i < 4; i++) {
        s->gl_normal_matrix[i * 5] = ODSY_F32_ONE;
    }
    s->gl_lit = false;
    s->gl_local_viewer = false;
    s->gl_colour_overridden = false;
    s->gl_program_logged_count = 0;
    s->gl_two_sided_logged = false;
    s->gl_texgen_logged = false;
    s->gl_clip_logged = false;
    s->gl_texture_stage_logged = false;
    s->gl_block_logged = false;
    s->ctx_open_prim_logged = false;
    s->ctx_shadow_fail_reported = false;
    s->gl_dither = false;
    s->gl_dither_reported = false;
    s->scanout_overlay_reported = false;
    s->scanout_group25_reported = false;
    s->copy_src = 0;
    s->copy_size = 0;
    s->copy_dst = 0;
    memset(s->pattern_buffer, 0, sizeof(s->pattern_buffer));
    s->pattern_shape = 0;
    s->pattern_mode = 0;
    s->pattern_load_offset = 0;
    s->image_active = false;
    s->image_x1 = 0;
    s->image_y1 = 0;
    s->image_x2 = 0;
    s->image_y2 = 0;
    s->image_cur_x = 0;
    s->image_cur_y = 0;
    s->image_fmt = 0;
    s->image_group_len = 0;
    s->image_group_painted = 0;
    s->image_code = 0;
    s->image_bytes = 0;
    s->image_nbytes = 0;
    s->image_code_reported = false;
    s->dma_seq = 0;
}

/*
 * The generation/capacity pairs the family evidence supports
 * (docs/reviews/odyssey-family-behaviour-evidence-20261001.md): v12 is the
 * measured 128 MB board odyssey_properties' defaults already describe; v10
 * is the same second-generation part cut down to 32 MB; v8 and v6 are the
 * first generation (revision nibble 1) at 128 MB and 32 MB. No profile
 * offers 64 MB, size code 3, or a revision nibble other than 0x1 and 0xa:
 * the evidence record does not establish them.
 */
static const struct {
    const char *name;
    uint32_t revision;
    uint32_t memory_size;
} odyssey_profiles[] = {
    { "v12", 0xa, 128 },
    { "v10", 0xa, 32 },
    { "v8",  0x1, 128 },
    { "v6",  0x1, 32 },
};

static bool odyssey_resolve_profile(SGIOdysseyState *s, Error **errp)
{
    size_t i;

    if (!s->profile) {
        return true;
    }
    for (i = 0; i < ARRAY_SIZE(odyssey_profiles); i++) {
        if (!strcmp(s->profile, odyssey_profiles[i].name)) {
            s->revision = odyssey_profiles[i].revision;
            s->memory_size = odyssey_profiles[i].memory_size;
            return true;
        }
    }
    error_setg(errp, "sgi-odyssey profile must be v12, v10, v8 or v6, not %s",
               s->profile);
    return false;
}

static bool odyssey_build_widget(SGIXioBoardState *board, Error **errp)
{
    SGIOdysseyState *s = SGI_ODYSSEY(board);

    if (!odyssey_resolve_profile(s, errp)) {
        return false;
    }
    if (s->revision > 0xf) {
        error_setg(errp, "sgi-odyssey revision must fit in four bits, not %u",
                   s->revision);
        return false;
    }
    if (!odyssey_compose_memory_config(s, errp)) {
        return false;
    }
    odyssey_reset_registers(s);
    board->cpu_face = &s->regs;
    board->packet_face = &s->regs;
    return true;
}

/*
 * The base class re-parents the board under its module, and re-parenting a
 * device destroys every bus hanging off it, so the board-identity bus has
 * to be built after the base realize rather than inside build_widget().
 */
static void odyssey_realize(DeviceState *dev, Error **errp)
{
    SGIOdysseyClass *oc = SGI_ODYSSEY_GET_CLASS(dev);
    SGIOdysseyState *s = SGI_ODYSSEY(dev);
    Error *local_err = NULL;

    /*
     * Named-slot placement (sgi_xio_board_place_by_name()) resolves fabric
     * itself during the parent realize below and refuses a property already
     * set, so the check and the address space it guards must wait until the
     * parent has placed the board. A build_widget failure returns through
     * local_err before either runs, leaving nothing here to unwind.
     */
    oc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    if (!s->fabric) {
        error_setg(errp, "sgi-odyssey needs the fabric its interrupt "
                  "packets travel");
        return;
    }
    address_space_init(&s->fabric_as, s->fabric, "sgi-odyssey.fabric");
    if (!odyssey_attach_nic(s, errp)) {
        address_space_destroy(&s->fabric_as);
        return;
    }

    /*
     * Fixed-size and not property-dependent, so realize-time allocation
     * (rather than instance_init) is only about matching where the console
     * is created below, not about sizing.
     *
     * The colour framebuffer is two RGBA8 planes, one per colour buffer,
     * each over device-backed board memory of its own: a model-owned byte
     * array, byte-for-byte the surface this model has always drawn. It is
     * not page-backed onto guest RAM -- see the mem field's comment for why
     * the framebuffer's guest placement stays unlocated. add_plane can only
     * fail on a programming error in these fixed sizes, so a failure aborts.
     */
    sgi_board_memory_init(&s->mem, (uint64_t)SGI_ODYSSEY_WIDTH *
                          SGI_ODYSSEY_HEIGHT * sizeof(uint32_t));
    {
        SGIPlane plane = {
            .format = SGI_PLANE_RGBA8,
            .packing = SGI_PACK_BE_RED_HIGH,
            .width = SGI_ODYSSEY_WIDTH,
            .height = SGI_ODYSSEY_HEIGHT,
            .stride = SGI_ODYSSEY_WIDTH * sizeof(uint32_t),
            .base = 0,
            .len = (uint64_t)SGI_ODYSSEY_WIDTH * SGI_ODYSSEY_HEIGHT *
                   sizeof(uint32_t),
            .bottom_origin = false,
            .evidence = "red-high RGBA8 approximation; the real board's "
                        "framebuffer base, stride and per-depth channel "
                        "packing are unlocated (odyssey-display-backend-"
                        "survey-20260922.md)",
        };

        s->colour_plane = sgi_board_memory_add_plane(&s->mem, &plane,
                                                     &error_abort);
        /*
         * Colour buffer 1, the same shape in its own memory
         * (ODSY_REG_DRAW_MASK). Two model planes preserve the visible
         * contract; they do not claim the board's two-buffer layout.
         */
        sgi_board_memory_init(&s->buffer1_mem, plane.len);
        plane.evidence = "second colour buffer, model storage; the board's "
                         "front/back placement and packing are unknown "
                         "(odyssey.md, buffer-selection contract)";
        s->buffer1_plane = sgi_board_memory_add_plane(&s->buffer1_mem, &plane,
                                                      &error_abort);
    }

    /*
     * The board-memory object store (ODSY_OBJ_POOL_BYTES): device-backed words
     * the objects are laid in by odyssey_object_rows(), addressed directly.
     */
    sgi_board_memory_init(&s->obj_mem, ODSY_OBJ_POOL_BYTES);

    s->did = g_new0(uint8_t, SGI_ODYSSEY_WIDTH * SGI_ODYSSEY_HEIGHT);
    s->poly_marks = g_new0(uint8_t, SGI_ODYSSEY_WIDTH * SGI_ODYSSEY_HEIGHT);

    /*
     * The depth plane: 24-bit depth in a 32-bit word per frame pixel, in
     * device-backed memory of its own. The depth visual changes the
     * destination-select mode word, but where the board keeps the depth
     * buffer, and its width and layout, are unknown, so no aperture reaches
     * this plane. 24 bits matches the Z24 names in the GL library.
     */
    sgi_board_memory_init(&s->depth_mem, (uint64_t)SGI_ODYSSEY_WIDTH *
                          SGI_ODYSSEY_HEIGHT * sizeof(uint32_t));
    {
        SGIPlane depth = {
            .format = SGI_PLANE_Z24,
            .packing = SGI_PACK_BE_RED_HIGH,
            .width = SGI_ODYSSEY_WIDTH,
            .height = SGI_ODYSSEY_HEIGHT,
            .stride = SGI_ODYSSEY_WIDTH * sizeof(uint32_t),
            .base = 0,
            .len = (uint64_t)SGI_ODYSSEY_WIDTH * SGI_ODYSSEY_HEIGHT *
                   sizeof(uint32_t),
            .bottom_origin = false,
            .evidence = "model plane; the board's depth-buffer placement and "
                        "layout are unknown (odyssey-gl-shading-depth-"
                        "lighting.md)",
        };

        s->depth_plane = sgi_board_memory_add_plane(&s->depth_mem, &depth,
                                                    &error_abort);
    }

    /*
     * The shared scanout owns the console and its surface, resolving the
     * colour plane through odyssey_head_ops.resolve_row. Setting the mode
     * allocates the presented frame, marks it all dirty and arms the retrace
     * timer at the mode's rate.
     */
    sgi_scanout_init(&s->scanout, dev, &odyssey_head_ops, s);
    sgi_scanout_set_mode(&s->scanout, &(SGIScanoutMode) {
        .width = SGI_ODYSSEY_WIDTH,
        .height = SGI_ODYSSEY_HEIGHT,
        .frame_clocks = ODSY_FRAME_CLOCKS,
        .clock_hz = ODSY_PIXEL_CLOCK_HZ,
    });
}

static void odyssey_unrealize(DeviceState *dev)
{
    SGIOdysseyClass *oc = SGI_ODYSSEY_GET_CLASS(dev);
    SGIOdysseyState *s = SGI_ODYSSEY(dev);

    oc->parent_unrealize(dev);
    sgi_gl_cleanup(&s->gl_ctx);
    address_space_destroy(&s->fabric_as);
    sgi_scanout_cleanup(&s->scanout);
    g_free(s->did);
    g_free(s->poly_marks);
    sgi_board_memory_cleanup(&s->mem);
    sgi_board_memory_cleanup(&s->buffer1_mem);
    sgi_board_memory_cleanup(&s->obj_mem);
    sgi_board_memory_cleanup(&s->depth_mem);
}

static void odyssey_reset_hold(Object *obj, ResetType type)
{
    SGIOdysseyState *s = SGI_ODYSSEY(obj);

    odyssey_reset_registers(s);
    odyssey_update_irq(s);
    /*
     * odyssey_build_widget() also calls odyssey_reset_registers() above,
     * before the board memory exists (realize initializes it after the base
     * class's own realize returns) -- this phase only ever runs through
     * the Resettable machinery, strictly after realize, so the board memory
     * is guaranteed to exist here. Zero the device-backed colour plane's
     * bytes directly; its planes are realize-time configuration and stay.
     * Redraw because reset clears it.
     */
    memset(s->mem.bytes, 0, s->mem.bytes_len);
    memset(s->buffer1_mem.bytes, 0, s->buffer1_mem.bytes_len);
    memset(s->obj_mem.bytes, 0, s->obj_mem.bytes_len);
    memset(s->did, 0, SGI_ODYSSEY_WIDTH * SGI_ODYSSEY_HEIGHT * sizeof(*s->did));
    memset(s->poly_marks, 0, SGI_ODYSSEY_WIDTH * SGI_ODYSSEY_HEIGHT);
    /* The depth plane starts at the far value, all ones (a model choice). */
    for (int y = 0; y < SGI_ODYSSEY_HEIGHT; y++) {
        for (int x = 0; x < SGI_ODYSSEY_WIDTH; x++) {
            sgi_plane_store(&s->depth_mem,
                            sgi_board_memory_plane(&s->depth_mem,
                                                   s->depth_plane),
                            x, y, ODSY_DEPTH24_MAX);
        }
    }
    /* Forget swaps and the cursor, repaint the cleared plane, restart phase. */
    sgi_scanout_reset(&s->scanout);
}

/*
 * Host-only qtest access to native pixel storage, in the colour buffer
 * x-test-pixel-buffer names (0 by default). Read-write: no register path
 * fills the framebuffer yet, so this doubles as how a qtest fills it.
 */
static void odyssey_test_get_pixel(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    SGIOdysseyState *s = SGI_ODYSSEY(obj);
    uint32_t value = 0;

    if (DEVICE(obj)->realized && s->test_pixel_x < SGI_ODYSSEY_WIDTH &&
        s->test_pixel_y < SGI_ODYSSEY_HEIGHT &&
        s->test_pixel_buffer < ODSY_BUFFERS) {
        value = odyssey_buffer_load(s, s->test_pixel_buffer,
                                    s->test_pixel_x, s->test_pixel_y);
    }
    visit_type_uint32(v, name, &value, errp);
}

/* Check the fallible destination callback without creating invalid draws. */
static void odyssey_test_get_rgba_readable(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    SGIOdysseyState *s = SGI_ODYSSEY(obj);
    SGIRasterColour colour;
    bool readable = DEVICE(obj)->realized &&
                    s->test_pixel_x <= INT_MAX && s->test_pixel_y <= INT_MAX &&
                    odyssey_gl_load_rgba(s, s->test_pixel_buffer,
                                         s->test_pixel_x, s->test_pixel_y,
                                         &colour);

    visit_type_bool(v, name, &readable, errp);
}

static void odyssey_test_set_pixel(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    SGIOdysseyState *s = SGI_ODYSSEY(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!DEVICE(obj)->realized) {
        error_setg(errp, "sgi-odyssey: x-test-pixel needs a realized device");
        return;
    }
    if (s->test_pixel_x >= SGI_ODYSSEY_WIDTH ||
        s->test_pixel_y >= SGI_ODYSSEY_HEIGHT) {
        error_setg(errp, "sgi-odyssey: x-test-pixel-x/y is out of range");
        return;
    }
    if (s->test_pixel_buffer >= ODSY_BUFFERS) {
        error_setg(errp, "sgi-odyssey: x-test-pixel-buffer is out of range");
        return;
    }
    odyssey_buffer_store(s, s->test_pixel_buffer, s->test_pixel_x,
                         s->test_pixel_y, value);
    sgi_scanout_invalidate_all(&s->scanout);
}

/* Update scanout before exposing a display pixel to qtests. */
static void odyssey_test_get_display_pixel(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    SGIOdysseyState *s = SGI_ODYSSEY(obj);
    uint32_t value = 0;

    if (DEVICE(obj)->realized) {
        qemu_console_hw_update(s->scanout.con);
    }
    if (DEVICE(obj)->realized && s->scanout.frame &&
        s->test_pixel_x < SGI_ODYSSEY_WIDTH &&
        s->test_pixel_y < SGI_ODYSSEY_HEIGHT) {
        value = s->scanout.frame[s->test_pixel_y * SGI_ODYSSEY_WIDTH +
                                 s->test_pixel_x];
    }
    visit_type_uint32(v, name, &value, errp);
}

/*
 * Host-only qtest read of a board-memory object: texel (x-test-pixel-x,
 * x-test-pixel-y) of the object at board base x-test-object-base, as
 * 0xRRGGBBAA; zero when there is no such object or texel.
 */
static void odyssey_test_get_object_texel(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    SGIOdysseyState *s = SGI_ODYSSEY(obj);
    uint32_t value = 0;
    int i;

    if (DEVICE(obj)->realized) {
        i = odyssey_object_find(s, s->test_object_base);
        if (i >= 0) {
            value = odyssey_object_load(s, i, s->test_pixel_x,
                                        s->test_pixel_y);
        }
    }
    visit_type_uint32(v, name, &value, errp);
}

/*
 * Host-only qtest trigger for one vertical retrace. The shared SGIScanout
 * drives odyssey_retrace() from a QEMU_CLOCK_VIRTUAL timer at the mode's rate,
 * but the register-contract tests halt the CPU (-S), which pauses that clock,
 * so a test cannot step time to reach a retrace. Setting this fires exactly the
 * hook the timer would, letting a test pin the swap latch, the displayed-buffer
 * advance and the completion interrupt. The per-frame timer wiring itself is
 * the foundation's and is exercised by a running guest.
 */
static void odyssey_test_fire_retrace(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    SGIOdysseyState *s = SGI_ODYSSEY(obj);
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    if (DEVICE(obj)->realized && value) {
        odyssey_retrace(s);
    }
}

static void odyssey_instance_init(Object *obj)
{
    SGIOdysseyState *s = SGI_ODYSSEY(obj);

    memory_region_init_io(&s->regs, obj, &odyssey_ops, s, "sgi-odyssey.regs",
                          ODSY_APERTURE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->regs);

    object_property_add_uint32_ptr(obj, "x-test-pixel-x", &s->test_pixel_x,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "x-test-pixel-y", &s->test_pixel_y,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "x-test-pixel-buffer",
                                   &s->test_pixel_buffer,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "x-test-draw-word", &s->draw_word,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-read-word", &s->read_word,
                                   OBJ_PROP_FLAG_READ);
    object_property_add(obj, "x-test-pixel", "uint32", odyssey_test_get_pixel,
                        odyssey_test_set_pixel, NULL, NULL);
    object_property_add(obj, "x-test-rgba-readable", "bool",
                        odyssey_test_get_rgba_readable, NULL, NULL, NULL);
    object_property_add(obj, "x-test-display-pixel", "uint32",
                        odyssey_test_get_display_pixel, NULL, NULL, NULL);
    object_property_add_uint32_ptr(obj, "x-test-object-base",
                                   &s->test_object_base,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-object-texel", "uint32",
                        odyssey_test_get_object_texel, NULL, NULL, NULL);
    object_property_add_uint32_ptr(obj, "x-test-object-count", &s->obj_count,
                                   OBJ_PROP_FLAG_READ);
    object_property_add(obj, "x-test-fire-retrace", "bool", NULL,
                        odyssey_test_fire_retrace, NULL, NULL);
    object_property_add_uint64_ptr(obj, "x-test-last-interrupt-addr",
                                   &s->test_last_interrupt_addr,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "x-test-last-interrupt-value",
                                   &s->test_last_interrupt_value,
                                   OBJ_PROP_FLAG_READ);
}

/*
 * Crosstalk part 0xc013 and manufacturer 0x023 are the board's, from the
 * part number the IP35 driver registers for. Revision nibble 0xa and the
 * 128 MB/two-bank/latency-3 memory profile are the values a live V12
 * board (030-1611-001 rev C) answered, not a placeholder
 * (odyssey-octane2-v12-registers-20260926.md). The one-wire record
 * defaults to the public owner record of the 128 MB board; the serial is
 * synthetic and says so.
 *
 * "profile" is a named preset for the generation/capacity pairs the family
 * evidence supports
 * (docs/reviews/odyssey-family-behaviour-evidence-20261001.md): v12
 * (default, the measured board above), v10, v8 and v6. Selecting one
 * overrides "revision" and "memory-size"; left unset, both keep their own
 * defaults/overrides unchanged.
 */
static const Property odyssey_properties[] = {
    DEFINE_PROP_LINK("pi", SGIOdysseyState, pi, TYPE_SGI_PI_GFX, Object *),
    DEFINE_PROP_UINT32("revision", SGIOdysseyState, revision, 0xa),
    DEFINE_PROP_UINT32("memory-size", SGIOdysseyState, memory_size, 128),
    DEFINE_PROP_UINT32("memory-banks", SGIOdysseyState, memory_banks, 2),
    DEFINE_PROP_UINT32("cas-latency", SGIOdysseyState, cas_latency, 3),
    DEFINE_PROP_STRING("profile", SGIOdysseyState, profile),
    DEFINE_PROP_UINT64("nic-serial", SGIOdysseyState, nic_serial,
                       0x5d0000000001ULL),
    DEFINE_PROP_STRING("nic-record", SGIOdysseyState, nic_record),
    DEFINE_PROP_LINK("fabric", SGIOdysseyState, fabric, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

/*
 * The colour framebuffer's board memory, carried as a subsection so adopting
 * the shared foundation leaves the family's own version_id untouched. Its
 * bytes newly migrate the rendered framebuffer, which the standalone array
 * never did.
 */
DEFINE_VMSTATE_SGI_BOARD_MEMORY(vmstate_odyssey_mem, "sgi-odyssey/board-memory",
                                SGIOdysseyState, mem);

/*
 * The scanout's mode, retrace phase, queued swaps and cursor, carried as a
 * second subsection for the same reason as the board memory's: adopting the
 * shared foundation leaves the family's own version_id untouched. The
 * presented frame itself is not migrated -- a loaded scanout marks every line
 * dirty and repaints from the colour plane.
 */
/*
 * The depth plane, a subsection of its own so a stream from before it existed
 * still loads, leaving the plane at its reset value.
 */
DEFINE_VMSTATE_SGI_BOARD_MEMORY(vmstate_odyssey_depth,
                                "sgi-odyssey/depth-memory",
                                SGIOdysseyState, depth_mem);

/*
 * Colour buffer 1 and the buffer-selection words, subsections so a stream
 * from before two buffers existed still loads: buffer 1 stays clear and the
 * drawing mask at its reset value, buffer 0.
 */
DEFINE_VMSTATE_SGI_BOARD_MEMORY(vmstate_odyssey_buffer1,
                                "sgi-odyssey/buffer1-memory",
                                SGIOdysseyState, buffer1_mem);

static const VMStateDescription vmstate_odyssey_buffer_select = {
    .name = "sgi-odyssey/buffer-select",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(draw_mask, SGIOdysseyState),
        VMSTATE_UINT32(draw_word, SGIOdysseyState),
        VMSTATE_UINT32(read_word, SGIOdysseyState),
        VMSTATE_END_OF_LIST()
    },
};

/* The object store's words, a subsection for the same reason (v21). */
DEFINE_VMSTATE_SGI_BOARD_MEMORY(vmstate_odyssey_objects,
                                "sgi-odyssey/object-memory",
                                SGIOdysseyState, obj_mem);

DEFINE_VMSTATE_SGI_SCANOUT(vmstate_odyssey_scanout, "sgi-odyssey/scanout",
                           SGIOdysseyState, scanout);

/* Streams 14 to 22 held up to 128 integer vertices (v23 streams them). */
static bool odyssey_stream_had_vertex_buffer(void *opaque, int version_id)
{
    return version_id >= 14 && version_id < 23;
}

/* Only version 25 kept a program window per context. */
static bool odyssey_stream_had_context_windows(void *opaque, int version_id)
{
    return version_id == 25;
}

/* An open integer POLYGON's crossing marks, sent only while one is open. */
static bool odyssey_polygon_needed(void *opaque)
{
    SGIOdysseyState *s = opaque;

    return s->gl_primitive == ODSY_GL_POLYGON && s->gl_vertex_count &&
           !s->image_active;
}

static int odyssey_polygon_post_load(void *opaque, int version_id)
{
    SGIOdysseyState *s = opaque;

    if (s->poly_row_hi > SGI_ODYSSEY_HEIGHT ||
        s->poly_col_lo > SGI_ODYSSEY_WIDTH) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_odyssey_polygon = {
    .name = "sgi-odyssey/polygon",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = odyssey_polygon_needed,
    .post_load = odyssey_polygon_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER_POINTER_UNSAFE(poly_marks, SGIOdysseyState, 0,
                                      SGI_ODYSSEY_WIDTH * SGI_ODYSSEY_HEIGHT),
        VMSTATE_UINT32(poly_row_lo, SGIOdysseyState),
        VMSTATE_UINT32(poly_row_hi, SGIOdysseyState),
        VMSTATE_UINT32(poly_col_lo, SGIOdysseyState),
        VMSTATE_END_OF_LIST()
    },
};

static bool odyssey_back_material_needed(void *opaque)
{
    SGIOdysseyState *s = opaque;

    return s->gl_back_shininess_sqrt ||
           memcmp(s->gl_back_material, odyssey_back_material_defaults,
                  sizeof(s->gl_back_material));
}

static const VMStateDescription vmstate_odyssey_back_material = {
    .name = "sgi-odyssey/back-material",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = odyssey_back_material_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(gl_back_material, SGIOdysseyState, 16),
        VMSTATE_UINT32(gl_back_shininess_sqrt, SGIOdysseyState),
        VMSTATE_END_OF_LIST()
    },
};

static int odyssey_pre_load(void *opaque)
{
    SGIOdysseyState *s = opaque;

    /* An absent subsection replaces any destination state with defaults. */
    odyssey_back_material_reset(opaque);
    /* Before version 29 the records were not sent: load their defaults. */
    odyssey_query_records_reset(opaque);
    memset(s->gl_fog, 0, sizeof(s->gl_fog));
    s->gl_begin_open = false;
    s->gl_integer_prim = false;
    s->gl_open_r = s->gl_open_c = 0;
    s->gl_open_m3 = s->gl_open_m4 = s->gl_open_held = 0;
    s->gl_restart_absorb = s->gl_restart_total = 0;
    s->gl_restart_armed = false;
    return 0;
}

/*
 * Rebuild what is derived from migrated registers. The shared GL context is
 * transient render state and is not migrated, so its viewport restarts empty
 * until the client's next combined matrix; the drawable is rebound from the
 * migrated origin and rectangles before its next command, the shade model and
 * depth state are taken again from the migrated 0x0404, 0x0420 and 0x0452
 * words, and the projection from the kept combined matrix and block-16 sign.
 * The lighting program is recognised again from the migrated entries and
 * constants. The model-view matrix is not kept and restarts at the identity.
 */
static int odyssey_post_load(void *opaque, int version_id)
{
    SGIOdysseyState *s = opaque;

    if (s->gl_program_half >= ODSY_PROG_ENTRY_HALFWORDS) {
        return -EINVAL;
    }
    /* A retained word has at most four operands, and five records exist. */
    if (s->query_current_valid & ~0x1fu) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->query_current); i++) {
        if (odyssey_cmd_arity(s->query_current[i][0]) >
            ARRAY_SIZE(s->query_current[i]) - 1) {
            return -EINVAL;
        }
    }

    if (version_id < 28) {
        memset(s->gl_texture_matrix, 0, sizeof(s->gl_texture_matrix));
        for (unsigned i = 0; i < 4; i++) {
            s->gl_texture_matrix[i * 5] = ODSY_F32_ONE;
        }
    }
    if (version_id < 14) {
        s->line_stipple[0] = s->line_stipple[1] = 0xffff;
        s->line_stipple[2] = 0;
    }
    if (version_id < 35) {
        memset(s->tex_colour_table, 0, sizeof(s->tex_colour_table));
        memset(s->tex_colour_table_valid, 0,
               sizeof(s->tex_colour_table_valid));
    }
    if (version_id < 31) {
        s->tex_lod[0] = s->tex_lod[1] = 0;
        s->tex_lod[2] = 0x3100;
        s->tex_lod[3] = 0x0f00;
    }
    if (version_id < 32) {
        memcpy(s->read_origin, s->win_origin, sizeof(s->read_origin));
    }
    odyssey_clip_update(s);
    {
        SGIGLSink sink = odyssey_gl_sink(s);

        assert(sgi_gl_enable_line_stipple(&s->gl_ctx, &sink));
    }
    odyssey_gl_apply_mode(s);
    odyssey_gl_apply_line_stipple(s);
    /*
     * The shared context's scissor box and clip-plane enables are transient
     * render state, not migrated; reset already gave gl_clip_box its
     * full-window default before this runs, so a pre-v30 sender with no
     * clip box in its stream still re-applies that default correctly here.
     */
    odyssey_gl_apply_clip_box(s);
    odyssey_gl_apply_clip_planes(s);
    odyssey_gl_apply_depth(s);
    odyssey_gl_reload_combined(s);
    odyssey_gl_program(s);
    /*
     * Before version 23 the held vertices were the first four of a buffer,
     * not what a streaming primitive holds: drop a primitive left open.
     */
    if (version_id < 23) {
        s->gl_vertex_count = 0;
    }
    /* The shared context's current colour restarts white: restore gl_colour. */
    s->gl_colour_overridden = true;
    return 0;
}

static const VMStateDescription vmstate_sgi_odyssey = {
    .name = TYPE_SGI_ODYSSEY,
    .version_id = 35,
    .minimum_version_id = 5,
    .pre_load = odyssey_pre_load,
    .post_load = odyssey_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(widget_control, SGIOdysseyState),
        VMSTATE_UINT32(xt_req_timeout, SGIOdysseyState),
        VMSTATE_UINT32(int_dest_upper, SGIOdysseyState),
        VMSTATE_UINT32(int_dest_lower, SGIOdysseyState),
        VMSTATE_UINT32(error_command, SGIOdysseyState),
        VMSTATE_UINT32(intr_flags, SGIOdysseyState),
        VMSTATE_UINT32(intr_enable, SGIOdysseyState),
        VMSTATE_UINT32(flctl_config, SGIOdysseyState),
        VMSTATE_UINT32(sw_sync, SGIOdysseyState),
        VMSTATE_UINT32(route_dest_upper, SGIOdysseyState),
        VMSTATE_UINT32(route_dest_lower, SGIOdysseyState),
        VMSTATE_UINT32(route_vectors, SGIOdysseyState),
        VMSTATE_UINT32(host_write_addr_upper, SGIOdysseyState),
        VMSTATE_UINT32(host_write_addr_lower, SGIOdysseyState),
        VMSTATE_UINT32(ctx_shadow_addr_upper, SGIOdysseyState),
        VMSTATE_UINT32(ctx_shadow_addr_lower, SGIOdysseyState),
        VMSTATE_UINT32_ARRAY(buzz, SGIOdysseyState, 6),
        VMSTATE_UINT32(nic, SGIOdysseyState),
        VMSTATE_UINT32(i2c_main_cmd, SGIOdysseyState),
        VMSTATE_UINT32(i2c_opt_cmd, SGIOdysseyState),
        /* The display back end's colormap RAM, display modes and parser state. */
        VMSTATE_UINT32_ARRAY(lut, SGIOdysseyState, 0x2000),
        VMSTATE_UINT32_ARRAY(dispmode, SGIOdysseyState, 32),
        VMSTATE_UINT32(backend_index, SGIOdysseyState),
        VMSTATE_UINT32(backend_count, SGIOdysseyState),
        VMSTATE_UINT32(selftest_progress, SGIOdysseyState),
        VMSTATE_BOOL(cfifo_selftest_ready, SGIOdysseyState),
        /* The command-FIFO interpreter's parser and drawing state. */
        VMSTATE_UINT32(cmd_state, SGIOdysseyState),
        VMSTATE_UINT32(cmd_tag, SGIOdysseyState),
        VMSTATE_UINT32_SUB_ARRAY(cmd_operand, SGIOdysseyState, 0,
                                 ODSY_CMD_OLD_OPERANDS),
        VMSTATE_UINT32(cmd_operand_count, SGIOdysseyState),
        VMSTATE_UINT32(cmd_operand_index, SGIOdysseyState),
        VMSTATE_UINT32(gl_primitive, SGIOdysseyState),
        VMSTATE_UINT32(gl_color, SGIOdysseyState),
        VMSTATE_UINT32_ARRAY(gl_vertex_x, SGIOdysseyState,
                             SGI_ODYSSEY_GL_HELD_VERTICES),
        VMSTATE_UINT32_ARRAY(gl_vertex_y, SGIOdysseyState,
                             SGI_ODYSSEY_GL_HELD_VERTICES),
        VMSTATE_UINT32(gl_vertex_count, SGIOdysseyState),
        /*
         * Streams from 14 to 22 carry the retired 128-vertex buffer's other
         * 124 x and 124 y words and its overflow flag.
         */
        VMSTATE_UNUSED_BUFFER(odyssey_stream_had_vertex_buffer, 0,
                              2 * 124 * sizeof(uint32_t) + 1),
        VMSTATE_UINT32_ARRAY_V(line_stipple, SGIOdysseyState, 3, 14),
        /* The fill colour source and drawable-id plane write state (v12). */
        VMSTATE_UINT32_V(gl_color_index, SGIOdysseyState, 12),
        VMSTATE_BOOL_V(gl_color_is_index, SGIOdysseyState, 12),
        VMSTATE_UINT32_V(dest_buffer_word, SGIOdysseyState, 12),
        VMSTATE_UINT32_V(plane_write_mask, SGIOdysseyState, 12),
        VMSTATE_BUFFER_POINTER_UNSAFE(did, SGIOdysseyState, 12,
                                      SGI_ODYSSEY_WIDTH * SGI_ODYSSEY_HEIGHT),
        VMSTATE_UINT32(copy_src, SGIOdysseyState),
        VMSTATE_UINT32(copy_size, SGIOdysseyState),
        VMSTATE_UINT32(copy_dst, SGIOdysseyState),
        /* The one-bit pattern-quad text state (see the seventh increment). */
        VMSTATE_UINT32_ARRAY(pattern_buffer, SGIOdysseyState, 32),
        VMSTATE_UINT32(pattern_shape, SGIOdysseyState),
        VMSTATE_UINT32(pattern_mode, SGIOdysseyState),
        VMSTATE_UINT32(pattern_load_offset, SGIOdysseyState),
        /* The BEGIN-0xa image raster path's parser and cursor state. */
        VMSTATE_BOOL(image_active, SGIOdysseyState),
        VMSTATE_UINT32(image_x1, SGIOdysseyState),
        VMSTATE_UINT32(image_y1, SGIOdysseyState),
        VMSTATE_UINT32(image_x2, SGIOdysseyState),
        VMSTATE_UINT32(image_y2, SGIOdysseyState),
        VMSTATE_UINT32(image_cur_x, SGIOdysseyState),
        VMSTATE_UINT32(image_cur_y, SGIOdysseyState),
        VMSTATE_UINT32(image_fmt, SGIOdysseyState),
        VMSTATE_UINT32(image_group_len, SGIOdysseyState),
        VMSTATE_UINT32(image_group_painted, SGIOdysseyState),
        /* The one modeled source's transition state (see the packet code). */
        VMSTATE_BOOL(interrupt_asserted, SGIOdysseyState),
        /* The swap-control bank and displayed-buffer status (v13). */
        VMSTATE_UINT32_V(swap_arm, SGIOdysseyState, 13),
        VMSTATE_UINT32_V(swap_pending, SGIOdysseyState, 13),
        VMSTATE_UINT32_V(swap_control, SGIOdysseyState, 13),
        VMSTATE_UINT32_V(swap_control_companion, SGIOdysseyState, 13),
        VMSTATE_UINT32_V(displayed_buffer, SGIOdysseyState, 13),
        /*
         * The rest of a burst's operands and the window placement registers
         * (v15). The kept clip rectangles are rebuilt in post_load.
         */
        VMSTATE_SUB_ARRAY(cmd_operand, SGIOdysseyState, ODSY_CMD_OLD_OPERANDS,
                          ODSY_CMD_MAX_OPERANDS - ODSY_CMD_OLD_OPERANDS, 15,
                          vmstate_info_uint32, uint32_t),
        VMSTATE_INT32_ARRAY_V(win_origin, SGIOdysseyState, 2, 15),
        VMSTATE_INT32_ARRAY_V(read_origin, SGIOdysseyState, 2, 32),
        VMSTATE_UINT32_ARRAY_V(clip_words, SGIOdysseyState,
                               2 * SGI_ODYSSEY_CLIP_RECTS, 15),
        VMSTATE_UINT32_V(did_compare, SGIOdysseyState, 15),
        /* The colour buffer's base a DMA header names (v16). */
        VMSTATE_UINT32_V(dma_colour_base, SGIOdysseyState, 16),
        /* The GL depth state and the matrices it is rebuilt from (v17). */
        VMSTATE_UINT32_V(gl_depth_func, SGIOdysseyState, 17),
        VMSTATE_UINT32_V(gl_write_mask, SGIOdysseyState, 17),
        VMSTATE_UINT32_ARRAY_V(gl_combined, SGIOdysseyState, 16, 17),
        VMSTATE_UINT32_ARRAY_V(gl_block16, SGIOdysseyState, 4, 17),
        /* The geometry program and the lighting data (v18). */
        VMSTATE_UINT32_ARRAY_V(gl_const, SGIOdysseyState,
                               SGI_ODYSSEY_GL_CONSTANTS, 18),
        VMSTATE_UINT32_V(gl_const_index, SGIOdysseyState, 18),
        VMSTATE_UINT32_ARRAY_V(gl_entry, SGIOdysseyState,
                               SGI_ODYSSEY_GL_ENTRIES, 18),
        VMSTATE_UINT32_ARRAY_V(gl_light_colour, SGIOdysseyState,
                               SGI_ODYSSEY_GL_LIGHT_COLOURS * 4, 18),
        VMSTATE_UINT32_ARRAY_V(gl_light_vec, SGIOdysseyState,
                               SGI_ODYSSEY_GL_LIGHTS * 16, 18),
        VMSTATE_UINT32_V(gl_light_colour_sel, SGIOdysseyState, 18),
        VMSTATE_UINT32_V(gl_light_vec_sel, SGIOdysseyState, 18),
        VMSTATE_UINT32_ARRAY_V(gl_material, SGIOdysseyState, 16, 18),
        VMSTATE_UINT32_V(gl_shininess_sqrt, SGIOdysseyState, 18),
        VMSTATE_UINT32_ARRAY_V(gl_normal, SGIOdysseyState, 3, 18),
        VMSTATE_UINT32_ARRAY_V(gl_normal_matrix, SGIOdysseyState, 16, 18),
        VMSTATE_UINT32_ARRAY_V(gl_colour, SGIOdysseyState, 4, 18),
        /* The kernel-committed window the restore word brings back (v19). */
        VMSTATE_INT32_ARRAY_V(win_committed_origin, SGIOdysseyState, 2, 19),
        VMSTATE_UINT32_ARRAY_V(win_committed_clip, SGIOdysseyState,
                               2 * SGI_ODYSSEY_CLIP_RECTS, 19),
        VMSTATE_UINT32_V(win_committed_did, SGIOdysseyState, 19),
        /* The image source format and a pixel split across words (v20). */
        VMSTATE_UINT32_V(image_code, SGIOdysseyState, 20),
        VMSTATE_UINT32_V(image_bytes, SGIOdysseyState, 20),
        VMSTATE_UINT32_V(image_nbytes, SGIOdysseyState, 20),
        /* The board-memory object table; the words are a subsection (v21). */
        VMSTATE_UINT32_V(dest_base, SGIOdysseyState, 21),
        VMSTATE_UINT32_V(obj_count, SGIOdysseyState, 21),
        VMSTATE_UINT32_V(obj_used, SGIOdysseyState, 21),
        VMSTATE_UINT32_ARRAY_V(obj_base, SGIOdysseyState,
                               SGI_ODYSSEY_OBJECTS, 21),
        VMSTATE_UINT32_ARRAY_V(obj_width, SGIOdysseyState,
                               SGI_ODYSSEY_OBJECTS, 21),
        VMSTATE_UINT32_ARRAY_V(obj_offset, SGIOdysseyState,
                               SGI_ODYSSEY_OBJECTS, 21),
        VMSTATE_UINT32_ARRAY_V(obj_rows, SGIOdysseyState,
                               SGI_ODYSSEY_OBJECTS, 21),
        /* The texture state words and coordinate (v22). */
        VMSTATE_UINT32_V(tex_mode, SGIOdysseyState, 22),
        VMSTATE_UINT32_ARRAY_V(tex_sampler, SGIOdysseyState, 7, 22),
        VMSTATE_UINT32_2DARRAY_V(tex_colour_table, SGIOdysseyState,
                                 2, SGI_ODYSSEY_TEX_COLOUR_TABLE_ENTRIES, 35),
        VMSTATE_UINT32_ARRAY_V(tex_colour_table_valid, SGIOdysseyState, 2, 35),
        VMSTATE_UINT32_ARRAY_V(tex_env, SGIOdysseyState, 5, 22),
        VMSTATE_UINT32_ARRAY_V(tex_levels, SGIOdysseyState,
                               SGI_ODYSSEY_TEX_LEVELS * 3, 22),
        VMSTATE_UINT32_V(tex_level0, SGIOdysseyState, 22),
        VMSTATE_UINT32_ARRAY_V(tex_lod, SGIOdysseyState, 4, 31),
        VMSTATE_UINT32_ARRAY_V(tex_coord, SGIOdysseyState, 2, 22),
        VMSTATE_UINT32_V(tex_coord_r, SGIOdysseyState, 34),
        /* The colour-mode words (v24). */
        VMSTATE_UINT32_ARRAY_V(colour_mode, SGIOdysseyState, 2, 24),
        /* Program memory and the per-context windows (v25). */
        VMSTATE_UINT16_ARRAY_V(gl_program_mem, SGIOdysseyState,
                               SGI_ODYSSEY_GL_PROGRAM_ENTRIES * 6, 25),
        VMSTATE_UINT32_V(gl_program_addr, SGIOdysseyState, 25),
        VMSTATE_UINT32_V(gl_program_half, SGIOdysseyState, 25),
        /*
         * Version 25 streams carry the retired per-context windows: an owner
         * word, eight context ids, eight 0xc7-entry windows and a next-slot
         * word.
         */
        VMSTATE_UNUSED_BUFFER(odyssey_stream_had_context_windows, 0,
                              4 + 8 * 4 + 8 * 0xc7 * 6 * 2 + 4),
        /* The model-view lighting reads (v27). */
        VMSTATE_UINT32_ARRAY_V(gl_modelview, SGIOdysseyState, 16, 27),
        VMSTATE_UINT32_ARRAY_V(gl_texture_matrix, SGIOdysseyState, 16, 28),
        /*
         * The retained current-attribute records and their support mask
         * (v29): the context save writes records 0 and 1 back to the page.
         */
        VMSTATE_UINT32_2DARRAY_V(query_current, SGIOdysseyState, 5, 5, 29),
        VMSTATE_UINT32_V(query_current_valid, SGIOdysseyState, 29),
        /* The 0x81d clip box, window-relative min/max pairs (v30). */
        VMSTATE_UINT32_ARRAY_V(gl_clip_box, SGIOdysseyState, 2, 30),
        VMSTATE_UINT32_ARRAY_V(gl_fog, SGIOdysseyState, 6, 33),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_odyssey_mem,
        &vmstate_odyssey_scanout,
        &vmstate_odyssey_depth,
        &vmstate_odyssey_objects,
        &vmstate_odyssey_buffer1,
        &vmstate_odyssey_buffer_select,
        &vmstate_odyssey_polygon,
        &vmstate_odyssey_back_material,
        NULL
    },
};

static void odyssey_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);
    SGIOdysseyClass *odc = SGI_ODYSSEY_CLASS(oc);

    dc->desc = "SGI Odyssey (VPro) graphics board";
    dc->vmsd = &vmstate_sgi_odyssey;
    dc->user_creatable = true;
    device_class_set_props(dc, odyssey_properties);
    device_class_set_parent_realize(dc, odyssey_realize, &odc->parent_realize);
    device_class_set_parent_unrealize(dc, odyssey_unrealize,
                                      &odc->parent_unrealize);
    rc->phases.hold = odyssey_reset_hold;
    bc->build_widget = odyssey_build_widget;
}

static const TypeInfo odyssey_types[] = {
    {
        .name = TYPE_SGI_ODYSSEY,
        .parent = TYPE_SGI_XIO_BOARD,
        .instance_size = sizeof(SGIOdysseyState),
        .class_size = sizeof(SGIOdysseyClass),
        .instance_init = odyssey_instance_init,
        .class_init = odyssey_class_init,
    },
};

DEFINE_TYPES(odyssey_types)
