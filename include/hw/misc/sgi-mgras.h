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

/* Unknown display-table fields retain opaque state. */
#define SGI_MGRAS_DAC_LUT_INDEX 0x071890
#define SGI_MGRAS_DAC_LUT_DATA_1 0x071810
#define SGI_MGRAS_DAC_LUT_DATA_2 0x071910

#define SGI_MGRAS_DAC_CONFIG 0x071988

#define SGI_MGRAS_DAC_LUT_DATA_1_LOW 0x071811

/* Unknown board-configuration fields retain opaque state. */
#define SGI_MGRAS_CONFIG_TABLE_BASE 0x061000
#define SGI_MGRAS_CONFIG_TABLE_WORDS 64

#define SGI_MGRAS_BRINGUP_FLAG 0x060008

#define SGI_MGRAS_BRINGUP_ARG_2 0x011088

#define SGI_MGRAS_BRINGUP_BLOCK_1 0x040918
#define SGI_MGRAS_BRINGUP_BLOCK_2 0x040920
#define SGI_MGRAS_BRINGUP_BLOCK_3 0x040928

/* Secondary FIFO routing is unsupported. Writes retain opaque state. */
#define SGI_MGRAS_SCRATCH_COMMAND 0x130400

#define SGI_MGRAS_BRINGUP_ARG_3 0x011058

#define SGI_MGRAS_VC3_ARG 0x070d88

#define SGI_MGRAS_RE_ARG 0x071e80

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
    (UINT8_MAX / sizeof(uint32_t))

struct SGIMgrasClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

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
    /* Interrupt sources keep independent transition state. */
    bool lowater_asserted;
    bool general_asserted;
    bool retrace_asserted;
    bool error_asserted;
    /* Host-only qtest introspection. Not guest state and not migrated. */
    uint32_t test_pixel_x;
    uint32_t test_pixel_y;
    uint64_t test_last_interrupt_addr;
    uint64_t test_last_interrupt_value;
    /* Host-only event ordering. Not guest state and not migrated. */
    uint64_t trace_sequence;
    QEMUTimer *retrace_timer;
    uint32_t *front;
    uint32_t *back;
    uint32_t front_count;
    uint32_t back_count;
    /* Per-pixel visual tag. Only a plane fill writes it. */
    uint8_t *wid;
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
