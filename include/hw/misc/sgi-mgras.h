/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_MGRAS_H
#define HW_MISC_SGI_MGRAS_H

#include "hw/core/resettable.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xio-widget.h"
#include "system/memory.h"
#include "qom/object.h"
#include "qemu/units.h"
#include "hw/display/sgi/board-memory.h"
#include "hw/display/sgi/command-fetch.h"
#include "hw/display/sgi/scanout.h"
#include "hw/display/sgi/gl-context.h"

#define TYPE_SGI_MGRAS "sgi-mgras"
OBJECT_DECLARE_TYPE(SGIMgrasState, SGIMgrasClass, SGI_MGRAS)

#define SGI_MGRAS_MMIO_SIZE (2 * MiB)
/*
 * The widest screen a board descriptor may name. Scanout's per-row buffers
 * are sized by it and realize refuses a wider descriptor; everything else
 * follows the descriptor's own width and height.
 */
#define SGI_MGRAS_MAX_WIDTH 1280
#define SGI_MGRAS_CMAP_ENTRIES 8192
#define SGI_MGRAS_XMAP_ENTRIES 32

/*
 * Uploaded program state is retained. Its protocol is unsupported.
 * Established from MgrasDownloadGE11Ucode (IRIX 6.5.30 mgras_init.o): the
 * kernel loads GE 0 through slot B's ports and GE 1 through slot A's,
 * setting 0x011000 bits 5:4 to 2 and 3 for them. A one-GE board uses
 * slot B alone.
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
 * The widget interrupt destination pair of the block every XIO widget
 * begins with (hw/mips/sgi/xio-widget.h), at its data halves. Direct-XIO
 * interrupt routing is an approximation.
 */
#define SGI_MGRAS_INT_DEST_UPPER (SGI_XIO_WIDGET_INT_DEST_UPPER + \
                                  SGI_XIO_WIDGET_REG_DATA)
#define SGI_MGRAS_INT_DEST_LOWER (SGI_XIO_WIDGET_INT_DEST_LOWER + \
                                  SGI_XIO_WIDGET_REG_DATA)

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

/*
 * PP1SELECT picks which PP1 the XMAP registers reach
 * (docs/datasheets/mgras-dual-re.md). Established from the IRIX 6.5.30
 * kernel: it writes ((re << 1) | pp) << 1 before a sequence meant for one
 * PP1 and 1 before writes every PP1 must take. Two PP1s sit behind each
 * RE, so a two-RE board has four. The model keeps a copy per PP1 of the
 * index byte 0x071c88 and of 0x071d00, 0x071d08, 0x071d80, 0x071e00 and
 * 0x071e80. It keeps none of 0x071f00 and 0x071f80, which read the same
 * fixed values under every selection: an approximation the datasheet's
 * model contract lists.
 */
#define SGI_MGRAS_REPP_STATUS_1 0x071c08
#define SGI_MGRAS_PP1_SELECT SGI_MGRAS_REPP_STATUS_1
#define SGI_MGRAS_PP1_SELECT_ALL 1
#define SGI_MGRAS_MAX_PP1 4

/* Unknown XMAP configuration fields retain opaque state, per PP1. */
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
 * MgrasSchedSwapBuf writes this XMAP-indexed port with the per-window swap
 * mask that takes effect at the next retrace. SGI's IDE XMAP test writes
 * and reads it back under index byte 0 (all 32 bits, the main buffer
 * select) and index byte 4 (low 8 bits, which it calls the overlay buffer
 * select); the kernel reading takes index 4 as a second swap-mask word.
 * The probe's swap captures (run irix-20260927T172059-fdbada) write
 * 0x4000 under index byte 0 at the first swap of a window with display ID
 * 14 and 0 at its second, and those swaps must show first the frame drawn
 * before them and then the next. Scanout therefore shows bank 1 for a
 * pixel whose main display ID has its bit set in the index-0 word, and
 * bank 0 otherwise; the polarity is inferred from that pairing.
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

/* The slots' other halves share the approximated destination state. */
#define SGI_MGRAS_INT_DEST_UPPER_ALT SGI_XIO_WIDGET_INT_DEST_UPPER
#define SGI_MGRAS_INT_DEST_LOWER_ALT SGI_XIO_WIDGET_INT_DEST_LOWER

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
/*
 * The payload buffer: the raster, descriptor and HQ headers accept up to
 * the limit above, and the OpenGL packets the model executes need up to
 * 16 words, for glLoadMatrixf and glMultMatrixf.
 */
#define SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS 16
#define SGI_MGRAS_CMD_TOKEN_DISCARD UINT32_MAX
#define SGI_MGRAS_CMD_TOKEN_MAX_DISCARD_WORDS \
    DIV_ROUND_UP(UINT8_MAX, sizeof(uint32_t))
/* An inline data block header carries a 20-bit byte length. */
#define SGI_MGRAS_CMD_TOKEN_MAX_BLOCK_WORDS DIV_ROUND_UP(0xfffff, 4)

/* Values for the per-pixel format tag; see the "format" field below. */
#define SGI_MGRAS_PIXEL_FORMAT_CI8 0
#define SGI_MGRAS_PIXEL_FORMAT_RGB 1
#define SGI_MGRAS_PIXEL_FORMAT_RGB12 2

/*
 * One PP1's XMAP register file: the index byte and every register it
 * indexes. Which of a board's PP1s a guest access reaches is PP1SELECT's.
 */
typedef struct SGIMgrasXmap {
    uint8_t index;
    uint32_t config;            /* 0x071d00 */
    uint32_t config_b;          /* 0x071d08 */
    uint32_t mode[SGI_MGRAS_XMAP_ENTRIES];
    uint32_t buf_select[SGI_MGRAS_XMAP_ENTRIES];
    uint32_t overlay_mode[SGI_MGRAS_XMAP_ENTRIES];
} SGIMgrasXmap;

struct SGIMgrasClass {
    SGIXioBoardClass parent_class;
    ResettablePhases parent_phases;
    DeviceUnrealize parent_unrealize;
};

/*
 * What one XIO IMPACT variant answers to the reads IRIX uses to tell the
 * variants apart. The two version bytes are the top bytes of 32-bit reads of
 * 0x072488 and 0x072408; the revisions land in the fields named beside
 * each member. The counts restate what those bytes decode to, so a board
 * the model cannot render is refused by name rather than by decoding.
 */
typedef struct SGIMgrasBoard {
    const char *name;           /* the "board" property value */
    const char *hinv_name;      /* what IRIX 6.5.x hinv prints for it */
    uint8_t ge_count;
    uint8_t re_count;
    uint8_t tram_count;         /* per raster path, as gfxinfo prints it */
    /* The screen the model scans out, in pixels. */
    uint16_t width;
    uint16_t height;
    uint8_t version1;           /* 0x072488 bits 31:24 */
    uint8_t version0;           /* 0x072408 bits 31:24 */
    uint8_t hq_revision;        /* 0x000000 bits 31:28 */
    uint8_t ge_revision;        /* 0x028004 bits 7:0, before any HQ query */
    uint8_t re4_revision;       /* 0x02c578 bits 7:4 */
    uint8_t pp1_revision;       /* 0x071d00 bits 2:0 under XMAP index 4 */
    uint8_t vc3_revision;       /* VC3 register 0x1f bits 7:6 */
    uint8_t cmap_revision;      /* 0x071708 bits 2:0 */
    uint8_t ra_revision;        /* 0x071708 bits 6:4 */
    bool video_option;          /* 0x071708 bit 7 clear */
    /* The model renders everything IRIX drives on this board. */
    bool implemented;
} SGIMgrasBoard;

/*
 * The main-port parser's position in a packet: the header being collected
 * and how many of its payload words are still to come. SGIMgrasState's
 * cmd_token_* fields hold the loaded context's; each GL context keeps its
 * own while another is loaded (hw/misc/sgi-mgras-gl.c).
 */
typedef struct SGIMgrasCmdParser {
    uint32_t cls;
    uint32_t reg;
    uint32_t words_remaining;
    uint32_t payload[SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS];
    uint32_t payload_count;
} SGIMgrasCmdParser;

/*
 * The family's side of one GL context-table entry: where the main-port
 * parser stood when the context was last switched out, beside the entry's
 * GL state. SGIGLContext is the GL state machine's; the parser position is
 * MGRAS framing and stays out of it.
 */
typedef struct SGIMgrasGLContext {
    /* The parser's position while this context is not loaded. */
    SGIMgrasCmdParser parser;
    SGIGLContext *gl;
} SGIMgrasGLContext;

/*
 * Entry 0 is the state used while no GL context is loaded; the others
 * hold one context each, keyed by its context-image word 0.
 */
#define SGI_MGRAS_GL_CONTEXTS 33

/* General interrupt source 10: the swap command has been reached. */
#define SGI_MGRAS_PENDING_SWAP 0x00000400

/* Latch general interrupt sources and update the interrupt lines. */
void sgi_mgras_raise_pending(SGIMgrasState *s, uint32_t sources);

/*
 * Composes a guest-visible widget/address pair into the fabric address a
 * widget-addressed link carries, the widget in bits 51:48 (the XIO widget
 * layer, hw/mips/sgi/xio-widget.h). Shared with hw/misc/sgi-mgras-raster.c
 * so host-memory DMA reads use the same routing as interrupt packets.
 */
bool sgi_mgras_xio_compose(SGIMgrasState *s, unsigned int widget,
                      uint64_t address, hwaddr *fabric);

struct SGIMgrasState {
    SGIXioBoardState parent_obj;
    MemoryRegion regs;
    MemoryRegion *fabric;
    AddressSpace fabric_as;
    uint32_t int_dest_upper;
    uint32_t int_dest_lower;
    uint32_t widget_control;
    uint32_t reset_arg;
    uint32_t reset_control;
    uint32_t i2c_scratch;
    uint32_t pp1_select;
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
    /*
     * The display list being executed, which lives only inside the port
     * write that invoked it. Not guest state and not migrated.
     */
    SGICommandFetch list_fetch;
    /*
     * Pixel-memory banks 0 and 1. The X server draws in bank 0; GL front
     * and back buffers map to either (hw/misc/sgi-mgras-gl.c), and bank 1
     * holds only direct colour.
     */
    uint32_t *front;
    uint32_t *back;
    uint32_t front_count;
    uint32_t back_count;
    /*
     * The same two banks as board memory: front and back point into its
     * bytes, and bank_plane[] names each bank's plane, so the shared DMA
     * code can bound a transfer by the bank it targets. Realize-time
     * configuration, not migrated: the pixels migrate through front and
     * back.
     */
    SGIBoardMemory mem;
    uint32_t bank_plane[2];
    /* 24-bit depth plane, one value per pixel of the screen. */
    uint32_t *depth;
    uint32_t depth_count;
    /*
     * Per-pixel colour-format tag, one of the SGI_MGRAS_PIXEL_FORMAT_*
     * values above: CI8 selects the indexed plane (front holds an 8-bit
     * CMAP index), RGB selects packed colour (front holds 0x00RRGGBB
     * directly) and RGB12 a 12-bit window's colour (front holds 0xBGR,
     * four bits a channel). No register exposes a stored per-pixel format;
     * painting code derives it from PP1FILLMODE at the time each pixel is
     * written and records it here so scanout can resolve mixed-format
     * content within one WID. This is a model approximation.
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
    uint32_t cmd_token_payload[SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS];
    uint32_t cmd_token_payload_count;
    /*
     * OpenGL command state (hw/misc/sgi-mgras-gl.c): gl[gl_current] is the
     * loaded context's, and gl_context[i].gl points at gl[i]. gl_slot[]
     * holds each entry's image word 0 and gl_selected[] the gl_selections
     * count when it was last loaded, zero for a free entry.
     */
    SGIGLContext gl[SGI_MGRAS_GL_CONTEXTS];
    SGIMgrasGLContext gl_context[SGI_MGRAS_GL_CONTEXTS];
    uint32_t gl_slot[SGI_MGRAS_GL_CONTEXTS];
    uint64_t gl_selected[SGI_MGRAS_GL_CONTEXTS];
    uint64_t gl_selections;
    uint32_t gl_current;
    /* GE-port traffic logged since the last switch; not migrated. */
    bool gl_ge_port_logged;
    /* The bank holding the back buffer, from 0x9808 or context words 16-17. */
    uint32_t gl_banks[2];
    uint64_t retraces;
    /*
     * One XMAP register file per PP1 the largest board has; a board uses
     * the first 2 * re_count. Scanout reads the first PP1's tables, an
     * approximation: which PP1 drives which pixels is unknown, and the
     * kernel and X server keep every PP1's tables alike.
     */
    SGIMgrasXmap xmap[SGI_MGRAS_MAX_PP1];
    uint16_t vc3_reg[32];
    uint8_t vc3_index;
    uint16_t vc3_ram_addr;
    uint16_t vc3_sram[SGI_MGRAS_VC3_SRAM_WORDS];
    uint32_t revision;
    uint32_t part;
    uint32_t manufacturer;
    char *board_name;
    bool unimplemented_board;
    const SGIMgrasBoard *board;
    /*
     * The board's screen, copied from its descriptor at realize. Signed so
     * that clipping arithmetic against possibly negative coordinates stays
     * signed.
     */
    int width;
    int height;
    /*
     * The word 0x028004 returns: the board's GE revision until the first
     * class-0 0xa2 query from the kernel or a client, then that query's
     * answer.
     */
    uint32_t ge_report;
    /*
     * The console, presented frame and retrace timer
     * (hw/display/sgi/scanout.h). Each frame is resolved from guest state
     * by sgi-mgras-display.c, so only the retrace phase is migrated: as
     * the timer's expiry, the field the version-2 stream always carried.
     */
    SGIScanout scanout;
    /*
     * Something the whole frame resolves from changed. Folded into the
     * scanout's dirty lines by sgi_mgras_display_flush() before control
     * leaves the device; not migrated, since loading repaints everything.
     */
    bool dirty;
};

#endif
