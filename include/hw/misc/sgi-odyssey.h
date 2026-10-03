/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI Odyssey (VPro) graphics board on an XIO slot. See hw/misc/sgi-odyssey.c
 * for the modeled surface and what is deliberately left unimplemented.
 */
#ifndef HW_MISC_SGI_ODYSSEY_H
#define HW_MISC_SGI_ODYSSEY_H

#include "hw/1wire/onewire.h"
#include "hw/display/sgi/board-memory.h"
#include "hw/display/sgi/gl-context.h"
#include "hw/display/sgi/head-ops.h"
#include "hw/display/sgi/pixel-dma.h"
#include "hw/display/sgi/scanout.h"
#include "hw/mips/sgi/xio-board.h"
#include "system/memory.h"
#include "qom/object.h"
#include "ui/console.h"

#define TYPE_SGI_ODYSSEY "sgi-odyssey"
OBJECT_DECLARE_TYPE(SGIOdysseyState, SGIOdysseyClass, SGI_ODYSSEY)

/*
 * Distinct unimplemented offsets reported before the device falls silent
 * about them. Diagnostic bookkeeping, not guest state.
 */
#define SGI_ODYSSEY_UNIMP_REPORTS 64

/*
 * The vertices an integer GL primitive holds while it streams: a QUADS
 * rectangle's four, or a strip's first and previous vertex. The board's
 * command FIFO has no batch size, so a primitive of any length streams
 * through these (hw/misc/sgi-odyssey.c, odyssey_gl_vertex()).
 */
#define SGI_ODYSSEY_GL_HELD_VERTICES 4

/*
 * The operands a register-select word's gather keeps: the arity field's
 * largest count, a 16-word burst, so every operand of every word is kept
 * (the window-clip register 0x204b carries eight).
 */
#define SGI_ODYSSEY_CMD_OPERANDS 16

/* The window-clip register's four rectangles, two words each. */
#define SGI_ODYSSEY_CLIP_RECTS 4

/*
 * The geometry engine's program state (docs/analysis/odyssey-gl-geometry-
 * program.md): constant-memory entries an 11-bit index reaches, the four
 * entry words (0x29, 0x2a, 0x2c, 0x33), eight lights, the class 0x32 colours
 * (three per light and the light-model ambient at 24), and how many distinct
 * unrecognised programs are reported before the model falls silent.
 */
#define SGI_ODYSSEY_GL_CONSTANTS 2048
/* Off-screen board-memory objects the model keeps (texture levels). */
#define SGI_ODYSSEY_OBJECTS 64
/* The texture level table's entries (0x0e80 .. 0x0ebf). */
#define SGI_ODYSSEY_TEX_LEVELS 64
#define SGI_ODYSSEY_TEX_COLOUR_TABLE_ENTRIES 256
#define SGI_ODYSSEY_GL_ENTRIES 4
#define SGI_ODYSSEY_GL_LIGHTS 8
#define SGI_ODYSSEY_GL_LIGHT_COLOURS 25
#define SGI_ODYSSEY_GL_PROGRAM_REPORTS 16
/*
 * Program memory: 2048 instructions of six halfwords, of which the window
 * at 0x738 is the last 0xc7 (hw/misc/sgi-odyssey.c, ODSY_PROG_WINDOW_BASE).
 */
#define SGI_ODYSSEY_GL_PROGRAM_ENTRIES 2048
#define SGI_ODYSSEY_GL_WINDOW_ENTRIES 0xc7

/*
 * The one modeled mode: the SI/desktop target size a later increment's
 * command interpreter will render into. A real V12 board supports other
 * VFO-described modes; picking one fixed size to model first is scope, not
 * a claim that it is the only mode the hardware supports.
 */
#define SGI_ODYSSEY_WIDTH 1280
#define SGI_ODYSSEY_HEIGHT 1024

typedef struct SGIOdysseyOpenVertex {
    uint32_t word;
    uint32_t operands[4];
    uint32_t attribute[19][5];
    uint32_t slot;
} SGIOdysseyOpenVertex;

struct SGIOdysseyState {
    SGIXioBoardState parent_obj;

    MemoryRegion regs;
    /*
     * The crosstalk fabric this board's interrupt packets travel: the
     * host's own inbound window, reached through whichever machine glue
     * points this link at it (hw/misc/sgi-mgras.c's own "fabric" carries
     * the same contract). A packet carries its destination widget in
     * address bits 51:48 (hw/mips/sgi/xio-widget.h).
     */
    MemoryRegion *fabric;
    AddressSpace fabric_as;
    OneWireBus *nic_bus;

    /* Master node receiving credits for counted processor stores. */
    Object *pi;

    /* Properties. */
    uint32_t revision;
    uint32_t memory_size;       /* megabytes */
    uint32_t memory_banks;
    uint32_t cas_latency;
    char *profile;
    uint64_t nic_serial;
    char *nic_record;

    /* Composed at realize from the properties above. */
    uint32_t memory_config;

    /* Modeled registers. */
    uint32_t widget_control;
    uint32_t xt_req_timeout;
    uint32_t int_dest_upper;
    uint32_t int_dest_lower;
    uint32_t error_command;
    uint32_t intr_flags;
    uint32_t intr_enable;
    uint32_t flctl_config;
    uint32_t sw_sync;

    /*
     * The routed-interrupt destination and per-source vector table
     * (ODSY_ROUTE_DEST_UPPER/_LOWER/_VECTORS in hw/misc/sgi-odyssey.c) --
     * distinct from int_dest_upper/int_dest_lower above, which the one
     * source this device modeled before now used. This increment adds a
     * second routed source, softsync, delivered through this table.
     */
    uint32_t route_dest_upper;
    uint32_t route_dest_lower;
    uint32_t route_vectors;

    /*
     * The DMA-sync command's (ODSY_REG_HOST_WRITE, 0x0a5a) host-write base:
     * a 48-bit guest physical address the command's operand offsets into.
     */
    uint32_t host_write_addr_upper;
    uint32_t host_write_addr_lower;

    /*
     * The kernel driver's context switch save address (ODSY_CTX_SHADOW_ADDR_
     * UPPER/_LOWER, 0x4004/0x400c): a 48-bit guest physical address split
     * the same way as host_write_addr_upper/lower above, the per-context
     * shadow page the switch store at ODSY_CTX_SWITCH_PORT completes into.
     */
    uint32_t ctx_shadow_addr_upper;
    uint32_t ctx_shadow_addr_lower;

    uint32_t buzz[6];
    uint32_t nic;
    uint32_t i2c_main_cmd;
    uint32_t i2c_opt_cmd;

    /*
     * The swap-control register bank (ODSY_SWAP_ARM and companions in
     * hw/misc/sgi-odyssey.c, docs/analysis/odyssey-swap-wake-mechanism.md),
     * one bit per window in each. swap_arm accumulates the windows whose
     * displayed buffer flips at the next retrace; odyssey_retrace() XORs it
     * into displayed_buffer, which the driver's retrace ISR reads back.
     * swap_control and swap_control_companion set and clear displayed_buffer
     * bits at once (the immediate path); swap_pending is stored but inert.
     */
    uint32_t swap_arm;
    uint32_t swap_pending;
    uint32_t swap_control;
    uint32_t swap_control_companion;
    uint32_t displayed_buffer;

    /*
     * The display back end's colour lookup RAM and per-drawable display-mode
     * registers, both programmed through the ODSY_BACKEND_SELECT word stream
     * (hw/misc/sgi-odyssey.c, docs/analysis/odyssey-colormap-format.md). lut
     * holds one 30-bit colour per entry (ten bits per channel); dispmode[d]
     * bits 8:4 name the colormap group a drawable's indexed pixels resolve
     * through. backend_index and backend_count carry the counted-record
     * parser's position across successive stores to the port.
     */
    uint32_t lut[0x2000];
    uint32_t dispmode[32];
    uint32_t backend_index;
    uint32_t backend_count;

    /*
     * Command-FIFO self-test: how far through the kernel driver's
     * command-FIFO initialisation the ports have seen, and whether
     * 0x201594 answers OK yet.
     */
    uint32_t selftest_progress;
    bool cfifo_selftest_ready;

    /* The one modeled source's transition state, for the packet edge below. */
    bool interrupt_asserted;

    /*
     * The modeled colour framebuffer: one fixed SGI_ODYSSEY_WIDTH x
     * SGI_ODYSSEY_HEIGHT surface, colour buffer 0's RGBA8 plane over the
     * shared graphics foundation's board memory (hw/display/sgi/
     * board-memory.h) rather than a standalone array. The interpreter and
     * the test-only accessor paint it through sgi_plane_store/sgi_plane_load;
     * the scanout resolves it through the head's resolve_row hook.
     *
     * Device-backed -- a model-owned byte array the size of one surface,
     * byte-for-byte the surface this model always drew. It is deliberately
     * not page-backed onto guest RAM: the real board's framebuffer base,
     * stride and per-depth channel packing are unlocated
     * (docs/research/graphics/odyssey-display-backend-survey-20260922.md),
     * so page-backing would invent the framebuffer's guest placement. The
     * plane is declared with the red-high packing this model approximates.
     * Unlike the old array, the rendered contents now migrate, through the
     * foundation's vmstate subsection.
     */
    SGIBoardMemory mem;
    uint32_t colour_plane;      /* the id sgi_board_memory_add_plane returned */

    /*
     * The second colour buffer: buffer 1 in the displayed-buffer bits'
     * sense, mem above being buffer 0. A board memory of its own, the same
     * shape as mem, so a stream from before it existed still loads with
     * buffer 1 at its reset value. Model storage only: where the board
     * keeps its two buffers, and how it packs them, is unknown
     * (docs/datasheets/odyssey.md, "Buffer-selection software contract").
     */
    SGIBoardMemory buffer1_mem;
    uint32_t buffer1_plane;

    /*
     * Buffer selection. draw_mask is the last 0x0424 operand, the drawing
     * mask the kernel resolves from the client's virtual one; draw_word and
     * read_word the last 0x608 and 0x609 words, the bound drawing and
     * reading drawables. Only draw_mask steers drawing; the two words are
     * kept raw because their decoding is not known.
     */
    uint32_t draw_mask;
    uint32_t draw_word;
    uint32_t read_word;
    bool draw_mask_stereo_reported;
    bool draw_mask_high_reported;

    /*
     * The board-memory objects (hw/misc/sgi-odyssey.c, ODSY_OBJ_POOL_BYTES):
     * off-screen buffers a 0x1017 select or a DMA header names by board base,
     * each a run of 0xRRGGBBAA words in obj_mem -- obj_width pixels a row,
     * obj_rows rows at word obj_offset -- the first obj_used words of which
     * are taken. dest_base is the last 0x1017 select's base. Device-backed
     * like the colour plane, for the same reason, and migrated. dma_seq tags
     * the DMA engine's replay-trace chunks.
     */
    SGIBoardMemory obj_mem;
    uint32_t dest_base;
    uint32_t obj_count;
    uint32_t obj_used;
    uint32_t obj_base[SGI_ODYSSEY_OBJECTS];
    uint32_t obj_width[SGI_ODYSSEY_OBJECTS];
    uint32_t obj_offset[SGI_ODYSSEY_OBJECTS];
    uint32_t obj_rows[SGI_ODYSSEY_OBJECTS];
    bool obj_full_reported;
    /* A destination base beyond the active profile's board memory. */
    bool board_oob_reported;

    /*
     * The texture state words as last pushed (hw/misc/sgi-odyssey.c,
     * ODSY_REG_TEX_*): tex_mode 0x405, tex_sampler 0x1c16, tex_env 0x141b,
     * tex_levels the 0x0e80 table, three words an entry, tex_level0 0x406,
     * tex_lod the four 0x1013 words,
     * tex_coord the last 0xc380c821 (s, t) as float bits, untransformed;
     * tex_coord_r retains the 3D coordinate. The remaining flag
     * records the unsupported on-board mipmap rectangle report.
     */
    uint32_t tex_mode;
    uint32_t tex_sampler[7];
    /*
     * The two PIO colour-table planes used by the observed L8 sampler
     * selectors. Each word retains both raw 12-bit fields; valid marks the
     * sixteen uploaded 16-word blocks in each plane.
     */
    uint32_t tex_colour_table[2][SGI_ODYSSEY_TEX_COLOUR_TABLE_ENTRIES];
    uint32_t tex_colour_table_valid[2];
    uint32_t tex_env[5];
    uint32_t tex_levels[SGI_ODYSSEY_TEX_LEVELS * 3];
    uint32_t tex_level0;
    uint32_t tex_lod[4];
    uint32_t tex_coord[2];
    uint32_t tex_coord_r;
    bool tex_rect_reported;
    uint64_t dma_seq;

    /*
     * The per-pixel drawable-id plane, one byte per framebuffer pixel. The
     * board carries this alongside the colour planes: bits 7:3 name the
     * drawable that owns the pixel and bits 2:0 a sub-field for ids 32 and
     * up, and the display-mode register for that id (dispmode[]) selects the
     * pixel format and colormap group the pixel displays through. The X
     * server's drawable-id plane write moves a window's screen area to its
     * drawable when the window is mapped; the model routes those writes here
     * instead of painting the colour framebuffer, and scanout resolves each
     * pixel through the drawable it belongs to when the frame is shown
     * (docs/analysis/odyssey-colormap-scanout.md). Allocated at realize
     * beside framebuffer, cleared at reset.
     */
    uint8_t *did;

    /*
     * The command-FIFO interpreter (0x110000), reused across successive
     * writes so a multi-word command can span them. Its command grammar
     * follows OpenBSD's public odyssey(4) driver; each word is described
     * at its handler in hw/misc/sgi-odyssey.c.
     *
     * query_current[] retains each public current-attribute record as its
     * last command word and that word's raw operands, at most four. The
     * context save writes records 0 and 1 back to the shadow page.
     */
    uint32_t query_current[5][5];
    uint32_t query_captured[5][5];
    uint32_t query_current_valid;
    uint32_t query_captured_valid;
    uint32_t query_stage;
    uint32_t query_count;
    uint32_t query_sync_index;
    uint64_t query_epoch;
    uint64_t query_captured_epoch;
    uint64_t query_base;
    uint64_t query_sync_base;

    uint32_t cmd_state;
    uint32_t cmd_tag;
    uint32_t cmd_operand[SGI_ODYSSEY_CMD_OPERANDS];
    uint32_t cmd_operand_count;
    uint32_t cmd_operand_index;

    /*
     * Where the current context's window is, as the kernel pushes it after
     * every context switch and clip change (hw/misc/sgi-odyssey.c,
     * docs/analysis/odyssey-gl-window-placement.md): win_origin the screen
     * pixel of the window's top-left corner (register 0x841, sign-extended
     * 13-bit fields), clip_words the eight raw operands of the four inclusive
     * clip rectangles (0x204b) and did_compare the drawable-id compare word
     * (0x840's second operand). clip_kept and clip_rect are derived from
     * clip_words -- the non-empty rectangles -- rebuilt on every 0x204b and
     * after migration, so they are not migrated themselves.
     */
    int32_t win_origin[2];
    /* The bound reading drawable's 0x60b origin survives scratch selects. */
    int32_t read_origin[2];
    uint32_t clip_words[2 * SGI_ODYSSEY_CLIP_RECTS];
    uint32_t did_compare;
    uint32_t clip_kept;
    SGIClipRect clip_rect[SGI_ODYSSEY_CLIP_RECTS];
    /*
     * The window as the kernel last pushed it on its own port, which the
     * 0x90008116 word restores after a user-port override (hw/misc/
     * sgi-odyssey.c, ODSY_CMD_WINDOW_RESTORE). cmd_from_kernel says which
     * port the word being parsed came from; it is set on every write, so it
     * is not migrated.
     */
    int32_t win_committed_origin[2];
    uint32_t win_committed_clip[2 * SGI_ODYSSEY_CLIP_RECTS];
    uint32_t win_committed_did;
    bool cmd_from_kernel;

    /*
     * The 2D stream's GL immediate-mode subset: the primitive BEGIN named,
     * the vertices it still holds (gl_vertex_x/y, SGI_ODYSSEY_GL_HELD_VERTICES)
     * and how many it has had since BEGIN. Each piece is drawn as its last
     * vertex arrives (odyssey_gl_vertex()); END closes a loop or polygon.
     * line_stipple holds the three 0x0c48 operands. Float GL uses word 0's
     * low 16 bits and word 2's low 8 bits; integer 2D uses word 1's 32 bits.
     *
     * An open POLYGON keeps its edges as crossing marks instead of its
     * vertices: poly_marks has one byte per framebuffer pixel, toggled where
     * an edge crosses a row, and END fills the pixels with an odd count of
     * marks at or left of them. poly_row_lo/poly_row_hi (exclusive) and
     * poly_col_lo bound the marks set, empty when lo >= hi. Allocated at
     * realize; its marks migrate only while a polygon is open.
     */
    uint32_t gl_primitive;
    uint32_t gl_color;
    uint32_t gl_vertex_x[SGI_ODYSSEY_GL_HELD_VERTICES];
    uint32_t gl_vertex_y[SGI_ODYSSEY_GL_HELD_VERTICES];
    uint32_t gl_vertex_count;
    uint32_t line_stipple[3];
    uint8_t *poly_marks;
    uint32_t poly_row_lo;
    uint32_t poly_row_hi;
    uint32_t poly_col_lo;

    /*
     * The true-colour OpenGL path: the shared GL context and software
     * rasterizer (hw/display/sgi/gl-context.h, rasterizer.h). A GL client
     * draws with float vertices and colour (0x8080c801 glVertex2f, 0xc580cc0e
     * glColor3f) and glBegin geometry other than the greeter's integer QUADS;
     * hw/misc/sgi-odyssey.c decodes those words into SGIGLCmd and feeds
     * sgi_gl_command(), which assembles each primitive and rasterizes it into
     * the colour plane. The integer 2D greeter path (gl_primitive/gl_color/
     * gl_vertex_* above, the fill/pattern/image interpreters) is untouched.
     *
     * Apart from the texture matrix, this transient render state is
     * reconstructed from the command stream and not migrated. gl_in_prim
     * records that a GL BEGIN opened a primitive routed here; gl_matrix_slot
     * the shared matrix slot a board-side matrix-select word chose; gl_matrix
     * the 16 float-bit words of a matrix load being gathered
     * (docs/designs/odyssey-gl-family-side.md); gl_texture_matrix the last
     * texture-matrix load as sent, separately migrated and applied to ST;
     * gl_rect
     * the ten float-bit
     * words of a screen-aligned rectangle being gathered
     * (docs/analysis/odyssey-gl-clear-stream.md). The drawable is rebound
     * from win_origin and the clip rectangles above before every command, and
     * the projection and viewport come from the combined matrix (matrix
     * slot 0x00).
     */
    SGIGLContext gl_ctx;
    bool gl_in_prim;
    bool gl_begin_open;
    bool gl_integer_prim;
    uint32_t gl_open_r;
    uint32_t gl_open_c;
    uint32_t gl_open_m3;
    uint32_t gl_open_m4;
    uint32_t gl_open_held;
    SGIOdysseyOpenVertex gl_open_vertex[3];
    uint32_t gl_restart_type;
    uint32_t gl_restart_select;
    uint32_t gl_restart_absorb;
    uint32_t gl_restart_total;
    uint32_t gl_restart_c;
    uint32_t gl_restart_m3;
    uint32_t gl_restart_m4;
    bool gl_restart_armed;
    float gl_restart_vertex[3][SGI_GL_VERTEX_FLOATS];
    uint32_t gl_matrix_slot;
    uint32_t gl_matrix[16];
    uint32_t gl_texture_matrix[16];
    uint32_t gl_rect[10];

    /*
     * Depth (docs/analysis/odyssey-gl-shading-depth-lighting.md). The GL
     * state registers as last written: gl_depth_func 0x420 (bits 2:0 the
     * comparison) and gl_write_mask 0x452 (bit 24 the depth write enable,
     * bits 23:20 the colour write mask). gl_combined is the last slot-0x00
     * load as sent and gl_block16 the four floats behind matrix index 16,
     * whose fourth float's sign orients the depth row, so a later block-16
     * load can rebuild the projection. These migrate, and post_load gives
     * the shared context its projection and depth state back from them.
     * Rectangle report flags are reset diagnostics, not migrated guest state.
     *
     * depth_mem holds the 24-bit depth plane, one word per pixel of the
     * frame. Where the board keeps its depth buffer is not known, so this is
     * a model plane that no aperture reaches, migrated as its own
     * subsection.
     */
    uint32_t gl_depth_func;
    uint32_t gl_write_mask;
    uint32_t gl_combined[16];
    uint32_t gl_block16[4];
    bool gl_rect_depth_logged;
    bool gl_rect_pattern_logged;
    /*
     * Diagnostic only: whether a non-all-ones 0x452 index write mask (bits
     * 11:0) has already been reported in index mode. No retained capture
     * shows what a real colour-index drawable sends there -- every probe is
     * RGBA, where the field is always 0 for the unrelated reason that an
     * RGBA drawable has no index planes -- and the board's own reset value
     * leaves it 0, not GL's own all-ones "fully open" default. Applying the
     * bits as a literal write mask would need to pick one of those as the
     * safe case with no evidence for either, so instead they are decoded,
     * never applied, and reported once whenever they are not all-ones,
     * which includes the board's own reset value.
     */
    bool gl_index_mask_logged;
    SGIBoardMemory depth_mem;
    uint32_t depth_plane;

    /*
     * The scissor and window clip box, 0x081d, as last written: word 0
     * (x_min << 12) | x_max, word 1 (y_min << 12) | y_max
     * (hw/misc/sgi-odyssey.c, ODSY_REG_CLIP_BOX). Migrated; reset to the
     * full window and post_load re-applies it, since the shared context's
     * own scissor box is transient render state and is not migrated.
     */
    uint32_t gl_clip_box[2];
    uint32_t gl_fog[6];

    /*
     * Lighting (docs/analysis/odyssey-gl-shading-depth-lighting.md and
     * odyssey-gl-geometry-program.md). The geometry engine's program state as
     * the library writes it: gl_const the constant-memory list of microcode
     * routine addresses and gl_const_index the next store's index; gl_entry
     * the 0x29, 0x2a, 0x2c and 0x33 entry words, decoded. The lighting data,
     * float bits as sent: gl_light_colour (class 0x32, index 3 i + {ambient,
     * diffuse, specular}, 24 the model ambient) and gl_light_vec (class 0x31,
     * one 16-float block per light), selected by gl_light_colour_sel and
     * gl_light_vec_sel; gl_material the front ambient, diffuse, specular and
     * emission; gl_shininess_sqrt; gl_normal the current normal;
     * gl_normal_matrix slot 0x34 as sent, row-major; gl_colour the current
     * glColor. All migrate.
     *
     * Derived from the program on every 0x2c write and after migration, not
     * migrated: gl_lit, gl_local_viewer, gl_colour_overridden (the shared
     * context's current colour holds a lit result, not gl_colour), and the
     * one-time report state.
     */
    uint32_t gl_const[SGI_ODYSSEY_GL_CONSTANTS];
    uint32_t gl_const_index;
    uint32_t gl_entry[SGI_ODYSSEY_GL_ENTRIES];
    uint32_t gl_light_colour[SGI_ODYSSEY_GL_LIGHT_COLOURS * 4];
    uint32_t gl_light_vec[SGI_ODYSSEY_GL_LIGHTS * 16];
    uint32_t gl_light_colour_sel;
    uint32_t gl_light_vec_sel;
    uint32_t gl_material[16];
    uint32_t gl_shininess_sqrt;
    /* Raw back material retained for idle context save, not lighting. */
    uint32_t gl_back_material[16];
    uint32_t gl_back_shininess_sqrt;
    uint32_t gl_normal[3];
    uint32_t gl_normal_matrix[16];
    uint32_t gl_colour[4];
    /*
     * The model-view (slot 0x24), row-major float bits as sent, for
     * lighting's eye position only; the combined matrix already carries it.
     * Migrated.
     */
    uint32_t gl_modelview[16];
    bool gl_lit;
    bool gl_local_viewer;
    bool gl_colour_overridden;
    uint32_t gl_program_logged[SGI_ODYSSEY_GL_PROGRAM_REPORTS * 3];
    uint32_t gl_program_logged_count;
    /*
     * Program memory as uploaded (0x27 select, 0x40..0x5f halfwords), six
     * halfwords an entry in the image's order, one copy for every context:
     * the kernel re-establishes a context's program window (0x738..0x7fe)
     * itself, re-uploading it from its copy of the image on a switch-in
     * whenever the context's software shadow says it is not loaded, which
     * the switch clears in the previous context's shadow
     * (docs/analysis/odyssey-context-state.md). gl_program_addr is the entry
     * the next halfword goes to and gl_program_half how many of its six have
     * arrived. Both migrate.
     */
    uint16_t gl_program_mem[SGI_ODYSSEY_GL_PROGRAM_ENTRIES * 6];
    uint32_t gl_program_addr;
    uint32_t gl_program_half;
    bool gl_program_mem_reported;
    bool gl_two_sided_logged;
    bool gl_texgen_logged;
    bool gl_clip_logged;
    bool gl_texture_stage_logged;
    bool gl_block_logged;
    /* A context switch inside a primitive has been reported. */
    bool ctx_open_prim_logged;
    /* A failed store into the context shadow page has been reported. */
    bool ctx_shadow_fail_reported;
    /*
     * GL_DITHER, 0x0404 bit 18, as last pushed: derived from the migrated
     * pattern_mode word, not migrated itself. gl_dither_reported: a draw with
     * it set has been reported undithered.
     */
    bool gl_dither;
    bool gl_dither_reported;
    /* Scanout met an overlay drawable id / colormap group 25; reported. */
    bool scanout_overlay_reported;
    bool scanout_group25_reported;

    /*
     * The current fill's colour source. A COLOR_3UB word sets gl_color to a
     * packed RGB pixel and clears gl_color_is_index; a COLOR_INDEX word sets
     * gl_color_index to the raw PseudoColor index and sets the flag, so a
     * QUADS fill stores the index, which scanout resolves through the
     * drawable each pixel belongs to (odyssey_resolve_row()).
     */
    uint32_t gl_color_index;
    bool gl_color_is_index;

    /*
     * The colour-mode words as last sent, class 0x22 then 0x23 (hw/misc/
     * sgi-odyssey.c, ODSY_PROG_COLOUR_MODE): bit 0 of the first puts GL
     * fragments in index mode, where they draw gl_color_index and the plane
     * keeps the raw index. The kernel re-sends them at every context switch,
     * so one copy serves every context. Migrated. colour_mode_reported: an
     * unknown value has been reported; gl_index_lit_reported: a lit program
     * in index mode has.
     */
    uint32_t colour_mode[2];
    bool colour_mode_reported;
    bool gl_index_lit_reported;

    /*
     * The 0x1017 destination-select block's third operand and the 0x1023
     * plane-write-mask block's first operand (mask << 12), recorded for the
     * fill that follows. When the destination word is ODSY_DEST_ID_PLANES the
     * fill is the X server's drawable-id plane write, not a colour fill: it
     * updates the did plane under the rectangle and leaves the framebuffer
     * alone (docs/analysis/odyssey-account-panel-black-band.md).
     */
    uint32_t dest_buffer_word;
    uint32_t plane_write_mask;

    /*
     * The colour buffer's base: the first operand of the last colour 0x1017
     * block (0x2000 for the screen). A DMA whose table header names it paints
     * the colour plane; any other destination lands in the scratch pool
     * (docs/analysis/odyssey-dma-image-header.md).
     */
    uint32_t dma_colour_base;

    /* odyssey_copyrect()'s fixed literal template's three runtime slots. */
    uint32_t copy_src;
    uint32_t copy_size;
    uint32_t copy_dst;

    /*
     * The one-bit pattern the greeter draws its text with: a 32x32-bit
     * bitmap loaded through the 0x3c4e (words 0-15) and 0x3c4f (words 16-31)
     * register bursts, a shape code from the 0x043f operand (0x4 = 32x32,
     * 0x5 = 64x16, 0x6 = 16x64; 0x0 = off), and the 0x0404 mode operand
     * whose bits 3 and 4 enable pattern mode. With pattern mode on, a QUADS
     * fill paints gl_color only where a pattern bit is set instead of a
     * solid rectangle (hw/misc/sgi-odyssey.c,
     * docs/analysis/odyssey-glyph-raster-format.md). pattern_load_offset is
     * the counted-burst parser's write position within the buffer.
     */
    uint32_t pattern_buffer[32];
    uint32_t pattern_shape;
    uint32_t pattern_mode;
    uint32_t pattern_load_offset;

    /*
     * The BEGIN-0xa image raster path (the account icons and the text-field
     * caret; hw/misc/sgi-odyssey.c, docs/analysis/odyssey-glyph-raster-
     * format.md). BEGIN with primitive 0xa opens an image whose destination
     * box comes from the 0xcf80a92f floats: image_x1/image_y1 the top-left
     * pixel, image_x2/image_y2 the exclusive bottom-right. Pixel bursts fill
     * the box in raster order from a cursor (image_cur_x/image_cur_y) that
     * wraps at image_x2 back to image_x1 on the next row. image_fmt is the
     * running burst's pixel format (0x11 direct RGB, 0x10 four colour indices
     * per word, 0x12 two RGBA16 words per pixel for code 0x6b). A per-group
     * transfer header sets image_group_len, the count of valid pixels for
     * byte formats or words for SHORT (0 runs unbounded to the box edge);
     * image_group_painted counts those units. image_code is the header's
     * source format code. image_bytes and image_nbytes carry a partial pixel
     * across burst words. image_code_reported records unknown-code reporting.
     */
    bool image_active;
    uint32_t image_x1;
    uint32_t image_y1;
    uint32_t image_x2;
    uint32_t image_y2;
    uint32_t image_cur_x;
    uint32_t image_cur_y;
    uint32_t image_fmt;
    uint32_t image_group_len;
    uint32_t image_group_painted;
    uint32_t image_code;
    uint32_t image_bytes;
    uint32_t image_nbytes;
    bool image_code_reported;

    /* Host-only qtest introspection. Not guest state and not migrated. */
    uint32_t test_pixel_x;
    uint32_t test_pixel_y;
    uint32_t test_pixel_buffer;
    uint32_t test_object_base;
    uint64_t test_last_interrupt_addr;
    uint64_t test_last_interrupt_value;

    /*
     * The shared scanout (hw/display/sgi/scanout.h): it owns the console, the
     * presented frame, the dirty-line set and the vertical-retrace timer, and
     * resolves the colour plane above through odyssey_head_ops.resolve_row. It
     * is derived from the colour plane and never its source. Its mode, retrace
     * phase, swaps and cursor migrate through a vmstate subsection; the
     * presented frame is repainted from the plane on load, not carried.
     */
    SGIScanout scanout;

    /*
     * Diagnostic bookkeeping, not guest state: register-address reports and
     * command-FIFO word reports (odyssey_unimplemented()/
     * odyssey_cmd_unimplemented() in hw/misc/sgi-odyssey.c) each get their
     * own budget, so a run of unmodeled FIFO words can never fill the
     * table and hide a still-fresh unmodeled register access, or the
     * reverse.
     */
    /*< private >*/
    uint32_t reported_addr[SGI_ODYSSEY_UNIMP_REPORTS];
    unsigned int reported_addr_count;
    uint32_t reported_cmd[SGI_ODYSSEY_UNIMP_REPORTS];
    unsigned int reported_cmd_count;
    /* A line-stipple control other than 32 pixels has been reported. */
    bool line_stipple_reported;
    /* A drawable-id compare has been reported enabled. */
    bool did_compare_reported;
    /* An unmodeled DMA pixel size, and an 0xe03 descriptor, reported. */
    bool dma_bpp_reported;
    bool dma_readback_reported;
    /* A DMA source format this model does not decode has been reported. */
    bool dma_code_reported;
    /*
     * The board-to-host and sentinel transfer reports already made, one bit
     * per ODSY_DMA_REPORT_* reason (hw/misc/sgi-odyssey.c).
     */
    uint32_t dma_read_reported;
};

struct SGIOdysseyClass {
    SGIXioBoardClass parent_class;

    DeviceRealize parent_realize;
    DeviceUnrealize parent_unrealize;
};

/*
 * Publish one aligned big-endian completion scalar to direct writable RAM
 * under the BQL. The scalar must fit entirely in the initial translated
 * memory section. Earlier payload stores precede this release store.
 * Returns 0, or SGI_FAIL_BOUNDS for address alignment/range errors,
 * SGI_FAIL_BUS for composition or mapping failures. Every failure leaves
 * guest memory intact.
 */
int sgi_odyssey_publish_be16(const SGIDMABus *bus, uint64_t addr,
                             uint16_t value);
int sgi_odyssey_publish_be32(const SGIDMABus *bus, uint64_t addr,
                             uint32_t value);

#endif /* HW_MISC_SGI_ODYSSEY_H */
