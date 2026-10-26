/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_MGRAS_H
#define HW_MISC_SGI_MGRAS_H

#include "hw/1wire/onewire.h"
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
#include "hw/display/sgi/rasterizer.h"

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
 * Established from the GE11 microcode download routine (IRIX 6.5.30
 * mgras_init.o): the kernel loads GE 0 through slot B's ports and GE 1 through
 * slot A's, setting 0x011000 bits 5:4 to 2 and 3 for them. A one-GE board uses
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
/* The raster engine's host data port: a 64-bit unit's high, then low word. */
#define SGI_MGRAS_RE_DATA_HI 0x02d1c0
#define SGI_MGRAS_RE_DATA_LO 0x02c1c4

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
 * PP1SELECT picks which PP1 the XMAP registers reach. Established from the
 * IRIX 6.5.30 kernel: it writes ((re << 1) | pp) << 1 before a sequence meant
 * for one PP1 and 1 before writes every PP1 must take. Two PP1s sit behind
 * each RE, so a two-RE board has four. The model keeps a copy per PP1 of the
 * index byte 0x071c88 and of 0x071d00, 0x071d08, 0x071d80, 0x071e00 and
 * 0x071e80. It keeps none of 0x071f00 and 0x071f80, which read the same fixed
 * values under every selection: an approximation of the model.
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
 * plain storage apart from the pixel mask below: an approximation of a DAC
 * whose register map is unknown.
 */
#define SGI_MGRAS_DAC_REGS 0x10000

/*
 * The DAC's pixel mask. The IRIX 6.5.30 X server's screen saver (its
 * screen-blank routine in impactsr.so) blanks the display by writing 0
 * here and unblanks it by writing 0xff, and SGI's IDE DAC reset programs
 * 0xff.
 * Inferred, as for a common RAMDAC pixel read mask: the scanout ANDs each
 * 8-bit colour component with the mask before the gamma table, so 0 shows
 * gamma entry 0, black under every table IRIX loads. The model resets it
 * to 0xff so a display the guest has not programmed shows its pixels. The
 * cursor's ink, which the model already passes through the gamma table,
 * is masked too; its fixed white outline is not. The part is unidentified;
 * its power-up value, what other masks do, and what reaches the cursor
 * are unknown.
 */
#define SGI_MGRAS_DAC_REG_PIXEL_MASK 0x0400

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
 * desktop-session captures: bit 0 enables the overlay for that WID and
 * bits 7:3 select its colour map -- WID 1's 0xc9 names map 25, the map the
 * server loads with the popup-menu palette (entry 1 grey 0xc1c1c1, entry 2
 * black) that menu fills and text on the overlay visual draw with. Bit 8
 * (set for WIDs 4 and 5 only) is unexplained.
 */
#define SGI_MGRAS_XMAP_OVERLAY_MODE 0x071e80

/*
 * The swap-scheduling routine writes this XMAP-indexed port with the per-window
 * swap mask that takes effect at the next retrace. SGI's IDE XMAP test writes
 * and reads it back under index byte 0 (all 32 bits, the main buffer
 * select) and index byte 4 (low 8 bits, which it calls the overlay buffer
 * select); the kernel reading takes index 4 as a second swap-mask word.
 * The probe's swap captures (a captured run) write
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

/*
 * The MicroLAN master for the board's serial-number chip: a 32-bit word,
 * low-drive time in bits 19:10, sample offset in bits 9:2, done in bit 1
 * and the sampled wire level in bit 0.
 */
#define SGI_MGRAS_NIC 0x011098
#define SGI_MGRAS_NIC_LEVEL (1u << 0)
#define SGI_MGRAS_NIC_DONE (1u << 1)
#define SGI_MGRAS_NIC_SAMPLE_SHIFT 2
#define SGI_MGRAS_NIC_PULSE_SHIFT 10
#define SGI_MGRAS_NIC_WRITABLE 0x000ffffcu
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
 * The raster, descriptor, and HQ headers use the limit above. OpenGL matrix
 * uploads need 16 words; captured stipple uploads need 32.
 */
#define SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS 32
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
/* Bit 31 of a slot 2 word: a draw wrote the pixel (sgi-mgras-gl.c). */
#define SGI_MGRAS_SLOT_WRITTEN  0x80000000u

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
    const char *name;           /* the "product" property value */
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
    /* The serial-number chip's board record: name, part and revision. */
    const char *record_name;
    const char *record_part;
    const char *record_revision;
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
 * A GE pixel draw (0x8d20) waiting for its image, which arrives as inline
 * blocks, one a row, or through one host-memory DMA transfer
 * (hw/misc/sgi-mgras-gl.c). The rectangle is in window pixels.
 */
typedef struct SGIMgrasGLPixels {
    uint32_t active;            /* 0 or 1 */
    uint32_t format;            /* MGRAS_GL_PIXELS_* in sgi-mgras-gl.c */
    uint32_t bpp;               /* host bytes a pixel */
    int32_t x, y;
    uint32_t w, h;
    uint32_t row, col;          /* the next pixel */
    uint32_t skip;              /* bytes still to drop from this row's block */
    uint32_t carry;             /* bytes of a pixel split across words */
    uint32_t ncarry;
    uint32_t components[4];
    uint32_t ncomponent;
    uint32_t dma_only;          /* 0x8d20 marked the image as DMA: 0 or 1 */
    uint32_t apart;             /* a header came after its last word: 0/1 */
    /* A pixel-texture pass: its first row, and rows apart (0 means 1). */
    uint32_t pass;
    uint32_t row_step;
    /*
     * A zoomed draw (glPixelZoom): the zoom factors' magnitudes as float
     * bits, 0 when the draw is unzoomed; the destination box's size in
     * window pixels; bit 0 and bit 1 of flip set when x and y run toward
     * lower window coordinates.
     */
    uint32_t zoom_x, zoom_y;
    uint32_t dest_w, dest_h;
    uint32_t flip;
} SGIMgrasGLPixels;

/*
 * The levels a draw reads and the TXADDR-indexed tables
 * (hw/misc/sgi-mgras-texture.c).
 */
#define SGI_MGRAS_TEX_LEVELS 16
#define SGI_MGRAS_TEX_TABLE 32
#define SGI_MGRAS_TEX_BORDER_CACHES (256u * SGI_MGRAS_TEX_LEVELS)

/*
 * TRAM: one raster path's 4 MiB in 256 blocks of 16 KiB, each 64 rows of
 * 64 32-bit words (hw/misc/sgi-mgras-texture.c, "TRAM").
 */
#define SGI_MGRAS_TRAM_BLOCKS 256
#define SGI_MGRAS_TRAM_BLOCK_BYTES (16 * 1024)
#define SGI_MGRAS_TRAM_BYTES \
    (SGI_MGRAS_TRAM_BLOCKS * SGI_MGRAS_TRAM_BLOCK_BYTES)
/*
 * The model's TRAM image: one TRAM's bytes for each of the four TRAM
 * selects a one- or two-component texture can name (hw/misc/
 * sgi-mgras-texture.c, "Storage"). Select 0's image is TRAM as the save
 * and restore see it.
 */
#define SGI_MGRAS_TRAM_SELECTS 4
#define SGI_MGRAS_TRAM_IMAGE_BYTES \
    (SGI_MGRAS_TRAM_SELECTS * SGI_MGRAS_TRAM_BYTES)

/*
 * One level of a draw as the sampler reads it out of TRAM: the bit its
 * first texel starts at, its log2 width and height and the kept format in
 * TL_MODE's layout and depth fields. Host-only, rebuilt for each draw.
 */
/* Software storage is bounded by the largest captured RGBA8 volume. */
#define SGI_MGRAS_VOLUME_TEX_BASE_BYTES 4096u
#define SGI_MGRAS_VOLUME_TEX_BYTES 65536u
#define SGI_MGRAS_TEXTURE_COLOR_TABLE_ENTRIES 256u
#define SGI_MGRAS_TEXTURE_COLOR_TABLE_BYTES \
    (3u * SGI_MGRAS_TEXTURE_COLOR_TABLE_ENTRIES)

typedef struct SGIMgrasTexView {
    const uint8_t *tram;
    const uint8_t *volume_data;
    const uint8_t *volume_data_extra;
    uint64_t bit;
    uint32_t log2w, log2h;
    /*
     * A TRAM volume's slices go two to a TL_SPEC-shaped level twice the
     * slice's width, pair_bits apart: row_log2w is that level's log2
     * width. A two-dimensional level has row_log2w == log2w and no pairs.
     */
    uint32_t row_log2w;
    uint64_t pair_bits;
    uint32_t mode;
    uint32_t format;
    uint32_t depth;
    uint8_t component_bits[4];
    const uint64_t *border_data;
    uint32_t level, border_offset, border_count;
} SGIMgrasTexView;

/*
 * One GL context's SI+T texture state: the TE1 registers the client
 * writes, the load under way, and the levels loaded so far.
 */
typedef struct SGIMgrasTexture {
    uint32_t texmode1, texmode2;
    uint32_t txsize, txlod, txclampsize, mddma_cntrl;
    uint32_t txenv[2];          /* 0x142, 0x143 */
    uint32_t txbcolor[2];
    uint32_t txaddr;
    uint32_t txmipmap[SGI_MGRAS_TEX_TABLE];
    uint32_t txborder[SGI_MGRAS_TEX_TABLE];
    uint32_t tl_mode, tl_spec;
    uint32_t tl_wbuffer, texrbuffer;
    uint32_t tl_addr;
    uint32_t tl_mipmap[SGI_MGRAS_TEX_TABLE];
    uint32_t base;
    uint32_t t_offset;          /* the 0x5f04 float's bits */
    uint32_t on;                /* 0x0b00 came after the last 0x0c00: 0/1 */
    uint32_t bracket;           /* inside 0x9b00 ... 0x9c00: 0 or 1 */
    uint32_t wide_branch;       /* the client's wide-level branch: 0 to 2 */
    uint32_t loaded_levels;      /* levels with committed interior uploads */
    /* Bracketed loads closed since TL_SPEC was last written. */
    uint32_t tl_slices;
    /*
     * A 0x511a transfer waiting for its DMA: 0 or 1, and what it loads:
     * the level, the rectangle of it, x, y and h << 16 | w, the image's
     * components a texel and bytes a component, and whether the image's
     * components come in GL_ABGR_EXT order: 0 or 1.
     */
    uint32_t load;
    uint32_t load_base, load_block, load_spec, load_mode;
    uint32_t load_x, load_y, load_size, load_depth, load_volume;
    uint32_t load_comps, load_csize, load_abgr;
    uint32_t load_slice;        /* the volume slice the transfer loads */
    uint32_t load_packet[8];
    uint64_t load_dma_bytes;
    uint32_t volume_base, volume_mode, volume_level;
    uint32_t volume_width, volume_height, volume_depth, volume_bytes;
    /* One volume image, kept as two fields that migrate separately. */
    uint8_t volume_data[SGI_MGRAS_VOLUME_TEX_BASE_BYTES];
    uint8_t volume_data_extra[SGI_MGRAS_VOLUME_TEX_BYTES -
                              SGI_MGRAS_VOLUME_TEX_BASE_BYTES];
    uint32_t color_table_selector, color_table_written;
    uint32_t color_table_active, color_table_valid;
    uint8_t color_table_data[SGI_MGRAS_TEXTURE_COLOR_TABLE_BYTES];
    /*
     * Host-only: the host bytes of a transfer, gathered row by row inside
     * the one port write that kicks it, so never migrated.
     */
    uint8_t *staging;
    uint32_t staged, staging_bytes;
    /* Host-only: the sampler's view of the levels, rebuilt for each draw. */
    SGIGLTexPyramid pyramid;
    SGIGLTexLevel sampler_levels[SGI_MGRAS_TEX_LEVELS];
    SGIMgrasTexView views[SGI_MGRAS_TEX_LEVELS];
    uint32_t border_base[SGI_MGRAS_TEX_BORDER_CACHES];
    uint32_t border_mode[SGI_MGRAS_TEX_BORDER_CACHES];
    uint32_t border_width[SGI_MGRAS_TEX_BORDER_CACHES];
    uint32_t border_height[SGI_MGRAS_TEX_BORDER_CACHES];
    uint32_t border_offset[SGI_MGRAS_TEX_BORDER_CACHES];
    uint32_t border_count[SGI_MGRAS_TEX_BORDER_CACHES];
    uint32_t border_valid_count[SGI_MGRAS_TEX_BORDER_CACHES];
    uint64_t border_start[SGI_MGRAS_TEX_BORDER_CACHES];
    uint64_t border_bits[SGI_MGRAS_TEX_BORDER_CACHES];
    uint64_t *border_data;
    uint32_t border_words;
} SGIMgrasTexture;

/*
 * The family's side of one GL context-table entry: where the main-port
 * parser stood when the context was last switched out, beside the entry's
 * GL state. SGIGLContext is the GL state machine's; the parser position is
 * MGRAS framing and stays out of it.
 */
typedef enum SGIMgrasClientReadBank {
    MGRAS_CLIENT_READ_FRONT = 0,
    MGRAS_CLIENT_READ_BACK = 1,
    MGRAS_CLIENT_READ_UNSUPPORTED = 2,
    MGRAS_CLIENT_READ_DEEP_FRONT = 3,
    MGRAS_CLIENT_READ_DEEP_BACK = 4,
} SGIMgrasClientReadBank;

typedef struct SGIMgrasGLContext {
    /* The parser's position while this context is not loaded. */
    SGIMgrasCmdParser parser;
    SGIGLContext *gl;
    uint32_t client_read_bracket;
    uint32_t client_read_kicked;
    uint32_t client_read_bank;
    /*
     * The four words the last 0xef08 or 0xee08 put where the 0x3ba to
     * 0x3bd queries read them (hw/misc/sgi-mgras-gl.c).
     */
    uint32_t readback[4];
    uint32_t clip_query[2];
    /*
     * IRIS GL's colour and normal rule (hw/misc/sgi-mgras-gl.c): whether
     * the board applies it, and whether a colour came after the last
     * normal. Each is 0 or 1.
     */
    uint32_t iris_rule;
    uint32_t iris_colour_last;
    /*
     * glTexGen's plane (hw/misc/sgi-mgras-gl.c): 1 between a 0x5b08 and
     * the 0x5c10 that carries the plane it names, with its coordinate
     * and parameter.
     */
    uint32_t tex_gen_plane_open;
    uint32_t tex_gen_plane[2];
    /* A pixel draw under way in this context. */
    SGIMgrasGLPixels pixels;
    /* Texture state, used on a board with TRAM. */
    SGIMgrasTexture tex;
} SGIMgrasGLContext;

#define SGI_MGRAS_GL_SELECTION_NAMES 32
#define SGI_MGRAS_GL_SELECTION_HITS 32

typedef struct SGIMgrasGLSelectionHit {
    uint32_t name_count;
    uint32_t depth_min;
    uint32_t depth_max;
    uint32_t names[SGI_MGRAS_GL_SELECTION_NAMES];
} SGIMgrasGLSelectionHit;

typedef struct SGIMgrasGLSelection {
    bool active;
    bool result_phase;
    bool pending;
    bool overflow;
    uint32_t name_count;
    uint32_t hit_count;
    uint32_t depth_min;
    uint32_t depth_max;
    uint32_t names[SGI_MGRAS_GL_SELECTION_NAMES];
    SGIMgrasGLSelectionHit hits[SGI_MGRAS_GL_SELECTION_HITS];
} SGIMgrasGLSelection;

/*
 * The GE parameter words the client's 0x80 and 0xcd packets load, by the
 * address each load names (hw/misc/sgi-mgras-gl.c). Every address the
 * IRIX 6.5.30 client was seen to load is below this.
 */
#define SGI_MGRAS_GE_PARAM_WORDS 0x1000

/*
 * Entry 0 is the state used while no GL context is loaded; the others
 * hold one context each, keyed by its context-image word 0.
 */
#define SGI_MGRAS_GL_CONTEXTS 33

/* General interrupt source 10: the swap command has been reached. */
#define SGI_MGRAS_PENDING_SWAP 0x00000400

/* The display IDs of the first @columns columns of line @y. */
void sgi_mgras_display_did_line(SGIMgrasState *s, bool overlay,
                                unsigned int y, uint8_t *did,
                                unsigned int columns);

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

/* Whether `name` is a board the "product" property accepts. */
bool sgi_mgras_board_known(const char *name);

struct SGIMgrasState {
    SGIXioBoardState parent_obj;
    MemoryRegion regs;
    MemoryRegion *fabric;
    AddressSpace fabric_as;
    /*
     * Bytes per DMA page-list entry, 8 or 4, chosen by the machine that
     * fits the board: see mgras_raster_dma_setup().
     */
    uint8_t page_list_entry_bytes;
    /*
     * The master node's graphics credits (sgi-pi-gfx), which the machine
     * links when it places the board. Each processor store the node
     * counted on its way to the port returns one credit there as soon as
     * the handler has taken it, since nothing here queues port words.
     */
    Object *pi;
    /* Host-only: the missing link has been logged once. */
    bool pi_unlinked_logged;
    /*
     * Host-only, fixed once the board is placed: the link's credit hook
     * and the widget it is told, so that a store does not look up the
     * node's interface class again. NULL sends the credit through
     * sgi_pi_gfx_return_credits(), which logs a node without the hook.
     */
    void (*pi_return_credits)(Object *node, unsigned int widget,
                              uint32_t credits);
    unsigned int pi_widget;
    /*
     * The board's serial-number chip behind the MicroLAN register at
     * 0x011098. Without a fitted record the register keeps the older
     * always-done answer.
     */
    OneWireBus *nic_bus;
    bool board_record;
    char *board_record_barcode;
    uint64_t board_record_serial;
    uint32_t nic_control;
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
     * Payload words of a raster-register token the mapping port is still
     * handing to the main command parser.
     */
    uint32_t dma_map_forward_words;
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
    /* A timing table was refused since reset; not migrated. */
    bool timing_problem_logged;
    /*
     * Host-only qtest introspection. Not guest state and not migrated.
     * test_slot picks which of slots 2 and 3 x-test-slot-pixel reads.
     */
    uint32_t test_slot;
    uint32_t test_pixel_x;
    uint32_t test_pixel_y;
    uint32_t test_pixel_count;
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
     * holds only what GL draws, a direct colour or a colour index.
     */
    uint32_t *front;
    uint32_t *back;
    uint8_t *front_alpha;
    uint8_t *back_alpha;
    uint32_t front_count;
    uint32_t back_count;
    /*
     * Colour-buffer slots 2 and 3 of a two-RE board, written by GL's
     * draw-buffer bits 2 and 3 (hw/misc/sgi-mgras-gl.c); NULL, with a zero
     * slot_count, on a one-RE board. Direct colour or an index, with no
     * format tag. A pixel of slot 2 that a draw wrote carries
     * SGI_MGRAS_SLOT_WRITTEN above its 24 colour bits, which tells scanout a
     * deep window's pixel from a shallow one; slot 3 is never scanned out.
     */
    uint32_t *slot[2];
    uint8_t *slot_alpha[2];
    uint32_t slot_count;
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
     * values above: CI8 selects the indexed plane (front holds a CMAP
     * index, eight bits or a 12-bit GL context's twelve), RGB selects
     * packed colour (front holds 0x00RRGGBB directly) and RGB12 a 12-bit
     * window's colour (front holds 0xBGR, four bits a channel). No
     * register exposes a stored per-pixel format; painting code derives
     * it from PP1FILLMODE at the time each pixel is written and records it
     * here so scanout can resolve mixed-format content within one WID.
     * This is a model approximation.
     */
    uint8_t *format;
    /*
     * The same tag for bank 1, where only GL draws: CI8 for a colour
     * index, RGB for a direct colour.
     */
    uint8_t *back_format;
    /* Per-pixel overlay index. 0 is transparent and shows the main planes. */
    uint8_t *overlay;
    /*
     * Per-pixel clip ID, 0 to 3, which the X server writes over a GL
     * window's visible region and a GL context may require; see
     * mgras_fill_clip_id() and mgras_gl_owns().
     */
    uint8_t *clip_id;
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
    uint32_t public_glyph_row;
    bool public_glyph_armed;
    bool public_glyph_wide_valid;
    uint32_t pio_x;
    uint32_t pio_y;
    /*
     * A host read through the raster engine's data port (sgi-mgras-raster.c,
     * mgras_pio_read_start()): its box, top row in screen rows, the next
     * 64-bit unit and the low word the last high-word read latched.
     */
    bool pio_read_active;
    uint32_t pio_read_x;
    uint32_t pio_read_y;
    uint32_t pio_read_w;
    uint32_t pio_read_h;
    uint32_t pio_read_next;
    uint32_t pio_read_low;
    /*
     * The RE-count token a context setup sends (0x8c04): its last payload
     * and how many arrived since reset. Recorded, not acted on.
     */
    uint32_t gl_re_count_word;
    uint32_t gl_re_count_tokens;
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
    SGIMgrasGLSelection gl_selection[SGI_MGRAS_GL_CONTEXTS];
    uint32_t gl_slot[SGI_MGRAS_GL_CONTEXTS];
    uint64_t gl_selected[SGI_MGRAS_GL_CONTEXTS];
    uint64_t gl_selections;
    uint32_t gl_current;
    /*
     * The kernel's moves of GL contexts out of and back into GE external
     * RAM (sgi-mgras-gl.c, "Context eviction"). gl_is_saved[i] says
     * entry i's context was read out and not yet written back, and then
     * gl_saved[i] is the host address it went to: zero is a valid host
     * address, so it cannot stand for "not saved". Neither means anything
     * while the entry holds its slot. eram_* is the transfer between a
     * DMA kick and the ERAM token that names its GE address, and the
     * restore that later chunks of one image continue.
     */
    uint64_t gl_saved[SGI_MGRAS_GL_CONTEXTS];
    bool gl_is_saved[SGI_MGRAS_GL_CONTEXTS];
    uint32_t eram_state;
    uint64_t eram_host;
    uint32_t eram_rows;
    uint32_t eram_bpr;
    uint32_t eram_next_addr;
    /*
     * TRAM and its save and restore (sgi-mgras-texture.c, "TRAM" and "TRAM
     * save and restore"): the bytes, allocated on every board and used on
     * one with texture; which blocks a load or restore has written since
     * reset; the transfer between the word that opens it and its DMA kick;
     * its TRAM rows or GE words and, for the client's restore, its first
     * row; the absolute blocks the last TXMIPMAP and TL_MIPMAP entry 0
     * named; and the kernel's own table indices and row count, which its
     * register pairs carry outside any GL context.
     */
    uint8_t *tram;
    uint32_t tram_written[SGI_MGRAS_TRAM_SELECTS * SGI_MGRAS_TRAM_BLOCKS / 32];
    uint32_t tram_transfer;
    uint32_t tram_rows, tram_words, tram_first_row;
    uint32_t tram_read_block, tram_load_block;
    uint32_t tram_kernel_txaddr, tram_kernel_tladdr, tram_kernel_rows;
    /* Host ownership and bounded diagnostics; never serialized. */
    bool gl_restore_in_progress;
    bool gl_cleanup_registered;
    bool gl_oom_logged;
    /* Host Mesa worker and provider diagnostics, never migrated. */
    SGIRenderer *gl_renderer;
    bool gl_renderer_opt_in;
    bool gl_renderer_unavailable; /* Failure latched for this device. */
    bool gl_renderer_scalar; /* No Mesa on this host: every draw scalar. */
    uint32_t gl_render_create_fault; /* Host-only qtest creation result. */
    /* The current checked command ended on Mesa, accepted or failed. */
    bool gl_render_validated;
    uint32_t gl_render_op; /* The GL command decoding, for traces. */
    /* Draws Mesa refused and the scalar path drew, by reason. */
    uint64_t gl_mesa_fallbacks[SGI_RENDER_REFUSAL__MAX];
    uint64_t gl_render_snapshots;
    uint64_t gl_render_commits;
    uint32_t gl_render_result;
    uint32_t gl_render_inject_result; /* Host-only qtest fault. */
    uint32_t gl_scalar_inject_oom; /* One-shot host-only scalar fault. */
    uint32_t gl_render_inject_commit_failure; /* One-shot host-only fault. */
    /* GE-port traffic logged since the last switch; not migrated. */
    bool gl_ge_port_logged;
    /* The bank holding the back buffer, from 0x9808 or context words 16-17. */
    uint32_t gl_banks[2];
    /* The GE's pixel parameters, one copy for the board. */
    uint32_t ge_param[SGI_MGRAS_GE_PARAM_WORDS];
    /* Host-only: a load past ge_param has been logged once. */
    bool ge_param_logged;
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
    char *product;
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
     * the timer's expiry.
     * The period follows from migrated VC3 and DAC state, or from the
     * timing subsection when that state no longer gives it.
     */
    SGIScanout scanout;
    /*
     * The running retrace period as the migration subsection carries it
     * (frame_clocks of a clock_hz clock), and whether a load delivered
     * one. Staging for migration only.
     */
    uint64_t timing_frame_clocks;
    uint32_t timing_clock_hz;
    bool timing_loaded;
    /*
     * Something the whole frame resolves from changed. Folded into the
     * scanout's dirty lines by sgi_mgras_display_flush() before control
     * leaves the device; not migrated, since loading repaints everything.
     */
    bool dirty;
};

/*
 * Whether the client's deep front and back read-buffer selections reach a
 * deep bank: on a board with two REs, whose second RE's slots hold the
 * deep visual's pixels. SSI's captured reads ground the rule; MXI, EMXI
 * and ESSI run the same client branch, which picks the pair from the
 * visual's fields, not the board.
 */
static inline bool sgi_mgras_deep_reads(const SGIMgrasState *s)
{
    return s->board->re_count == 2 && s->slot[0];
}

#endif
