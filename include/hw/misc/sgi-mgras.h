/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_MGRAS_H
#define HW_MISC_SGI_MGRAS_H

#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_SGI_MGRAS "sgi-mgras"
OBJECT_DECLARE_SIMPLE_TYPE(SGIMgrasState, SGI_MGRAS)

#define SGI_MGRAS_MMIO_SIZE (2 * MiB)
#define SGI_MGRAS_WIDTH 1280
#define SGI_MGRAS_HEIGHT 1024

/*
 * GE11 microcode is uploaded to one of two engine slots, each with its own
 * data and control port, then verified by reading it back through the two
 * shared readback ports below. Root-caused by disassembling the retained
 * disk's own rebuilt kernel (MgrasDownloadGE11Ucode,
 * docs/plans/si-graphics-widget-wiring-handoff-20260916.md, "GE microcode
 * download failure"): a control write of 0x200000 or 0x200001 resets that
 * slot's write and read pointers and starts a new load, with bit 0
 * selecting which of two verify-pass alignments the read pointer's unit
 * index maps to; each data-port write, while the load is active, appends
 * one flat microcode word (bounded at this many). Any other control write
 * -- 0x40038 among them -- ends the load (further data-port writes are then
 * opaque register-select/value traffic, not microcode) and, if bit 31 is
 * set, rewinds the read pointer to reread from the start of the current
 * pass.
 *
 * Each verify step reads the discard port (GE_READ_2) then the compare
 * port (GE_READ_1). Confirmed against the driver's own comparison loop:
 * the k-th compare-port read returns flat word w[2k] in the first
 * (even/0x200000) pass and w[2k-1] in the second (odd/0x200001) pass --
 * w[-1] reads as 0 -- and only the compare-port read advances the read
 * pointer. What the discard port actually carries on real hardware is not
 * observable through this driver (it never checks the value); this device
 * returns the pass's other word from the same pair as its own approximation,
 * not a confirmed wire contract.
 */
#define SGI_MGRAS_GE_WORDS (59392 * 3)
#define SGI_MGRAS_GE_SLOT_A 0
#define SGI_MGRAS_GE_SLOT_B 1
#define SGI_MGRAS_GE_DATA_A 0x058048
#define SGI_MGRAS_GE_CTRL_A 0x05804c
#define SGI_MGRAS_GE_DATA_B 0x058040
#define SGI_MGRAS_GE_CTRL_B 0x058044
#define SGI_MGRAS_GE_READ_1 0x05822c
#define SGI_MGRAS_GE_READ_2 0x058230

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
 * A second word the kernel driver's own reset routine writes once it
 * observes the readiness field at 0x020100 settled, live-traced through a
 * boot that actually reaches this code path -- next to the existing
 * startup-configuration register at 0x011000. Its own field meaning was
 * never captured, only that this driver writes it once during reset and
 * expects the access to succeed, so it stores opaque state, the same
 * treatment already given to the generic widget control register above.
 */
#define SGI_MGRAS_RESET_ARG 0x011008

/*
 * The board-population/reset-control word for the graphics rasterizer
 * pipeline, disassembled from the kernel driver's own board-bring-up and
 * inventory routines (MgrasReset, MgrasSetTiming, MgrasSetGenlock, the GE
 * microcode download's reset pulse, and MgrasGatherInfo). Write and read
 * carry different meanings, the usual shape for a combined control/status
 * word: writes to the top byte select a reset or clock line (the exact bit
 * assignments were not captured, so writes still store as opaque state);
 * a read returns a fixed population code MgrasGatherInfo decodes as two
 * independent 2-bit TRAM-daughtercard fields (bits 25:24 for the RA board,
 * bits 27:26 for the RB board), where the driver treats field value 3 as
 * "absent" and any other value as "4 TRAMs present". This project's own
 * retained evidence for the SI board class this device models
 * (docs/research/graphics/configurations.md) gives it no TRAM daughtercard
 * on either board, so the read side reports both fields absent (0xf in
 * that nibble) rather than echoing back a write-side reset bit that was
 * never meant to double as a population code.
 */
#define SGI_MGRAS_RESET_CONTROL 0x072408

/*
 * The GE (geometry engine) and board-revision inventory word, read once by
 * MgrasGatherInfo and never written by the kernel driver. The top byte
 * packs three fields: bits 1:0 the number of GEs populated, bits 4:2 a
 * GD/board revision code, bits 6:5 compared against 1 to mean "single-RE
 * board, no RB daughterboard". Leaving this offset unmodelled made every
 * boot report zero GEs, which silently skips the entire GE microcode
 * download path (MgrasDownloadGE11Ucode returns immediately for a
 * non-positive GE count) -- the single most consequential inventory gap
 * found so far. The same retained SI board-class evidence cited above (one
 * GE, one RE, no TRAM, single-RE population) fixes a value for all three
 * fields; the low 24 bits have no established meaning and read zero.
 */
#define SGI_MGRAS_BOARD_INVENTORY 0x072488

/*
 * The kernel driver's own I2C device probe, live-traced through a boot that
 * reaches this code for the first time: it writes a strobe byte (0x40) to
 * 0x073c88, writes a test byte (0x55, then 0xaa on the first pattern's
 * failure) to 0x073c08, strobes 0x073c88 again, and reads 0x073c08 back,
 * concluding a device answers only if the byte it reads matches the byte it
 * just wrote -- a write/read-back loopback test, not a real I2C exchange.
 * 0x073c08 stores whatever byte was last written and echoes it back, the
 * same "accept as opaque state" treatment as the other undocumented
 * registers above; 0x073c88 is never read back in this routine, so it is
 * accepted but not otherwise modelled.
 */
#define SGI_MGRAS_I2C_SCRATCH 0x073c08
#define SGI_MGRAS_I2C_STROBE 0x073c88

/*
 * The kernel driver's own RE/PP (rasterizer-to-pixel-processor) sync
 * handshake, live-traced through a boot that reaches this code for the
 * first time: the driver writes a channel-select word here, then reads
 * this same offset back, requiring bits 3:0 and bits 11:8 of the result to
 * agree with each other and equal 1 before it will continue -- otherwise it
 * retries the whole handshake forever. This device's usual "echo the last
 * write back" treatment does not work here, since every value the driver
 * writes (0x50214, 0x70214, 0x653, 0x10214) has two nibbles that already
 * disagree, so echoing any of them back would leave the driver spinning
 * exactly as it did unmodelled.
 *
 * The Octane_ide (Integrated Diagnostic Environment) Reference Guide (SGI
 * document 108-0217-002), a published manual, shows this same function
 * completing on real, working hardware with no visible delay -- its
 * "before/in/after MgrasSyncREPP" trace lines appear back to back in every
 * logged diagnostic run, immediately followed by the next test suite
 * passing. This project's own retained evidence for the SI board class
 * this device models (docs/research/graphics/configurations.md) also gives
 * this configuration a small, fixed pixel-processor topology: one GE, one
 * RE, no TRAM. Together, that licenses the same "immediate readiness, no
 * modelled delay" approximation already used at 0x011098 and 0x020100:
 * report the handshake already satisfied rather than modelling multi-chip
 * link training this configuration does not have.
 */
#define SGI_MGRAS_REPP_SYNC 0x071f80

/*
 * Three more offsets the REPP sync handshake above writes a status byte to
 * on every pass, before it ever reads SGI_MGRAS_REPP_SYNC back: 0x071c08,
 * 0x071d00 and 0x071d08. None had a case before this device live-verified
 * the handshake actually completing -- every write took the sticky
 * unsupported-access path, which does not fail the write itself but does
 * set the same error flag SGI_MGRAS_HQ_STATUS's healthy path depends on
 * being clear, so leaving these unmodelled was silently blocking the very
 * next poll after the handshake it appears in even once the handshake
 * itself was fixed. None of their bit meanings were captured, so all three
 * store opaque state, the same treatment already given to the other
 * undocumented registers above.
 *
 * The handshake writes a fourth offset in this same cluster, 0x071c88, but
 * that address already has an established meaning here as the colour map
 * index register (see the 0x071c88 read case and 0x070c30's write case) --
 * a real, working register from earlier in this device's history, not
 * something to redefine on the strength of one more write site. si_write
 * accepts that specific write without storing it, the same "accept but
 * don't invent a second meaning" treatment SGI_MGRAS_I2C_STROBE gets,
 * rather than either overwriting the colour map index with an unrelated
 * value or leaving the write to fall through to the sticky-error path.
 */
#define SGI_MGRAS_REPP_STATUS_1 0x071c08
#define SGI_MGRAS_REPP_STATUS_3 0x071d00
#define SGI_MGRAS_REPP_STATUS_4 0x071d08

/*
 * A fifth status write the same handshake makes, in a different register
 * block (0x61000s, not the 0x71000s cluster above) -- found and treated the
 * same way, for the same reason.
 */
#define SGI_MGRAS_SYNC_STATUS 0x061038

/*
 * A colour-lookup-table load reached once the fixes above let a boot get
 * this far: a bounded, index-checked loop writes a computed 16-bit word to
 * 0x071890 on every pass, and a byte each to 0x071810 and 0x071910
 * depending on which channel the pass targets. None of the three is ever
 * read back, so all three store opaque state, the same treatment already
 * given to the other undocumented registers above. This loop alone
 * accounts for most of the unsupported-access traffic this device
 * previously logged during a full boot.
 */
#define SGI_MGRAS_DAC_LUT_INDEX 0x071890
#define SGI_MGRAS_DAC_LUT_DATA_1 0x071810
#define SGI_MGRAS_DAC_LUT_DATA_2 0x071910

/* A fourth register the same initialization writes just before the loop
 * above. Never read back either, so it gets the same treatment. */
#define SGI_MGRAS_DAC_CONFIG 0x071988

/*
 * A single, live-observed 4-byte write one byte into
 * SGI_MGRAS_DAC_LUT_DATA_1's own word (offset 0x071811, not 0x071810 --
 * genuinely misaligned, not a byte-wide access this device's own width
 * adjustment already handles). This device's generic width/alignment
 * guard rejects any unaligned access before the address switch below ever
 * sees it, so this offset needs its own carve-out ahead of that guard.
 * Seen exactly once in a full boot, never read back, so it stores opaque
 * state like the other undocumented registers above rather than being
 * folded into the aligned word next to it, since nothing establishes the
 * two share one meaning.
 */
#define SGI_MGRAS_DAC_LUT_DATA_1_LOW 0x071811

/*
 * A board-configuration table, live-observed as a block of fixed 4-byte
 * constants written across this whole offset range during bring-up
 * (0x061008, 0x061018-0x061030, 0x061040, 0x061048, 0x061078 confirmed so
 * far; SGI_MGRAS_SYNC_STATUS above, at 0x061038, is a separately-evidenced
 * register in the same block, kept as its own named field since the REPP
 * handshake gives it a distinct story). None of this range is ever read
 * back, so the whole table stores opaque state rather than naming each
 * offset individually.
 */
#define SGI_MGRAS_CONFIG_TABLE_BASE 0x061000
#define SGI_MGRAS_CONFIG_TABLE_WORDS 64

/*
 * A single, fixed-value write live-observed in the same board bring-up
 * pass as the configuration table above, in a sibling register block
 * (0x060000s, not 0x061000s) -- always the same constant (2), never read
 * back, so it gets the same opaque treatment.
 */
#define SGI_MGRAS_BRINGUP_FLAG 0x060008

/*
 * A single, fixed-value write in the generic widget bring-up block (next
 * to SGI_MGRAS_RESET_ARG and the 0x011098 generic bring-up step), never
 * read back, so it gets the same opaque treatment.
 */
#define SGI_MGRAS_BRINGUP_ARG_2 0x011088

/*
 * A small, one-shot write-only block in a fourth register area
 * (0x040000s), each offset written exactly once during the same board
 * bring-up pass and never read back, so all three get the same opaque
 * treatment.
 */
#define SGI_MGRAS_BRINGUP_BLOCK_1 0x040918
#define SGI_MGRAS_BRINGUP_BLOCK_2 0x040920
#define SGI_MGRAS_BRINGUP_BLOCK_3 0x040928

/*
 * A register one word above the real context/command register
 * (0x120400/0x130500's own protocol -- see si_context_write and
 * si_command), written at both 4 and 8 bytes with varying values across
 * 33 accesses in one boot and never read back. Nothing ties this offset
 * to the entry-selection/context-word protocol those two enforce, so it
 * gets plain opaque storage at either width instead of being routed
 * through that protocol's own state machine.
 */
#define SGI_MGRAS_SCRATCH_COMMAND 0x130400

/*
 * A single, one-shot write in the same generic widget bring-up block as
 * SGI_MGRAS_BRINGUP_ARG_2, never read back, so it gets the same opaque
 * treatment. The kernel HQ driver's own enable-clear register
 * (SGI_MGRAS_HQ_ENABLE below) shares this exact offset, written later in
 * the boot for a different purpose; see that macro's comment.
 */
#define SGI_MGRAS_BRINGUP_ARG_3 0x011058

/*
 * A write-only register near the existing colour map data register
 * (0x070d18), seen twice with the same constant during VC3 SRAM
 * initialization. Never read back, so it gets the same opaque treatment.
 */
#define SGI_MGRAS_VC3_ARG 0x070d88

/*
 * A write-only register in the same 0x71000s block as the REPP handshake
 * and DAC registers above, always written zero, never read back, so it
 * gets the same opaque treatment.
 */
#define SGI_MGRAS_RE_ARG 0x071e80

/*
 * The XMAP main-mode window register: live-traced as the offset a real
 * desktop session writes once per XMAP entry during Xsgi's own start-of-day
 * window-ID setup (81 writes in one boot-to-desktop session, all clustered
 * in one setup pass, never read back). This device previously had no case
 * for this offset at all -- every one of those writes took the generic
 * unsupported-access path -- and separately had a `xmap_mode` field that
 * si_reset initialized to a plausible default but nothing ever wrote,
 * leaving the field permanently dead. Decoding the same session's own
 * traced values settles which family a write belongs to: eight consecutive
 * writes (`0x601`, `0x621`, ..., `0x6e1`) share bits 11:8 == `0x6` and
 * otherwise only vary bits 7:5, a plain 0-7 window-slot counter; four more
 * (`0x7a4`, `0x7a5`, `0x7a7`, `0x17a8`) share bits 11:8 == `0x7`. These
 * match the datasheet's own already-recorded public WID values for
 * PseudoColor (`0x601`/`0x621`/`0x641`) and TrueColor (`0x7a4`/`0x7a5`/
 * `0x7a7`/`0x17a8`) windows exactly, so bits 11:8 are this device's own
 * bounded discriminator: `0x600` selects PseudoColor, `0x700` selects
 * TrueColor. One traced write (`0x403`) matches neither family; this
 * device treats an unrecognized value as TrueColor, the same direct-RGB
 * scanout this device already had before this register was wired up, so
 * an unrecognized mode does not regress existing behavior.
 *
 * This device tracks no per-window/per-region visual class at all, so
 * `xmap_mode` is a single global "most recently programmed" value used for
 * every pixel at scanout (si_gfx_update, hw/misc/sgi-mgras-display.c) rather than a
 * real per-window lookup -- an explicit, labelled first-pass approximation,
 * not a claim of per-window precision. See si_gfx_update's own header
 * comment for how this drives colour-map lookup versus direct RGB.
 * CONTRACT CHANGE, needs independent review: this is new dispatch on a
 * previously-unhandled offset, and the PseudoColor/TrueColor split it
 * drives is this device's own bounded reading of the cited trace, not an
 * independently confirmed XMAP register contract.
 */
#define SGI_MGRAS_XMAP_MAIN_MODE 0x071e00

/*
 * The VC3 video-timing/cursor/display-control chip's own indexed register
 * file, disassembled from the kernel driver's timing, cursor and display
 * setup routines (MgrasSetVC3Timing, MgrasClearCursor, MgrasPositionCursor,
 * MgrasEnableVideoTiming, MgrasInitVC3, MgrasStart). The driver writes
 * every register through one indexed port -- a 32-bit write packing an
 * 8-bit register index in the top byte and a 16-bit value in bits 23:8 --
 * and reads one back through a separate index-then-data pair. Register
 * index 7 does not select an ordinary value register; it loads the address
 * pointer for a 64 KiB (0x8000-halfword) SRAM the same chip exposes through
 * its own data port (see SGI_MGRAS_VC3_RAM below), auto-incrementing after
 * every RAM access. The SRAM holds video/DID line and frame tables, the
 * cursor glyph, and colour lookup data the X server keeps writing after
 * attach; the register file's own remaining indices (cursor position,
 * cursor/display enable bits, scan geometry, a revision readback) were
 * already partly modelled ad hoc before this offset's full contract was
 * understood -- see the individual bit assignments in si_write/si_read.
 */
#define SGI_MGRAS_VC3_INDEXDATA 0x072038
#define SGI_MGRAS_VC3_INDEX 0x072008
#define SGI_MGRAS_VC3_DATA 0x0720b0
#define SGI_MGRAS_VC3_RAM 0x072190
#define SGI_MGRAS_VC3_SRAM_WORDS 0x8000

/*
 * Two more offsets the generic widget interrupt-destination pair above is
 * also reachable at: MgrasSetHQ4IntrVectors programs the same field layout
 * (target widget, vector, address) through 0x000030/0x000038 instead of
 * SGI_MGRAS_INT_DEST_UPPER/LOWER, one word below the pair this device
 * already models. Nothing establishes the two pairs hold independent
 * state, so both address the same destination fields rather than adding a
 * second, separate one.
 */
#define SGI_MGRAS_INT_DEST_UPPER_ALT 0x000030
#define SGI_MGRAS_INT_DEST_LOWER_ALT 0x000038

/*
 * The HQ4-specific interrupt-vector programming MgrasSetHQ4IntrVectors
 * writes alongside the generic destination pair above: a second address
 * pair in the same field shape (0x011060/0x011068, target widget in bits
 * 27:24, crosstalk address upper 16 bits in bits 15:0 of 0x011060, lower 32
 * bits in 0x011068), plus a four-byte packed vector table, one byte per
 * interrupt role (0x011070). Settled by disassembling mgras_attach's own
 * four xtalk_intr_connect-style registrations against the retained disk's
 * rebuilt kernel: mgras_hq4_lowater_intr (the FIFO status/low-water role)
 * takes bits 7:0, MgrasGeneralInterrupt (context/flags/swap/HQ-error,
 * dispatched from the pending register at 0x011020 and its mask at
 * 0x011040) takes bits 15:8, MgrasRetraceHandler takes bits 23:16. This
 * device delivers each of those three roles as its own independent
 * pending/assertion source using this destination pair and its own vector
 * byte (si_deliver_hq_role, sgi-mgras.c) instead of the single shared
 * vector/delivery path every role used before -- the actual cause of the
 * "TLBMISS: KERNEL FAULT" watchdog panic the SI_PENDING_LOWATER row above
 * used to record as still open: every packet this device ever sent used
 * the widget-error vector below instead (confirmed live: "HQ4 widget err -
 * 0x0," appears in every retained panicking console log, exactly once,
 * right when MgrasSetHQ4IntrVectors' packet lands in MgrasHQ4ErrInterrupt
 * instead of mgras_hq4_lowater_intr), so mgras_hq4_lowater_intr never ran
 * regardless of the arming-side fixes already landed. The fourth role,
 * MgrasHQ4ErrInterrupt (widget error, a printf-only handler that clears
 * nothing), keeps using the generic destination pair above
 * (0x000030/0x000038, vector in bits 31:24) exactly as already implemented
 * -- that pair's own decode was already correct, it just needed to stop
 * being every other role's vector source too. Confirmed live against the
 * same retained disk: with this fix, opening the board (gfxinit) twice in
 * one boot no longer reaches the panic at all, and "HQ4 widget err" no
 * longer appears anywhere in the console output, where it previously
 * appeared exactly once per open.
 */
#define SGI_MGRAS_HQ_VECTOR_DEST_UPPER 0x011060
#define SGI_MGRAS_HQ_VECTOR_DEST_LOWER 0x011068
#define SGI_MGRAS_HQ_VECTORS 0x011070

/*
 * Three more registers MgrasCtxsw and MgrasFlushHQ3 save, restore or
 * read-modify-write around a context switch: 0x011030 and 0x011038 store
 * plain opaque state (MgrasInitHQ also writes both once at start-of-day).
 * 0x011050 is the driver's own "current second-level interrupt enables"
 * word -- it reads this back, computes a new value in software, and writes
 * the whole word back, so the device only needs to store and echo it
 * faithfully, not perform the read-modify-write itself. 0x011058 is that
 * enable word's companion clear register: a bit written there is cleared
 * from 0x011050 rather than replacing the whole word. It shares its
 * offset with SGI_MGRAS_BRINGUP_ARG_3 above, a generic one-shot write an
 * earlier, PROM-level bring-up pass already found at this same address --
 * both are real, at different points in the boot (PROM bring-up, then
 * later kernel HQ operation), so the write case applies both effects
 * rather than treating one observation as replacing the other.
 *
 * Bit 0x10 of 0x011050 (SI_HQ_ENABLE_LOWATER, sgi-mgras.c) is no longer
 * opaque: it is MgrasFlushHQ3's and mgras_hq4_hiwater_intr's own
 * "arm the FIFO status/low-water wait" signal, live-traced from both
 * routines, and mgras_hq4_lowater_intr clears it back out via 0x011058
 * when that interrupt is delivered. See si_update_irq's SI_PENDING_LOWATER
 * comment and the datasheet's HQ interrupt/config block row for the
 * approximation this closes: a watchdog panic roughly 15 seconds after
 * any driver path (gfxinit, the X server) opened this board, since nothing
 * previously ever raised the interrupt that disarms it.
 *
 * 0x011058's SI_HQ_ENABLE_LOWATER-clearing write also clears the matching
 * SI_PENDING_LOWATER pending bit, not just the enable bit -- only that
 * handler ever clears it, matching the driver's own real arm/deliver/
 * disarm/re-arm usage. See the write case's own comment for the root
 * cause this closes (docs/plans/si-graphics-widget-wiring-handoff-
 * 20260916.md, "gfxinit clears GE download live, then MgrasCtxsw
 * timeout"): without this, a second arming after the first deliver/disarm
 * cycle could never raise a second packet, since the pending bit was
 * already stuck set.
 */
#define SGI_MGRAS_HQ_CONFIG_A 0x011030
#define SGI_MGRAS_HQ_CONFIG_B 0x011038
#define SGI_MGRAS_HQ_ENABLE 0x011050

/*
 * A block of FIFO threshold registers MgrasInitHQ writes once at start-of-
 * day and MgrasFlushHQ3 rewrites afterward (low/high-water marks and
 * similar bounds); none is ever read back, so the whole small range stores
 * opaque state the same way SGI_MGRAS_CONFIG_TABLE_BASE does for the
 * larger board-configuration block.
 */
#define SGI_MGRAS_FIFO_THRESHOLD_BASE 0x040000
#define SGI_MGRAS_FIFO_THRESHOLD_WORDS 13

/*
 * Four more registers in the same 0x040000s block MgrasInitialize,
 * MgrasStart, MgrasCtxsw and several buffer-swap paths write during normal
 * operation, none ever read back, so they get the same opaque treatment.
 * SGI_MGRAS_SCHED_ARG_4 (0x040070) is MgrasInitialize's own flow-control
 * credit word, computed as (0x11 << 3) | (board_byte << 8) | 0x1000 and
 * written once during setup -- root-caused live as the trigger for the
 * gfxinit CTXSW/CFIFO timeout documented in docs/plans/
 * si-graphics-widget-wiring-handoff-20260916.md, "gfxinit clears GE
 * download live, then MgrasCtxsw timeout": with no case here, the write
 * took the generic unsupported-access path and latched this device's
 * sticky error flag, which in turn poisoned every later FIFO status read
 * (see the 0x020000 case). Modelling it as opaque storage like its three
 * siblings removes the trigger.
 */
#define SGI_MGRAS_SCHED_ARG_1 0x040068
#define SGI_MGRAS_SCHED_ARG_2 0x040078
#define SGI_MGRAS_SCHED_ARG_3 0x040088
#define SGI_MGRAS_SCHED_ARG_4 0x040070

/*
 * The HQ4's own flag word and its clear alias, in the same 0x020000s block
 * as the reset-readiness and bus-status registers above. Both offsets read
 * back the same underlying flag word; a write to SGI_MGRAS_HQ_FLAGS ORs the
 * written value into it (sets bits), and a write to SGI_MGRAS_HQ_FLAGS_CLEAR
 * AND-NOTs the written value out of it (clears bits) -- the usual shape for
 * a combined set/clear status-flag pair.
 *
 * Root-caused by disassembling the kernel driver's own MgrasPcxSwap/
 * MgrasValidateBanks/MgrasValidateClip (which read and clear bits here,
 * previously modelled as two independent opaque scratch registers -- see
 * the superseded row this replaces in docs/datasheets/mgras-hq4.md) together
 * with the DDX's own mgrasCopyPixels/mgrasDrawPixels fence sequence: it
 * clears bit 0x10000 here, submits a specific command-FIFO token at offset
 * 0x120400 (see si_command_fifo_token, hw/misc/sgi-mgras-raster.c), and then polls this
 * same offset for that bit to come back. On real hardware the HQ4 sets the
 * bit as a side effect of processing that token; this device previously
 * modelled neither side of that mechanism, so the fence poll spun forever
 * -- confirmed live as the exact PC and address Xsgi itself stalls on. This
 * closed the one remaining blocker in SI graphics bring-up independent of
 * the separate keyboard/mouse gap. CONTRACT CHANGE, needs independent
 * review: replaces the two-opaque-register reading this offset pair had
 * before.
 */
#define SGI_MGRAS_HQ_FLAGS 0x020010
#define SGI_MGRAS_HQ_FLAGS_CLEAR 0x020018

/*
 * The retrace interrupt handler's own status registers: it reads a 64-bit
 * word (0x020038) and a 32-bit word (0x020044) on every retrace interrupt,
 * and when both read zero, writes a zero halfword to 0x020028. Nothing
 * establishes real content for the two reads, so this device reports zero
 * -- a labelled approximation, not evidence -- and accepts the write.
 */
#define SGI_MGRAS_RETRACE_STATUS_1 0x020038
#define SGI_MGRAS_RETRACE_STATUS_2 0x020044
#define SGI_MGRAS_RETRACE_ACK 0x020028

/*
 * Six words is the largest payload this device recognizes on any
 * command-FIFO token header (RE_BLOCKCOPY's own six-word payload,
 * sgi-mgras-raster.c's header comment on SI_CMD_HDR_CLASS_STATE); sized with a
 * little headroom rather than exactly six.
 */
#define SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS 8

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
    uint32_t i2c_scratch;
    uint32_t repp_status[3];
    uint32_t sync_status;
    uint32_t dac_lut_index;
    uint32_t dac_lut_data[2];
    uint32_t dac_config;
    uint32_t dac_lut_data_1_low;
    uint32_t config_table[SGI_MGRAS_CONFIG_TABLE_WORDS];
    uint32_t bringup_flag;
    uint32_t bringup_arg_2;
    uint32_t bringup_block[3];
    uint64_t scratch_command;
    uint32_t bringup_arg_3;
    uint32_t vc3_arg;
    uint32_t re_arg;
    uint32_t hq_status_extra;
    uint32_t hq_vector_dest_upper;
    uint32_t hq_vector_dest_lower;
    uint32_t hq_vectors;
    uint32_t hq_config_a;
    uint32_t hq_config_b;
    uint32_t hq_enable;
    uint32_t fifo_threshold[SGI_MGRAS_FIFO_THRESHOLD_WORDS];
    uint32_t sched_arg_1;
    uint32_t sched_arg_2;
    uint32_t sched_arg_3;
    uint32_t sched_arg_4;
    uint32_t hq_flags;
    /*
     * Four independent interrupt roles, each with its own transition
     * detector: low-water, general (context/flags/HQ-error), retrace and
     * widget-error. See the SGI_MGRAS_HQ_VECTOR_DEST_UPPER/LOWER/VECTORS
     * header comment above and si_update_irq (sgi-mgras.c).
     */
    bool lowater_asserted;
    bool general_asserted;
    bool retrace_asserted;
    bool error_asserted;
    /* Host-only qtest introspection; not guest state, not migrated. */
    uint32_t test_pixel_x;
    uint32_t test_pixel_y;
    uint64_t test_last_interrupt_addr;
    uint64_t test_last_interrupt_value;
    QEMUTimer *retrace_timer;
    uint32_t *front;
    uint32_t *back;
    uint32_t front_count;
    uint32_t back_count;
    /*
     * Per-pixel companion to front[], one byte per entry (same WIDTHxHEIGHT
     * sizing as front_count): nonzero means the paired front[] value is a
     * CMAP index needing si_cmap_rgb() at scan time, zero means it is
     * already a direct 0x00BBGGRR colour -- see si_fill_rect's own header
     * comment (sgi-mgras.c) for why this replaced a device-global XMAP-mode
     * flag. Allocated alongside front[] (si_realize) but unused before this
     * pass.
     */
    uint8_t *wid;
    uint32_t hq[5120];
    uint32_t *ge[2];
    uint32_t ge_write_ptr[2];
    /* Unit index (not a flat word index); see the GE port header comment. */
    uint32_t ge_read_ptr[2];
    uint32_t ge_words[2];
    uint32_t ge_active_slot;
    /* Per-slot verify-pass alignment (control bit 0) and load-vs-register-
     * value mode, both from the most recent control write to that slot;
     * see the GE port header comment. */
    bool ge_read_odd[2];
    bool ge_load_mode[2];
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
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t cmap_index;
    uint32_t cmap[256];
    /*
     * The direct-command register file the guest addresses through
     * si_command's decoded selector -- persistent state, not per-fill
     * scratch: fill mode, masks, window origin and colour are set once
     * and reused across operations until reset or a context switch, the
     * same way real hardware keeps them (docs/designs/2026-09-17-si-viz-
     * pixel-output-design.md). Indexed by the 9-bit register field
     * si_command decodes from the selector; see the SI_REG_* macros and
     * si_command's own header comment in sgi-mgras-raster.c.
     */
    uint32_t rss[0x200];
    /*
     * This device's own write cursor for glyph rows and PIO pixel pairs
     * (si_draw_glyph_row/si_draw_pio_pixel_pair, sgi-mgras-raster.c), reset
     * whenever the block-start register is rewritten. Real hardware has
     * no guest-visible cursor register for either -- this is internal
     * bookkeeping for this device's own bounded interpretation of "one
     * execute record per row/pair", not a modelled register.
     */
    uint32_t glyph_row;
    uint32_t pio_x;
    uint32_t pio_y;
    /*
     * Command-FIFO token-stream parser state (si_command_fifo_token,
     * sgi-mgras-raster.c), persisted across separate 64-bit MMIO stores to
     * 0x120400 rather than kept as local decode state: a recognized
     * header's own declared payload can trail into a later store, so the
     * parser needs to remember it is still mid-payload between si_write
     * calls. cmd_token_class/cmd_token_reg identify the header this
     * payload belongs to; cmd_token_payload holds what has arrived so far
     * (an oversized payload is consumed in full to keep the stream in sync,
     * but none of that command is stored or executed). See the header comment
     * on SI_CMD_HDR_CLASS_STATE for which headers this applies to and why it
     * is scoped that way.
     */
    uint32_t cmd_token_class;
    uint32_t cmd_token_reg;
    uint32_t cmd_token_words_remaining;
    uint32_t cmd_token_payload[SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS];
    uint32_t cmd_token_payload_count;
    uint64_t retraces;
    uint32_t xmap_mode;
    uint16_t vc3_reg[32];
    uint8_t vc3_index;
    uint16_t vc3_ram_addr;
    uint16_t vc3_sram[SGI_MGRAS_VC3_SRAM_WORDS];
    uint32_t revision;
    uint32_t part;
    uint32_t manufacturer;
    /*
     * The QEMU display surface front[] scans out into (docs/designs/
     * 2026-09-17-si-viz-pixel-output-design.md, "Wire a real
     * QemuConsole"). display_buffer is a plain, host-owned
     * PIXMAN_x8r8g8b8 buffer -- not qemu_create_displaysurface()'s own
     * default shareable pixman allocation -- matching the RAD4 sibling
     * device's own choice (hw/display/psitech-rad4.c) since nothing here
     * needs zero-copy sharing with an external consumer. Neither con nor
     * display_buffer is migrated; see si_post_load (sgi-mgras.c). dirty
     * tracks whether front[] or display_enabled changed since the last
     * gfx_update, so si_gfx_update only redoes the conversion when it
     * would actually differ.
     */
    QemuConsole *con;
    uint32_t *display_buffer;
    bool dirty;
};

#endif
