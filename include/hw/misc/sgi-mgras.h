/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_MGRAS_H
#define HW_MISC_SGI_MGRAS_H

#include "hw/core/sysbus.h"
#include "hw/core/resettable.h"
#include "system/memory.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_SGI_MGRAS "sgi-mgras"
OBJECT_DECLARE_TYPE(SGIMgrasState, SGIMgrasClass, SGI_MGRAS)

#define SGI_MGRAS_MMIO_SIZE (2 * MiB)
#define SGI_MGRAS_WIDTH 1280
#define SGI_MGRAS_HEIGHT 1024
#define SGI_MGRAS_CMAP_ENTRIES 8192
#define SGI_MGRAS_XMAP_ENTRIES 32

/* Uploaded program state is retained. Its protocol is unsupported. */
#define SGI_MGRAS_GE_WORDS (59392 * 3)
#define SGI_MGRAS_GE_SLOT_A 0
#define SGI_MGRAS_GE_SLOT_B 1
#define SGI_MGRAS_GE_DATA_A 0x058048
#define SGI_MGRAS_GE_CTRL_A 0x05804c
#define SGI_MGRAS_GE_DATA_B 0x058040
#define SGI_MGRAS_GE_CTRL_B 0x058044
#define SGI_MGRAS_GE_READ_1 0x05822c
#define SGI_MGRAS_GE_READ_2 0x058230

/* Direct-XIO interrupt routing is an approximation. */
#define SGI_MGRAS_INT_DEST_UPPER 0x000034
#define SGI_MGRAS_INT_DEST_LOWER 0x00003c

/* Unknown control fields are stored without interpreted side effects. */
#define SGI_MGRAS_WIDGET_CONTROL 0x000024

/* Unknown reset fields are stored without interpreted side effects. */
#define SGI_MGRAS_RESET_ARG 0x011008

/* Reset and population values are incomplete approximations. */
#define SGI_MGRAS_RESET_CONTROL 0x072408

/* Board inventory fields are incomplete approximations. */
#define SGI_MGRAS_BOARD_INVENTORY 0x072488

/* I2C behavior is unsupported. These fields retain opaque state. */
#define SGI_MGRAS_I2C_SCRATCH 0x073c08
#define SGI_MGRAS_I2C_STROBE 0x073c88

/* Raster synchronization completes immediately as an approximation. */
#define SGI_MGRAS_REPP_SYNC 0x071f80

/* Unknown synchronization fields retain opaque state. */
#define SGI_MGRAS_REPP_STATUS_1 0x071c08
#define SGI_MGRAS_REPP_STATUS_3 0x071d00
#define SGI_MGRAS_REPP_STATUS_4 0x071d08

#define SGI_MGRAS_SYNC_STATUS 0x061038

/*
 * DAC ports. 0x071810 is the DAC address, a halfword with the register
 * number in its high byte. A byte write to 0x071811 sets that register
 * number, not the halfword's low byte: SGI's IDE address test reads it
 * back in bits 15:8. Only a halfword write sets the low byte (0x0402).
 * 0x071910 moves one byte of the addressed register. SGI's IDE DAC reset
 * programs registers 0x0d00, 0x0400 (pixel mask), 0x0500-0x0800 (command
 * registers) and 0x0402 (cursor) this way and reads them back, and a
 * captured desktop session writes the same addresses in the same order
 * before each gamma load, then address 0. 0x071890 is the gamma-table
 * data port: each write stores its high byte into the current
 * row/channel and advances channel (red, green, blue), then row. The
 * model resets the load position on any halfword address write, which
 * every captured load ends with 0.
 */
#define SGI_MGRAS_DAC_LUT_INDEX 0x071810
#define SGI_MGRAS_DAC_LUT_DATA 0x071890
#define SGI_MGRAS_DAC_LUT_DATA_2 0x071910

#define SGI_MGRAS_DAC_CONFIG 0x071988

/*
 * One byte per DAC register address the address halfword can select, all
 * plain storage: an approximation of a DAC whose register map is unknown.
 */
#define SGI_MGRAS_DAC_REGS 0x10000

#define SGI_MGRAS_DAC_LUT_INDEX_LOW 0x071811

/* Unknown board-configuration fields retain opaque state. */
#define SGI_MGRAS_CONFIG_TABLE_BASE 0x061000
#define SGI_MGRAS_CONFIG_TABLE_WORDS 64

#define SGI_MGRAS_BRINGUP_FLAG 0x060008

#define SGI_MGRAS_BRINGUP_ARG_2 0x011088

#define SGI_MGRAS_BRINGUP_BLOCK_1 0x040918
#define SGI_MGRAS_BRINGUP_BLOCK_2 0x040920
#define SGI_MGRAS_BRINGUP_BLOCK_3 0x040928

/*
 * Kernel-privileged DMA source-mapping command port. Shares 0x120400's
 * class/reg/len token framing, but a class-0x48 header writes straight
 * into a flat register file instead of buffering a payload for dispatch;
 * see sgi_mgras_dma_map_write() in hw/misc/sgi-mgras-raster.c. Raw writes are
 * also retained so a plain readback still round-trips, matching the
 * approximation used before the mapping registers were understood.
 */
#define SGI_MGRAS_SCRATCH_COMMAND 0x130400
#define SGI_MGRAS_DMA_MAP_SLOTS 5
#define SGI_MGRAS_DMA_MAP_REGS 0x50

#define SGI_MGRAS_BRINGUP_ARG_3 0x011058

#define SGI_MGRAS_VC3_ARG 0x070d88

/*
 * XMAP overlay mode, one entry per WID, selected by the same index byte at
 * 0x071c88 as the main mode. The stock X server writes 0xc1, 0xc9, 0xd1,
 * 0xd9, 0x1f1 and 0x1f9 for WIDs 0-5 and 0 for the rest. Inferred from
 * captured desktop sessions: bit 0 enables the overlay for that WID and
 * bits 7:3 select its colour map -- WID 1's 0xc9 names map 25, the map the
 * server loads with the popup-menu palette (entry 1 grey 0xc1c1c1, entry 2
 * black) that menu fills and text on the overlay visual draw with. Bit 8
 * (set for WIDs 4 and 5 only) is unexplained.
 */
#define SGI_MGRAS_XMAP_OVERLAY_MODE 0x071e80

/*
 * Established (docs/datasheets/mgras-hq4.md, "Swap scheduling"):
 * MgrasSchedSwapBuf writes this XMAP-indexed port with the per-window swap
 * mask that takes effect at the next retrace. SGI's IDE XMAP test writes
 * and reads it back under index byte 0 (all 32 bits, the main buffer
 * select) and index byte 4 (low 8 bits, which it calls the overlay buffer
 * select); the kernel reading takes index 4 as a second swap-mask word.
 */
#define SGI_MGRAS_XMAP_BUF_SELECT 0x071d80

/* Visual selection remains approximate because WID encoding is unresolved. */
#define SGI_MGRAS_XMAP_MAIN_MODE 0x071e00

/* Timing and cursor state are retained. Latch timing is approximated. */
#define SGI_MGRAS_VC3_INDEXDATA 0x072038
#define SGI_MGRAS_VC3_INDEX 0x072008
#define SGI_MGRAS_VC3_DATA 0x0720b0
#define SGI_MGRAS_VC3_RAM 0x072190
#define SGI_MGRAS_VC3_SRAM_WORDS 0x8000

/* Alternate interrupt offsets share the approximated destination state. */
#define SGI_MGRAS_INT_DEST_UPPER_ALT 0x000030
#define SGI_MGRAS_INT_DEST_LOWER_ALT 0x000038

/* Interrupt sources remain separate, but their routing is approximate. */
#define SGI_MGRAS_HQ_VECTOR_DEST_UPPER 0x011060
#define SGI_MGRAS_HQ_VECTOR_DEST_LOWER 0x011068
#define SGI_MGRAS_HQ_VECTORS 0x011070

/* FIFO interrupt thresholds and acknowledgement remain approximate. */
#define SGI_MGRAS_HQ_CONFIG_A 0x011030
#define SGI_MGRAS_HQ_CONFIG_B 0x011038
#define SGI_MGRAS_HQ_ENABLE 0x011050

/* FIFO thresholds are stored. Exact transitions remain unknown. */
#define SGI_MGRAS_FIFO_THRESHOLD_BASE 0x040000
#define SGI_MGRAS_FIFO_THRESHOLD_WORDS 13

/* Unknown scheduling fields retain opaque state. */
#define SGI_MGRAS_SCHED_ARG_1 0x040068
#define SGI_MGRAS_SCHED_ARG_2 0x040078
#define SGI_MGRAS_SCHED_ARG_3 0x040088
#define SGI_MGRAS_SCHED_ARG_4 0x040070

/* Flag completion is synchronous because fence ordering is incomplete. */
#define SGI_MGRAS_HQ_FLAGS 0x020010
#define SGI_MGRAS_HQ_FLAGS_CLEAR 0x020018

/* Retrace status and acknowledgement are approximated. */
#define SGI_MGRAS_RETRACE_STATUS_1 0x020038
#define SGI_MGRAS_RETRACE_STATUS_2 0x020044
#define SGI_MGRAS_RETRACE_ACK 0x020028

/* Unknown command payloads remain unsupported. */
#define SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS 8
#define SGI_MGRAS_CMD_TOKEN_DISCARD UINT32_MAX
#define SGI_MGRAS_CMD_TOKEN_MAX_DISCARD_WORDS \
    DIV_ROUND_UP(UINT8_MAX, sizeof(uint32_t))
/* An inline data block header carries a 20-bit byte length. */
#define SGI_MGRAS_CMD_TOKEN_MAX_BLOCK_WORDS DIV_ROUND_UP(0xfffff, 4)

/* Values for the per-pixel format tag; see the "format" field below. */
#define SGI_MGRAS_PIXEL_FORMAT_CI8 0
#define SGI_MGRAS_PIXEL_FORMAT_RGB 1
#define SGI_MGRAS_PIXEL_FORMAT_RGB12 2

struct SGIMgrasClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

/*
 * Composes a guest-visible widget/address pair into a fabric address space
 * offset, matching whichever addressing mode the machine wired up (packet
 * widget-shift or flat fabric base). Shared with hw/misc/sgi-mgras-raster.c
 * so host-memory DMA reads use the same routing as interrupt packets.
 */
bool sgi_mgras_xio_compose(SGIMgrasState *s, unsigned int widget,
                      uint64_t address, hwaddr *fabric);

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
    uint32_t dac_lut_index_low;
    /* Gamma-table load position: row 0-255, channel 0=R, 1=G, 2=B. */
    uint8_t dac_lut_load_pos;
    uint8_t dac_lut_load_channel;
    /*
     * Post-CMAP gamma-correction table. Reset seeds an identity ramp;
     * no capture can show the DAC's own power-on/reset state (a write
     * trace only observes writes), so identity is an inferred default,
     * not an observed one.
     */
    uint8_t dac_gamma[3][256];
    /* DAC registers addressed through DAC_LUT_INDEX, data at DAC_LUT_DATA_2. */
    uint8_t dac_reg[SGI_MGRAS_DAC_REGS];
    uint32_t config_table[SGI_MGRAS_CONFIG_TABLE_WORDS];
    uint32_t bringup_flag;
    uint32_t bringup_arg_2;
    uint32_t bringup_block[3];
    uint64_t scratch_command;
    /*
     * Class-0x48 kernel DMA source-mapping register file, indexed by the
     * 0x130400 FIFO token's register number: five slots' base pair, page
     * count, index width and control, plus three unresolved extras.
     */
    uint32_t dma_map_regs[SGI_MGRAS_DMA_MAP_REGS];
    uint32_t dma_map_reg;
    uint32_t dma_map_words_remaining;
    /*
     * Host-memory DMA image-transfer descriptor banks, main-port FIFO
     * classes 0x0008/0x000a/0x000c, stored as plain registers indexed by
     * the header's register number. 0x20000 uses registers 0, 4-8 and 11
     * (kick); 0x28000 uses 2 and 6; 0x30000 uses only register 0.
     */
    uint32_t dma_desc_20000[12];
    uint32_t dma_desc_28000[7];
    uint32_t dma_desc_30000[1];
    uint32_t bringup_arg_3;
    uint32_t vc3_arg;
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
    /* Interrupt sources keep independent transition state. */
    bool lowater_asserted;
    bool general_asserted;
    bool error_asserted;
    /* Diagnostic rate limit, not guest state and not migrated. */
    bool retrace_no_dest_logged;
    /* Host-only qtest introspection. Not guest state and not migrated. */
    uint32_t test_pixel_x;
    uint32_t test_pixel_y;
    uint64_t test_last_interrupt_addr;
    uint64_t test_last_interrupt_value;
    uint32_t test_dma_map_slot;
    /* Host-only event ordering. Not guest state and not migrated. */
    uint64_t trace_sequence;
    QEMUTimer *retrace_timer;
    uint32_t *front;
    uint32_t *back;
    uint32_t front_count;
    uint32_t back_count;
    /*
     * Per-pixel colour-format tag, one of the SGI_MGRAS_PIXEL_FORMAT_*
     * values above: CI8 selects the indexed plane (front holds an 8-bit
     * CMAP index), RGB selects packed colour (front holds 0x00RRGGBB
     * directly) and RGB12 a 12-bit window's colour (front holds 0xBGR,
     * four bits a channel). No register exposes a stored per-pixel format;
     * painting code derives it from PP1FILLMODE at the time each pixel is
     * written and records it here so scanout can resolve mixed-format
     * content within one WID. Approximation: see
     * docs/datasheets/mgras-raster-display.md.
     */
    uint8_t *format;
    /* Per-pixel overlay index. 0 is transparent and shows the main planes. */
    uint8_t *overlay;
    uint32_t hq[5120];
    uint32_t *ge[2];
    uint32_t ge_write_ptr[2];
    /* Unit index, not a flat word index. */
    uint32_t ge_read_ptr[2];
    uint32_t ge_words[2];
    uint32_t ge_active_slot;
    /* Per-slot uploaded-program state. Its protocol is unsupported. */
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
    uint16_t cmap_selector;
    uint8_t cmap_position;
    uint32_t cmap[SGI_MGRAS_CMAP_ENTRIES];
    /* Bounded raster state persists across synchronous operations. */
    uint32_t rss[0x200];
    /* Internal cursors implement the bounded raster approximation. */
    uint32_t glyph_row;
    uint32_t glyph_unit;
    uint32_t pio_x;
    uint32_t pio_y;
    /* Parser state preserves FIFO ordering across separate stores. */
    uint32_t cmd_token_class;
    uint32_t cmd_token_reg;
    uint32_t cmd_token_words_remaining;
    uint32_t cmd_token_payload[SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS];
    uint32_t cmd_token_payload_count;
    uint64_t retraces;
    uint8_t xmap_index;
    uint32_t xmap_mode[SGI_MGRAS_XMAP_ENTRIES];
    uint32_t xmap_buf_select[SGI_MGRAS_XMAP_ENTRIES];
    uint32_t xmap_overlay_mode[SGI_MGRAS_XMAP_ENTRIES];
    uint16_t vc3_reg[32];
    uint8_t vc3_index;
    uint16_t vc3_ram_addr;
    uint16_t vc3_sram[SGI_MGRAS_VC3_SRAM_WORDS];
    uint32_t revision;
    uint32_t part;
    uint32_t manufacturer;
    /* Host scanout is derived from guest state and is not migrated. */
    QemuConsole *con;
    uint32_t *display_buffer;
    bool dirty;
};

#endif
