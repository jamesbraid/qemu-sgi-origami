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
 * treatment.
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
 * The VC3 SRAM self-test's own write/read register. The driver's
 * MgrasLoadVC3SRAM routine picks one of several fixed values (based on a
 * discovered per-unit configuration byte) and writes it here, then calls
 * MgrasVerifyVC3SRAM, which writes an address/mode word here before
 * reading it back -- disassembly shows both routines touch only this one
 * offset, never an address range, so this is a single scalar register
 * rather than a window onto a larger SRAM array. Its own bit layout was
 * never captured, so it gets the same opaque, plain-storage treatment as
 * this device's other undocumented registers: whatever the guest writes,
 * it reads back unchanged. The device's original stub instead indexed a
 * 64-entry array by bits of the written value, which never round-tripped
 * a write back to the same read -- exactly the "VC3SRAM verify failed"
 * failure a live boot reached once the driver's own hardware
 * self-verification step could run for the first time in this project's
 * history.
 */
#define SGI_MGRAS_VC3_SRAM 0x072000

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
    uint32_t vc3_sram;
    uint32_t revision;
    uint32_t part;
    uint32_t manufacturer;
};

#endif
