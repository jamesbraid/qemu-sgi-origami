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
#include "hw/display/sgi/board-memory.h"
#include "hw/display/sgi/gl-context.h"
#include "hw/display/sgi/scanout.h"
#include "hw/misc/sgi-command-port.h"
#include "hw/misc/sgi-kona-dma.h"
#include "hw/misc/sgi-kona-ports.h"
#include "system/memory.h"
#include "ui/sgi-renderer.h"
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
 *
 * Bits [5:3] are a second, established field: the kernel driver's XG
 * map RAM entry-count code (N), selecting (N == 0 ? 8 : N) << 15 entries.
 * The default probe_value answers 0 here too, so the count is the maximum
 * SGI_KONA_MAPRAM_ENTRIES below, and every count the field could select
 * stays within the fixed backing array that size names.
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
 * The generic crosstalk interrupt destination, as hw/misc/sgi-mgras.c's
 * SGI_MGRAS_INT_DEST_UPPER/LOWER read it: the upper word carries the
 * target widget in bits [19:16] and address bits [47:32] in [15:0], the
 * lower word address bits [31:0]. The driver's kona_reset_adaptor writes
 * both (established); the field layout is the generic widget convention
 * taken from the MGRAS model and inferred for this pipe. A guest run
 * whose geometry-engine interrupts reach the node's Hub at widget 9 is
 * evidence the model's reading works, not a hardware fact.
 */
#define SGI_KONA_INT_DEST_UPPER 0x000034
#define SGI_KONA_INT_DEST_LOWER 0x00003c
#define SGI_KONA_INT_DEST_WIDGET_SHIFT 16
#define SGI_KONA_INT_DEST_WIDGET_MASK  0xfu

/*
 * The three interrupt vectors, established from kona_reset_adaptor:
 * 0x20000 | (xg vector << 18) | (geometry-engine vector << 9) | general
 * vector, each eight bits. A captured run wrote 0x3e2210 (0xf, 0x11,
 * 0x10). Only the geometry-engine vector drives anything in the model;
 * bit 17 is stored with no meaning given to it.
 */
#define SGI_KONA_INT_VECTORS 0x00005c
#define SGI_KONA_INT_VECTORS_GE_SHIFT 9

/*
 * The IO6 firmware's one-time low-window write, distinct from the kernel
 * FIFO port hw/misc/sgi-kona-ports.h describes at this same offset --
 * see that header's own note on the ambiguity. Accepted and stored only;
 * the observed value (0x23) has no established meaning.
 */
#define SGI_KONA_LOW_WINDOW_WRITE 0x00007c

/*
 * The credit register the PROM and the driver poll before a command.
 * Masked with 0x7fe; the PROM waits for exactly zero, the
 * driver while the value is above zero, so zero is "go" to both and
 * anything else is "wait" to both. The model reports the port words it
 * holds and has not consumed, a count in bits 10:1: the words its
 * command-port framers hold of a packet still arriving, plus the words
 * of framed packets whose CONSUMED completion has not come back (see
 * sgi_kona_publish()). Between packets that is zero, so both readers
 * proceed. That the count sits above bit 0 is inferred from the mask and
 * from the driver's credit-request word, a count shifted left one; that
 * the register counts held words at all, rather than free space, is the
 * model's reading of the two polarities, which agree only on zero. No
 * write case is established.
 */
#define SGI_KONA_CREDIT 0x800208
#define SGI_KONA_CREDIT_MASK 0x7fe

/*
 * The initial credit configuration the driver writes once, 0x700. What
 * it configures is unknown: the model stores it, reads it back, and lets it
 * change nothing, so the credit register answers the kernel after the
 * write as it answered the PROM before it.
 */
#define SGI_KONA_CREDIT_CONFIG 0x800210

/*
 * Plain stored field, written 0 as the first step of the firmware's
 * recovery sequence on a failed sync probe. The kernel driver's own
 * KONAinitialize writes 0x8000 here instead, from a softc shadow at
 * +0x93c, right before calling kona_mapram_init(). This model interprets
 * neither writer's value nor its readback.
 */
#define SGI_KONA_RESET_ARG 0x800220

/*
 * The kernel driver's own KONAinitialize writes 0x100 here, from a softc
 * shadow at +0x940, between its SGI_KONA_RESET_STROBE and SGI_KONA_RESET_ARG
 * writes. No driver read of this offset is established, so reading zero is
 * an approximation. This model accepts and stores the write, but always
 * answers a plain 0 on read -- unlike this window's other plain-storage
 * registers, which echo back what was written.
 */
#define SGI_KONA_INT_ENABLE 0x800230

/*
 * The mailbox interrupt-cause register. Bit MAILBOX is established:
 * write-1-to-clear, and a clear that leaves a further reply word queued
 * re-arms it synchronously -- this model has no separate device-side
 * timing to simulate the real interrupt's latency.
 */
#define SGI_KONA_INT_CAUSE 0x800238
#define SGI_KONA_INT_CAUSE_MAILBOX (1u << 4)

/*
 * The geometry-engine cause bit: kona_ge_intr_handler writes it back to
 * acknowledge. The model sets it when it posts a completion reason (see
 * sgi_kona_post_reason()) and the write-1-to-clear above clears it; that
 * the write clears it is inferred from the handler's write.
 */
#define SGI_KONA_INT_CAUSE_GE (1u << 5)

/*
 * The general handler's video bit and one of its fatal bits, from the
 * driver's cause-bit dispatch: 0x8 calls kona_video_intr, 0x100 kona_die.
 */
#define SGI_KONA_INT_CAUSE_VIDEO (1u << 3)
#define SGI_KONA_INT_CAUSE_FATAL (1u << 8)

/*
 * The general status vector, bits [7:0] of SGI_KONA_INT_VECTORS
 * (kona_reset_adaptor, established; 0x10 in the captured run), which
 * delivers the cause bits above to the driver's general handler.
 */
#define SGI_KONA_INT_VECTORS_GENERAL_MASK 0xffu

/*
 * The retrace tag register. On each video interrupt kona_video_intr reads
 * it and completes, in order, every queued retrace callback whose tag is
 * at most the value read (established from the driver). A swap's
 * callback is
 * kona_swap_complete, which wakes the client waiting on its swap. The
 * model sets it to the tag of the last retrace request (0x1e7) a retrace
 * honoured, and sets SGI_KONA_INT_CAUSE_VIDEO at that retrace: inferred
 * from the handler, as is the one request honoured per retrace, which
 * follows the shared scanout's swap queue. Whether the bit also rises at
 * retraces with nothing honoured is unknown; the model raises it only
 * when a tag completes. No write is established.
 */
#define SGI_KONA_RETRACE_TAG 0x800248

/*
 * The cookie a retrace request's swap record carries through the shared
 * scanout: the tag in the low half, the context table entry the request
 * arrived under plus one (0 for none) in bits [39:32], and bit 62 to tell
 * it from the DMA engine's cookies (sgi-kona-dma.h).
 */
#define SGI_KONA_SWAP_COOKIE_FLAG (1ULL << 62)
#define SGI_KONA_SWAP_COOKIE(slot, tag) \
    (SGI_KONA_SWAP_COOKIE_FLAG | ((uint64_t)((slot) + 1) << 32) | \
     (uint32_t)(tag))
#define SGI_KONA_SWAP_COOKIE_TAG(cookie)  ((uint32_t)(cookie))
#define SGI_KONA_SWAP_COOKIE_SLOT(cookie) \
    ((int)(((cookie) >> 32) & 0xff) - 1)

/*
 * The reason kona_ge_intr takes as the end of a context deactivate: it
 * wakes the swap sleeping in KONApcxswap and cancels that swap's 90 s
 * deactivate timeout (established from the kernel).
 */
#define SGI_KONA_REASON_CONTEXT_DEACTIVATED 0x101

/*
 * The graphics contexts the model tells apart. The driver gives each
 * context a 64 KiB save area, named by 0x1fd on its first activation and
 * by 0x1f9 on every later one as a page word and a tagged offset word, and
 * a 16 KiB return-data page, registered by the 0x1f2 after its 0x1fd
 * only. An entry is keyed by the save area as sent: save_page the page
 * word, save_offset the offset word's low twelve bits. return_page is the
 * page its 0x1f2 registered, valid only while return_page_valid is set,
 * and last_used orders the entries for eviction.
 *
 * Approximated: eight entries is the model's choice, not a pipe limit,
 * which is unknown. When all eight are in use a new context takes the
 * entry of the one activated longest ago, logged; a later restore of the
 * forgotten context is logged too and answers nowhere.
 */
#define SGI_KONA_CONTEXTS        8
#define SGI_KONA_CONTEXT_NONE    (-1)

/*
 * The words the model leaves in a context's save area when 0x1fc switches
 * it out. KONApcxswap reads word 0 back as 0x1f9's first argument and
 * sends word 2 as the token of kona_act_pcx's last packet, which carries
 * one 0; no driver code reads any other word. What the pipe writes there
 * is unknown. Word 0 is the model's own marker, ASCII "KONA", not a value
 * from any hardware. Word 2 is 0x172, a guess: every kernel sender of
 * 0x172 follows it with one 0, and the pipe's command-name table calls it
 * a no-op.
 */
#define SGI_KONA_SAVE_AREA_MARKER 0x4b4f4e41
#define SGI_KONA_SAVE_AREA_TOKEN  0x172

typedef struct SGIKonaContext {
    bool valid;
    uint32_t save_page;
    uint32_t save_offset;
    bool return_page_valid;
    uint64_t return_page;
    uint64_t last_used;
} SGIKonaContext;

/*
 * Where a context table entry's GL context draws, beside the entry.
 *
 * The window comes from the restore packets kona_act_pcx sends with a
 * context it has marked stale: 0x1f3 with eleven words and 0x1ee with
 * three. The traced run placed a 128x128 window at X position 256, 256
 * on the 1280x1024 screen and saw 0x1f3 0x100 0x280 0x05670080
 * 0x05670080 0x100 0x280 0x80 0x80 0 0xd 0 and 0x1ee 0x100 0x280 0.
 * Inferred: 0x1f3's words 0 and 1 are the window's left column and its
 * lower edge counted up from the bottom line, which is where a colour
 * plane's rows count from, and words 6 and 7 its width and height. The X
 * server filled the same window's background from (256, 256) to (384,
 * 384) with rows counted down, and 1024 - 384 is 640, 0x280. The other
 * reading, 0x100 the row counted down and 0x280 half the screen's width,
 * fits that one window equally, since both its coordinates are 256; a
 * run with the window at two different coordinates would tell them
 * apart. 0x1ee's first two words repeat the corner and set it alone.
 * Words 2 to 5 and 8 to 10 of 0x1f3 and the last word of 0x1ee are
 * unknown and unused. placed is 0 until the first 0x1f3, and the context
 * then draws nothing.
 *
 * double_buffered is the double-buffer word of the context's buffer
 * configuration (token 1). A single-buffered context draws into colour
 * bank 0, the bank the X server and the textport draw into. A
 * double-buffered one draws into the bank its window does not show:
 * displayed_bank is the one its window shows, exchanged at the retrace
 * that honours the kernel's swap request for the context.
 */
typedef struct SGIKonaGLWindow {
    uint32_t placed;            /* 0 or 1 */
    int32_t x, y;               /* lower-left pixel, plane coordinates */
    uint32_t width, height;
    uint32_t double_buffered;   /* 0 or 1 */
    uint32_t displayed_bank;    /* 0 or 1 */
} SGIKonaGLWindow;

/* Native return state follows its GL context and accepted attribute groups. */
typedef struct SGIKonaGLReturnState {
    uint64_t depth_range[2];
    uint64_t clear_depth;
    uint32_t read_buffer;
    bool read_buffer_unsupported;
} SGIKonaGLReturnState;

/*
 * Pixel-store state remains available to image transfers. Small RGB replies
 * use dense rows regardless of pack state. Complete visual records and read
 * bindings authorize their source separately from the renderer's state.
 */
typedef struct SGIKonaGLPipe {
    uint32_t unpack_alignment;
    uint32_t unpack_row_length;
    uint32_t pack_alignment;
    uint32_t pack_row_length;
    /*
     * The buffer configuration's depth word: the bits of depth buffer the
     * context's visual asked for, 0 for none (see sgi_kona_gl_draw()).
     */
    uint32_t depth_bits;
    SGIKonaGLReturnState returns;
    uint32_t draw_config[10];
    uint32_t read_config[10];
    bool draw_config_received;
    bool draw_config_valid;
    bool read_config_received;
    bool read_config_valid;
    bool read_compatible;
    bool read_binding_unknown;
    uint32_t read_binding_word;
} SGIKonaGLPipe;

/* The copies one context's attribute stack holds, OpenGL 1.2.1's least. */
#define SGI_KONA_GL_ATTRIB_DEPTH 16

/*
 * The most words of a GL data run the model keeps: twelve for each of
 * the eight lights glPopAttrib can return, more than the 32 stipple rows.
 */
#define SGI_KONA_GL_DATA_MAX 96

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
 * (values 3, 2), its EEPROM-check sequence (values 1, 0), and the
 * kernel driver's own KONAinitialize, which writes 1 then 0 here,
 * reading back after each, before its SGI_KONA_INT_ENABLE and
 * SGI_KONA_RESET_ARG writes. This model interprets none of these
 * writers' values.
 */
#define SGI_KONA_RESET_STROBE 0x800300

/*
 * Plain stored field: written 1 before the firmware's fallback EEPROM
 * scan. This model interprets neither the write nor its readback.
 */
#define SGI_KONA_EEPROM_ENABLE 0x800308

/*
 * The indirect port into a pipe-internal 32-bit address space: the
 * address port selects a word, the data port reads or writes it, and
 * SGI_KONA_PORT_WRITE_NEXT writes it and advances the address by 4.
 * The firmware and driver read the identity EEPROM through it; a
 * user-space loader writes the pipe's firmware through it.
 *
 * Only EEPROM address SGI_KONA_EEPROM_CHECKED_ADDR is established (value
 * SGI_KONA_EEPROM_CHECKED_VALUE); it answers on top of whatever the
 * internal space holds there. Every other EEPROM address reads zero.
 *
 * Approximated: SGI_KONA_EEPROM_DATA does not advance the address. Every
 * reader selects a new address before each access, so no evidence says
 * whether the hardware advances; the model's choice is checked by qtest,
 * not against hardware. SGI_KONA_PORT_WRITE_NEXT's advance is what the
 * loader depends on (its sections abut at address plus four times their
 * word count), inferred for the hardware.
 */
#define SGI_KONA_EEPROM_ADDR 0x8003e8
#define SGI_KONA_EEPROM_DATA 0x8003f0
#define SGI_KONA_PORT_WRITE_NEXT 0x8003f8
#define SGI_KONA_EEPROM_CHECKED_ADDR  0x87ff1c
#define SGI_KONA_EEPROM_CHECKED_VALUE 2

/*
 * The two internal ranges the loader writes: the ARM's reset code at
 * address 0 (0x0-0x2b, eleven words) and its image, data and the
 * geometry-engine microcode from 0x4000000 through 0x414e62f. Backed with
 * plain storage the model never executes or interprets; writes anywhere
 * else in the internal space are traced and dropped, reads return zero.
 */
#define SGI_KONA_ARM_LOW_BASE   0x0
#define SGI_KONA_ARM_LOW_WORDS  (0x2c / 4)
#define SGI_KONA_ARM_HIGH_BASE  0x4000000
#define SGI_KONA_ARM_HIGH_WORDS ((0x414e630 - SGI_KONA_ARM_HIGH_BASE) / 4)

/*
 * Internal address the loader writes 1 before its first data word and 0
 * after its last (established from a trace). kpm's register table names
 * it the input control unit's control register, bit 0 ARM access, so the
 * reading is that bit 0 opens the ARM's memory to host writes (inferred
 * from that table, not from a hardware description). Stored with no
 * behaviour. Its 1-to-0 edge is where the model traces the load record
 * below.
 */
#define SGI_KONA_ICU_CONTROL 0x200050

/*
 * The load record: one section per SGI_KONA_EEPROM_ADDR select followed
 * by SGI_KONA_PORT_WRITE_NEXT writes, each with its start address and
 * word count, traced with the SHA-256 of its stored words when
 * SGI_KONA_ICU_CONTROL goes from 1 to 0. The stock loader writes ten
 * sections; a larger load keeps counting but records only the first
 * SGI_KONA_LOAD_SECTIONS_MAX.
 */
#define SGI_KONA_LOAD_SECTIONS_MAX 32

/*
 * The watchdog activity register. While a driver timeout is pending,
 * kona_callout reads it every 500 ms, treats bit 0x4 as transaction-bus
 * or ARM activity, and writes 4 back (established). The model sets bit
 * 0x4 whenever it consumes a command-port or load word, and a write with
 * bit 0x4 set clears it: both inferred from the "no TBUS or ARM activity"
 * message, not from a hardware description. The value does not affect
 * bring-up while the driver's kona_full_timeouts keeps its default of 1.
 * Other written bits (kona_die writes 8, kona_video_intr 1) have no
 * modeled meaning and stay on the unimplemented-write log. The
 * activity-latch qtest checks the model's rule; what would check it
 * against the driver is a guest run with kona_full_timeouts set to 0,
 * which must not log "Pipe hung", and against hardware a read of a real
 * pipe's register after it has been idle.
 */
#define SGI_KONA_ACTIVITY 0x8003a8
#define SGI_KONA_ACTIVITY_BUS (1u << 2)

/*
 * Written 5 and 1 by the loader after the load, never read. kpm's
 * register table names +0x8003b0 a 4-bit transaction FIFO high-water mark
 * and +0x800328 ARM unstall; both names are inferred from that table.
 * Stored and traced, no behaviour: in particular the model does not wait
 * for the unstall before answering commands.
 */
#define SGI_KONA_LOADER_3B0 0x8003b0
#define SGI_KONA_LOADER_328 0x800328

/*
 * The XG map RAM: a per-entry mapping table kona_mapram_init() probes at
 * attach and kona_mapram_map() later fills for ordinary page mappings.
 * This model backs the whole
 * widget +0x100000-+0x1fffff window with SGI_KONA_MAPRAM_ENTRIES 32-bit
 * words that store and return all 32 bits, cleared at reset, so every
 * entry count SGI_KONA_PROBE's bits [5:3] could select stays inside it.
 * The model does not interpret entry contents: no DMA path here reads
 * them yet.
 *
 * Two parts of that policy are approximations, not hardware facts. An
 * entry's physical width is unknown: the driver's own test values use
 * only bits [17:0] and 28, so a narrower real entry would pass it too,
 * and storing all 32 bits is this model's choice. The table's reset
 * contents are unknown: nothing reads an entry before writing it, and
 * clearing to zero is likewise a choice. What would settle each is a
 * register-level capture of a real
 * pipe reading back an entry written all ones, and of an entry read
 * after power-on. A guest boot with the table backed that prints no
 * "Bad XG MAPRAM entry" warning checks the policy is enough for the
 * driver, not that it matches the hardware.
 */
#define SGI_KONA_MAPRAM_BASE 0x100000
#define SGI_KONA_MAPRAM_ENTRIES ((1 * MiB) / sizeof(uint32_t))

#define SGI_KONA_PROBE_MAPRAM_COUNT_SHIFT 3
#define SGI_KONA_PROBE_MAPRAM_COUNT_MASK \
    (0x7u << SGI_KONA_PROBE_MAPRAM_COUNT_SHIFT)

/*
 * The PROM textport's own command port. The driver's kernel commands and
 * a graphics process's own commands arrive on this same port with no
 * leading sync word: a traced guest run of the X server's first open
 * saw every 0x1f1, 0x1f2, 0x1f8 and 0x1fd there.
 */
#define SGI_KONA_TEXTPORT_PORT 0xc10000

/*
 * The rest of the 16 KiB command page a graphics process maps: a store at
 * offset 0xc + 0x20 * t opens command t with the stored value as its
 * first argument, and SGI_KONA_COMMAND_DATA_SLOT, offset 0xc, carries
 * plain words like SGI_KONA_TEXTPORT_PORT itself.
 * sgi_kona_command_page_write() has what the model does with the rest of
 * each slot.
 */
#define SGI_KONA_COMMAND_PAGE_SIZE  0x4000
#define SGI_KONA_COMMAND_DATA_SLOT  0xc
#define SGI_KONA_COMMAND_SLOT_SHIFT 5
#define SGI_KONA_COMMAND_SLOT_MASK  0x1f
#define SGI_KONA_COMMAND_TOKEN_MASK 0x1ff

/*
 * Bit 28 of a command's page word marks a host physical page; without it
 * the page word is a device address translated through the XG map RAM.
 * Inferred from the driver's two uses (kona_alloc_bdata and KONApcxswap).
 */
#define SGI_KONA_PAGE_WORD_PHYSICAL (1u << 28)
#define SGI_KONA_PAGE_WORD_FRAME    0x0fffffffu

/*
 * An XG map RAM entry as the 6.5.30 driver's kona_mapram_map() writes
 * it: bits [31:28] = 0x2 (a 16 KiB page), bits [27:0] the 4 KiB frame
 * number of that page. The model translates only this encoding.
 */
#define SGI_KONA_MAPRAM_KIND_MASK   0xf0000000u
#define SGI_KONA_MAPRAM_KIND_16K    0x20000000u
#define SGI_KONA_MAPRAM_PAGE_SHIFT  14

/*
 * The kernel FIFO/command push and kick registers (SGI_KONA_PORT_FIFO,
 * hw/misc/sgi-kona-ports.h), wired to the DMA engine
 * (hw/misc/sgi-kona-dma.c), which frames its packets on their selectors
 * and reports any other word through SGICommandPortOps.unsupported.
 * sgi-kona-ports.h's own file-level
 * comment leaves the push register's aperture placement unresolved --
 * the driver's own citation is `+0x7c`, already claimed by
 * SGI_KONA_LOW_WINDOW_WRITE above, which this model resolves as a
 * distinct, disclosed approximation: a register-block-relative
 * `+0x80007c`, colliding with neither SGI_KONA_LOW_WINDOW_WRITE nor
 * SGI_KONA_TEXTPORT_PORT. The kick register is established directly: the
 * driver writes `0xc00108` with the literals 0x30000, 0x10000 and 0
 * around a packet as a bracket, not a word count.
 */
#define SGI_KONA_DMA_FIFO_PUSH 0x80007c
#define SGI_KONA_DMA_FIFO_KICK 0xc00108

/*
 * The textport's colour map: 256 entries, each a resolved 0x00RRGGBB host
 * pixel value. Command 2 (set color) selects an entry by index rather than
 * carrying a packed colour itself -- established from a real captured boot
 * trace, whose command-2 argument values run 0-255, the identical domain
 * command 3's own index argument uses. Command 3 (load colormap) stores an
 * entry; every drawing primitive (point, filled rectangle, bitmap blit)
 * resolves the current index through this table.
 */
#define SGI_KONA_COLORMAP_SIZE 256

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

/*
 * The gamma table the X gamma load (SGI_KONA_CMD_X_GAMMA) fills: three
 * components of 4,096 16-bit entries for the one channel the model
 * emulates, indexed by gamma-RAM address, component c at c * 0x1000. See
 * sgi_kona_gamma_open() for what is established and what approximated.
 */
#define SGI_KONA_GAMMA_COMPONENTS        3
#define SGI_KONA_GAMMA_COMPONENT_ENTRIES 0x1000
#define SGI_KONA_GAMMA_ENTRIES \
    (SGI_KONA_GAMMA_COMPONENTS * SGI_KONA_GAMMA_COMPONENT_ENTRIES)

/*
 * The most box words a run of data tokens may leave waiting for its X box:
 * the grammar's senders send one data token, of at most ten words, before
 * each X box. The bound is the model's; a run past it is dropped, logged.
 */
#define SGI_KONA_X_BOX_RUN_WORDS 32

/*
 * The X colour map the X load colour map (SGI_KONA_CMD_X_COLOR_MAP) fills,
 * by address: the stock X server loads 256 entries at 0x2d00 and single
 * entries after them. The table's size is the model's choice, covering
 * that window; SGI_KONA_X_COLOR_MAP_WINDOW is the base an X pixel's index
 * is added to (inferred, see sgi_kona_x_color()).
 */
#define SGI_KONA_X_COLOR_MAP_ENTRIES 0x4000
#define SGI_KONA_X_COLOR_MAP_WINDOW  0x2d00

/*
 * The depth of the X server's colour-index window, which the GC's
 * foreground packing depends on. The depth travels in the window's format
 * word, which reaches no command, so the model fixes the stock greeter's 8.
 */
#define SGI_KONA_X_INDEX_DEPTH 8

/*
 * The most rectangles an X clip list may carry; the bound is the model's,
 * and a longer list is logged and leaves drawing clipped to the screen.
 *
 * This list diverges from the shared SGIClipList (hw/display/sgi/
 * board-memory.h, section 2 of the graphics low-level design): that holds
 * up to 16 inclusive rectangles in plane coordinates, and this one keeps
 * the X server's words as sent, up to 64 boxes with an exclusive far
 * corner in X coordinates, y counting down. konPolyFillRectSolid sends
 * the list only with fewer than 13 rectangles, but the other callers of
 * kon_loadClipRects were not read for a bound, so the model leaves more
 * room than 16. Adopting the shared type would refuse lists of 17 to 64
 * that the model now applies, and convert every rectangle at load.
 */
#define SGI_KONA_X_CLIP_MAX_RECTS  64
#define SGI_KONA_X_CLIP_LIST_WORDS (1 + 4 * SGI_KONA_X_CLIP_MAX_RECTS)

/*
 * The X stipple cache: SGI_KONA_X_STIPPLE_ENTRIES patterns of at most
 * SGI_KONA_X_STIPPLE_MAX by SGI_KONA_X_STIPPLE_MAX bits, one word per
 * row. Both bounds are the model's choice: the grammar gives no cache
 * size, and the run's cursor stores entry 0x1f, 6 by 20.
 * SGI_KONA_X_STIPPLE_NONE is the load word that unloads the stipple.
 */
#define SGI_KONA_X_STIPPLE_ENTRIES 32
#define SGI_KONA_X_STIPPLE_MAX     32
#define SGI_KONA_X_STIPPLE_NONE    UINT32_MAX

/* The native framebuffer this increment publishes as a display surface. */
#define SGI_KONA_FB_WIDTH  1280
#define SGI_KONA_FB_HEIGHT 1024

/*
 * The field rate the scanout's vertical retrace runs at. The model's own
 * choice: the display-mode packet (0x1e6) names a timing-table index
 * whose meaning is not decoded, and no timing table for this pipe is
 * held, so the one mode the model presents gets a plain 60 Hz field.
 * A retrace request (0x1e7) completes at the next retrace, so the
 * period sets how long a swap waits; the scanout qtest checks the
 * period, and a decoded 0x1e6 index or a real pipe's timing table would
 * replace it.
 */
#define SGI_KONA_FIELD_HZ 60

/*
 * The pipe's board memory (hw/display/sgi/board-memory.c), device-backed,
 * and the planes over it. No capture gives the framebuffer's byte size:
 * the board memory-size values known from real boards are the colour-map
 * RAM and raster-manager texture sizes, neither of which is framebuffer.
 * So every size
 * here is the model's own, taken from the one display mode it presents:
 *
 *   front colour   1280x1024 32-bit words, little-endian x8r8g8b8
 *   back colour    the same, the second bank of a double-buffered GL
 *                  context (see SGIKonaGLWindow)
 *   front tag      one byte per front pixel, SGI_KONA_TAG_*
 *   back tag       the same for the back bank
 *   list           SGI_KONA_LIST_ROW_BYTES-byte rows of words for display
 *                  lists resident in the pipe, "list-memory-size" bytes
 *   depth          one internal 24-bit depth word per pixel
 *   stencil        one internal byte per pixel, shared by colour banks
 *
 * Colour planes are bottom-origin: plane row y is textport wire row y,
 * which counts upward from the bottom display line. The tag says how a
 * colour word resolves: a textport pixel holds a textport colour-map index
 * and an X pixel an X colour-map index, each resolved through its map at
 * scanout; a DMA pixel holds direct RGB. The front, back and list sizes,
 * the tag encoding and the byte order are approximations: a capture of a
 * real pipe's board-inventory bytes, or its pixel readback, would replace
 * them.
 */
#define SGI_KONA_TAG_DIRECT      0
#define SGI_KONA_TAG_COLOR_INDEX 1
/* An X server pixel: an index into the X colour map, see sgi_kona_x_color(). */
#define SGI_KONA_TAG_X_INDEX     2

#define SGI_KONA_LIST_ROW_BYTES  4096
#define SGI_KONA_LIST_DEFAULT    (1 * MiB)
#define SGI_KONA_LIST_MAX        (64 * MiB)

struct SGIKonaState {
    SGIXioBoardState parent_obj;

    /* Disposable host execution state; canonical planes alone migrate. */
    SGIRenderer *renderer;
    bool host_gpu;
    bool renderer_failed;
    uint64_t renderer_generation;
    uint64_t renderer_draws;
    uint64_t renderer_fallbacks;

    /*
     * Set by build_widget(), before this board re-parents: the XIO
     * fabric proxy sgi_xio_crossbow_link_claim_fabric() claims. Required;
     * a board realized without it refuses.
     */
    MemoryRegion *fabric;

    /*
     * fabric's own address space, initialized in finish_realize() once
     * fabric is claimed: the DMA engine's guest-memory transfers
     * (hw/misc/sgi-kona-dma.c) are this board's first feature needing
     * dma_memory_read()/dma_memory_write() rather than the register file's
     * own MMIO dispatch, matching hw/misc/sgi-mgras.h's own fabric_as
     * precedent.
     */
    AddressSpace fabric_as;

    /*
     * Approximated: the six-word DMA packet (see hw/misc/sgi-kona-dma.c)
     * carries no destination-widget field at all, unlike the MGRAS, whose
     * widget number arrives in a separate register. fabric_hub_widget
     * instead names a single fixed XIO widget the DMA engine's packets and
     * the board's host-memory writes always target; this model cannot
     * express a topology where the engine's destination Hub varies by
     * descriptor or by guest-address node. The machine sets it to the
     * board's own node's Hub; a -device board sets it explicitly.
     */
    uint32_t fabric_hub_widget;

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
    uint32_t int_enable;
    uint32_t int_cause;
    uint32_t mailbox_queue[SGI_KONA_MAILBOX_MAX_WORDS];
    uint32_t mailbox_len;
    uint32_t mailbox_pos;
    /*
     * Bit i set: mailbox_queue[i] is a DMA completion word, which a write
     * of the mailbox bit to SGI_KONA_INT_CAUSE consumes (see that
     * register's write).
     */
    uint32_t mailbox_completions;
    uint32_t reset_strobe;
    uint32_t eeprom_enable;
    uint32_t eeprom_addr;
    uint32_t low_window_write;

    /* Plain storage the geometry-engine interrupt is addressed from. */
    uint32_t int_dest_upper;
    uint32_t int_dest_lower;
    uint32_t int_vectors;

    /*
     * The firmware load: see SGI_KONA_ARM_LOW_BASE and the load record's
     * header comment. arm_high is allocated at finish_realize(), like
     * map_ram below, and arm_high_count carries its fixed length through
     * migration. load_sections counts every section opened since the
     * last trace of the record, including any past
     * SGI_KONA_LOAD_SECTIONS_MAX; load_section_open is false after a
     * select until the first advancing write opens the next section.
     */
    uint32_t arm_low[SGI_KONA_ARM_LOW_WORDS];
    uint32_t *arm_high;
    uint32_t arm_high_count;
    uint32_t icu_control;
    bool load_section_open;
    uint32_t load_sections;
    uint32_t load_section_addr[SGI_KONA_LOAD_SECTIONS_MAX];
    uint32_t load_section_words[SGI_KONA_LOAD_SECTIONS_MAX];
    uint32_t loader_3b0;
    uint32_t loader_328;

    /* SGI_KONA_ACTIVITY_BUS, latched since the last write that cleared it. */
    bool activity;

    /*
     * The XG map RAM: see the header comment on SGI_KONA_MAPRAM_BASE.
     * Allocated at finish_realize() like arm_high above, for the same
     * non-allocating VMSTATE_VARRAY_UINT32() reason: map_ram_count is
     * fixed at SGI_KONA_MAPRAM_ENTRIES, never guest-shrunk, and exists
     * solely to carry that fixed length through migration.
     */
    uint32_t *map_ram;
    uint32_t map_ram_count;
    /*
     * Host-only trace bookkeeping, not guest state: counts every access
     * this window sees so sgi_kona_mapram_trace_access() can trace only
     * the first access and each power-of-two milestone after it, instead
     * of one line per entry -- kona_mapram_init()'s own attach-time sweep
     * alone touches up to SGI_KONA_MAPRAM_ENTRIES entries twice. Not
     * migrated, like test_pixel_x/test_pixel_y below: a live trace's own
     * milestone count restarting after a migration is harmless.
     */
    uint32_t map_ram_accesses;

    /* Increment 2: the textport command port and its parser state. */
    SGICommandPort textport;
    bool textport_expect_sync;

    /*
     * The kernel commands that share SGI_KONA_TEXTPORT_PORT, framed by
     * sgi_kona_command_tokens[] in hw/misc/sgi-kona-ports.c. reason_page
     * is the guest physical address 0x1f1 registered, valid only while
     * reason_page_valid is set; return_page is the page the pipe answers
     * on, the same way: the one 0x1f2 last registered, or the current
     * context's.
     */
    SGICommandPort kernel_port;
    /*
     * The GL client's commands, framed by sgi_kona_gl_tokens[] on a port
     * of their own because several of their numbers are the textport's.
     * gl_grammar is set by the first context activation and cleared by
     * reset. gl_sync_held says a 1 arrived between packets in that
     * grammar, and the next word decides whether it was the textport's
     * sync word or the buffer configuration. See sgi_kona_textport_push().
     * gl_token_high is the top half of the word that opened the GL
     * command being framed, the data of a packed attribute form.
     */
    SGICommandPort gl_port;
    bool gl_grammar;
    bool gl_sync_held;
    uint32_t gl_token_high;
    bool reason_page_valid;
    uint64_t reason_page;
    bool return_page_valid;
    uint64_t return_page;

    /*
     * The credit register's parts beyond the framers: credit_config is
     * what the driver wrote to SGI_KONA_CREDIT_CONFIG, and credit_pending
     * the words of framed packets whose CONSUMED completion has not come
     * back. The model consumes a packet as it frames it, so credit_pending
     * is zero whenever an access ends and is not migrated.
     */
    uint32_t credit_config;
    uint32_t credit_pending;

    /*
     * The processor interface's graphics credits (hw/mips/sgi/pi-gfx.h).
     * pi is the "pi" link the machine sets to the node controlling the
     * board's slot. pi_held counts the processor stores the interface
     * charged a credit for whose words a command-port framer still holds:
     * their credits go back with the packet's last word (see
     * sgi_kona_pi_credits()). pi_unlinked_logged is host-only: a board
     * with no link has logged that it returns nothing.
     */
    Object *pi;
    uint32_t pi_held;
    bool pi_unlinked_logged;

    /*
     * SGI_KONA_RETRACE_TAG as it reads, and whether the retrace now being
     * run completed a tag, which the scanout's retrace() hook turns into
     * the video cause bit. retrace_completed is set and cleared inside one
     * retrace and is not migrated.
     */
    uint32_t retrace_tag;
    bool retrace_completed;
    bool video_delivered;

    /*
     * The contexts 0x1fd and 0x1f9 have named (see SGIKonaContext), the
     * index of the current one or SGI_KONA_CONTEXT_NONE, whether it is a
     * first activation still waiting for its 0x1f2, and the counter that
     * stamps each activation's last_used.
     */
    SGIKonaContext contexts[SGI_KONA_CONTEXTS];
    int32_t context_current;
    bool context_page_pending;
    uint64_t context_clock;

    /*
     * Each context table entry's GL context (hw/display/sgi/gl-context.c),
     * keyed by its index as hw_id, and its window and buffers. A GL
     * client's drawing commands act on the current entry's.
     */
    SGIGLContext gl[SGI_KONA_CONTEXTS];
    SGIKonaGLWindow gl_window[SGI_KONA_CONTEXTS];
    SGIKonaGLPipe gl_pipe[SGI_KONA_CONTEXTS];
    /*
     * Each entry's attribute stack (sgi_kona_gl_push_attrib()): entry i's
     * copies at gl_attrib[i * SGI_KONA_GL_ATTRIB_DEPTH] on, how many it
     * holds, and the mask each was pushed with.
     */
    SGIGLContext gl_attrib[SGI_KONA_CONTEXTS * SGI_KONA_GL_ATTRIB_DEPTH];
    SGIKonaGLReturnState
        gl_return_attrib[SGI_KONA_CONTEXTS * SGI_KONA_GL_ATTRIB_DEPTH];
    uint32_t gl_attrib_mask[SGI_KONA_CONTEXTS * SGI_KONA_GL_ATTRIB_DEPTH];
    uint32_t gl_attrib_depth[SGI_KONA_CONTEXTS];

    /*
     * A video command (SGI_KONA_CMD_VIDEO) still collecting its data words
     * and checksum after the command port framed its number: video_have
     * counts the data words so far, video_first is the first of them (the
     * one the trace and log show), and video_tail_sum the sum of the rest,
     * modulo 2^32, which with the number and video_first gives the
     * checksum a word must match. Only the first word and a sum are kept:
     * no command the model answers reads its data. See
     * sgi_kona_video_word().
     */
    bool video_open;
    uint32_t video_command;
    uint32_t video_have;
    uint32_t video_first;
    uint32_t video_tail_sum;

    /*
     * The X colour map, and a load (SGI_KONA_CMD_X_COLOR_MAP) still taking
     * its entries: color_map_start and color_map_count are its first two
     * words, color_map_remaining the entries still to come and
     * color_map_outside those so far past the table. See
     * sgi_kona_color_map_word().
     */
    uint32_t color_map_start;
    uint32_t color_map_count;
    uint32_t color_map_remaining;
    uint32_t color_map_outside;
    uint32_t x_color_map[SGI_KONA_X_COLOR_MAP_ENTRIES];

    /*
     * The gamma table (SGI_KONA_GAMMA_ENTRIES), kept for the scanout to
     * apply later; nothing reads it yet. An X gamma load still taking its
     * entries: gamma_start and gamma_count are the load's first two
     * words, gamma_remaining the entries still to come, and gamma_outside
     * the entries so far whose address the table does not cover. See
     * sgi_kona_gamma_word().
     */
    uint16_t gamma[SGI_KONA_GAMMA_ENTRIES];
    uint32_t gamma_start;
    uint32_t gamma_count;
    uint32_t gamma_remaining;
    uint32_t gamma_outside;

    /*
     * An X cursor load (SGI_KONA_CMD_X_CURSOR) still taking its data
     * words: the count still to come. See sgi_kona_cursor_word().
     */
    uint32_t cursor_remaining;
    bool cursor_state_loaded; /* migration subsection was present */
    uint32_t cursor_stage_words;
    uint32_t cursor_stage_entry;
    uint32_t cursor_stage[260];
    uint32_t cursor_images[64 * 260];
    uint8_t cursor_valid[64];
    uint32_t cursor_selector;
    uint32_t cursor_channel;
    uint32_t cursor_offset;
    uint32_t cursor_position;
    uint32_t cursor_mode;

    /*
     * A GL texture table (SGI_KONA_CMD_GL_TEX_TABLE) still taking its data
     * words: the count still to come. See sgi_kona_gl_tex_table_word().
     */
    uint32_t gl_tex_table_remaining;

    /*
     * A GL data run (see sgi_kona_gl_data_word()): the command it belongs
     * to, the words still to come, the words so far and the first
     * SGI_KONA_GL_DATA_MAX of them.
     */
    uint32_t gl_data_token;
    uint32_t gl_data_remaining;
    uint32_t gl_data_have;
    uint32_t gl_data[SGI_KONA_GL_DATA_MAX];

    /*
     * The X server's graphics context, the 13 words of its last X load GC
     * (SGI_KONA_CMD_X_GC), and the box words of a run of data tokens
     * waiting for the X box that ends it: x_boxes_have words of x_boxes.
     * See sgi_kona_x_fill_box() and sgi_kona_x_box_data().
     */
    uint32_t x_gc[SGI_KONA_X_GC_WORDS];
    uint32_t x_boxes[SGI_KONA_X_BOX_RUN_WORDS];
    uint32_t x_boxes_have;

    /*
     * The last X load position (SGI_KONA_CMD_X_POSITION), recorded and not
     * applied; the clip list: x_clip_count rectangles of four words in
     * x_clip_rects; and the words of a clip list waiting for the X load
     * clip that ends it, x_clip_run_have of x_clip_run. See
     * sgi_kona_x_load_clip().
     */
    uint32_t x_position[SGI_KONA_X_POSITION_WORDS];
    uint32_t x_clip_count;
    uint32_t x_clip_rects[4 * SGI_KONA_X_CLIP_MAX_RECTS];
    uint32_t x_clip_run[SGI_KONA_X_CLIP_LIST_WORDS];
    uint32_t x_clip_run_have;

    /*
     * The X load scissor (SGI_KONA_CMD_X_SCISSOR) as sent, x1, y1, width
     * and height, applied with the clip list while x_scissor_valid is set.
     * See sgi_kona_x_load_scissor().
     */
    bool x_scissor_valid;
    uint32_t x_scissor[SGI_KONA_X_SCISSOR_WORDS];

    /*
     * The X stipple cache: entry e is x_stipple_width[e] by
     * x_stipple_height[e] bits, zero width for an entry holding no
     * pattern, its rows at x_stipple_rows[e * SGI_KONA_X_STIPPLE_MAX], and
     * its origin from the last rotate. x_stipple_loaded is the entry the
     * fills are masked with, or SGI_KONA_X_STIPPLE_NONE. A store still
     * taking its rows: its entry, width and height as sent, and the rows
     * still to come. See sgi_kona_x_store_stipple().
     */
    uint32_t x_stipple_width[SGI_KONA_X_STIPPLE_ENTRIES];
    uint32_t x_stipple_height[SGI_KONA_X_STIPPLE_ENTRIES];
    int32_t x_stipple_origin_x[SGI_KONA_X_STIPPLE_ENTRIES];
    int32_t x_stipple_origin_y[SGI_KONA_X_STIPPLE_ENTRIES];
    uint32_t x_stipple_rows[SGI_KONA_X_STIPPLE_ENTRIES *
                            SGI_KONA_X_STIPPLE_MAX];
    uint32_t x_stipple_loaded;
    uint32_t x_stipple_store_entry;
    uint32_t x_stipple_store_width;
    uint32_t x_stipple_store_height;
    uint32_t x_stipple_store_remaining;

    /* Complete read state is valid only within one context activation. */
    bool x_read_gc_valid;
    bool x_read_setup_valid;

    /*
     * The last X pixel setup (SGI_KONA_CMD_X_PIXEL_SETUP), and a pixel
     * write open until its X pixel finish: its destination, bits per
     * pixel, row length in words, total and remaining data words, and the
     * previous data word of the row, which a pixel may straddle. See
     * sgi_kona_x_pixel_write().
     */
    uint32_t x_pixel_setup[SGI_KONA_X_PIXEL_SETUP_WORDS];
    bool x_pixel_open;
    int32_t x_pixel_x;
    int32_t x_pixel_y;
    uint32_t x_pixel_bpp;
    uint32_t x_pixel_stride;
    uint32_t x_pixel_words;
    uint32_t x_pixel_remaining;
    uint32_t x_pixel_prev;

    /* Pending source pixels stay separate from canonical board storage. */
    SGIBoardMemory x_write_mem;
    bool x_write_snapshot;
    bool x_write_refused;
    uint32_t x_write_gc[SGI_KONA_X_GC_WORDS];
    uint32_t x_write_setup[SGI_KONA_X_PIXEL_SETUP_WORDS];
    uint32_t x_write_clip_count;
    uint32_t x_write_clip_rects[4 * SGI_KONA_X_CLIP_MAX_RECTS];
    bool x_write_scissor_valid;
    uint32_t x_write_scissor[SGI_KONA_X_SCISSOR_WORDS];
    uint32_t x_write_stipple_width;
    uint32_t x_write_stipple_height;
    int64_t x_write_stipple_x;
    int64_t x_write_stipple_y;
    uint32_t x_write_stipple_rows[SGI_KONA_X_STIPPLE_MAX];

    /*
     * Stage 4: the kernel FIFO's framebuffer DMA engine. See
     * include/hw/misc/sgi-kona-dma.h for the wire format and its
     * established/inferred/approximated field breakdown.
     */
    SGIKonaDma dma;
    /*
     * An index into colormap[], not a packed pixel value; see the header
     * comment on SGI_KONA_COLORMAP_SIZE.
     */
    uint32_t draw_color;
    uint32_t colormap[SGI_KONA_COLORMAP_SIZE];
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
     * The textport's raster position: established from the IO6 PROM's
     * kona_tp_raster_pos routine as command 4's own two arguments (x, y),
     * set once before a run of
     * bitmap blits and read back by none of the firmware's own code --
     * only this model's bitmap placement consumes it. Command 4 stores
     * these and draws nothing; a completed bitmap blit advances them by
     * its own header's advance fields. Persists across bitmap_reset()
     * (a bitmap transfer's own header/payload state), unlike the fields
     * above.
     */
    int32_t raster_x;
    int32_t raster_y;

    /*
     * Board memory and its planes: see the header comment on
     * SGI_KONA_TAG_DIRECT. list_memory_size is the "list-memory-size"
     * property, fixed at realize.
     */
    SGIBoardMemory mem;
    /* Independent desktop planes preserve the original memory layout. */
    SGIBoardMemory desktop_mem;
    bool desktop_state_used;
    uint32_t desktop_map[256];
    bool desktop_map_valid[256];
    /* Diagnostic suppression is host state, rebuilt after reset/load. */
    bool desktop_map_reported[256];
    bool desktop_id_reported;
    uint32_t list_memory_size;
    uint32_t plane_front;
    uint32_t plane_back;
    uint32_t plane_front_tag;
    uint32_t plane_back_tag;
    uint32_t plane_list;
    uint32_t plane_depth;
    uint32_t plane_stencil;

    /*
     * Only to load a version 6 stream from before the board memory: it
     * carries the framebuffer, resolved, top display line first, in the
     * main section. A stream saved now carries a zero count and no
     * pixels here; post_load moves a loaded framebuffer into the front
     * plane and frees it.
     */
    uint32_t *legacy_framebuffer;
    uint32_t legacy_framebuffer_count;
    bool legacy_framebuffer_dirty;

    /*
     * The shared scanout (hw/display/sgi/scanout.c): the console, the
     * presented frame, its dirty lines and the retrace. See
     * sgi_kona_resolve_row() for what a line resolves to.
     */
    SGIScanout scanout;

    /*
     * Host-only qtest introspection coordinates for x-test-pixel. Not
     * guest state and not migrated, matching hw/misc/sgi-mgras.c's own
     * test_pixel_x/test_pixel_y precedent.
     */
    uint32_t test_pixel_x;
    uint32_t test_pixel_y;

    /*
     * Host-only qtest introspection of board memory: x-test-plane-word
     * reads the word of plane test_plane at (test_pixel_x, test_pixel_y)
     * in plane coordinates, through the shared plane load. Not migrated.
     */
    uint32_t test_plane;

    /*
     * Host-only qtest introspection of the gamma table: x-test-gamma
     * reads the entry at gamma-RAM address test_gamma_addr. Not guest
     * state and not migrated, like test_pixel_x/test_pixel_y.
     */
    uint32_t test_gamma_addr;

    /*
     * Host-only qtest introspection of the X graphics context: x-test-gc
     * reads word test_gc_index of it. Not migrated, like the above.
     */
    uint32_t test_gc_index;
};

/*
 * The front bank's pixel at plane (x, y), resolved by its tag, and a
 * direct-colour store there; hw/misc/sgi-kona-dma.c moves pixels through
 * these. See sgi_kona_fb_read() for the resolve.
 */
uint32_t sgi_kona_fb_read(SGIKonaState *s, int x, int y);

/*
 * The X server's image write by DMA: whether the open X pixel write waits
 * for its rows by DMA, and a DMA packet's lines fed to it; see the
 * definitions.
 */
bool sgi_kona_x_pixel_dma_waiting(SGIKonaState *s);
bool sgi_kona_x_image_dma(SGIKonaState *s, uint64_t io, uint32_t lines,
                          uint32_t per_line, uint64_t pitch);

/* A plain command-page word through the page's decoder. */
void sgi_kona_command_word(SGIKonaState *s, uint32_t word);

/*
 * The head's completions, from the DMA engine and the command ports: see
 * the function's comment for what each does to the credit register, the
 * mailbox and the cause register.
 */
void sgi_kona_publish(void *opaque, const SGICompletion *c);

/* A command port framed a packet of `words` words; see the definition. */
void sgi_kona_port_consumed(SGIKonaState *s, uint32_t words);
void sgi_kona_fb_store_direct(SGIKonaState *s, int x, int y, uint32_t word);

#endif
