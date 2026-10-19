/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SI+T MGRAS texture: one TE1 engine and its TRAM, as the IRIX 6.5.30
 * OpenGL client drives them.
 * Every word below is established as what that client and kernel sent
 * to the model, in a captured run (ivview drawing one
 * textured quad); what the board does with each is inferred.
 *
 * What the model does, and the approximations it makes:
 *
 * - Registers. The client writes texture registers as raster register
 *   records, a 0x7c08 packet of a record count and a word count followed
 *   by an inline block of (register, value, shadow address) triples and a
 *   pad word to an even count. The model keeps TEXMODE1 0x111, TEXMODE2
 *   0x180, TXSIZE 0x181, TXLOD 0x183, TXBCOLOR 0x184 and 0x185,
 *   TEXRBUFFER 0x18b, TXCLAMPSIZE 0x18c, TXADDR 0x190, TL_WBUFFER 0x1a0,
 *   TL_MODE 0x1a1 and TL_SPEC 0x1a2 whole, and TXMIPMAP 0x191 and
 *   TXBORDER 0x193 as tables the 0xec10 packets fill from TXADDR on, the
 *   index advancing one a packet (inferred from the single index write
 *   before 32 packets in the texture-memory initialization routine). The shadow
 *   address is the GE's and is not used. Any other record is logged, as the
 *   whole packet was before. Registers are kept as written, without the
 *   diagnostic's width masks.
 *
 * - Load. 0x9b00 opens a load and 0x9c00 closes it, once per iteration of
 *   the client's load loop. Between them TL_MODE names the kept format
 *   and TL_SPEC the level (bits 3:0) and its log2 width and height (7:4,
 *   11:8), and a GE pixel transfer whose 0x8d20 names routine 0x511a
 *   carries the level's host image by DMA, kicked 0xa7. The model takes
 *   that transfer as the level's texels. It reads the level's size from
 *   TL_SPEC. A transfer may carry the whole level or a rectangle of it:
 *   GE parameters 0xe34 and 0xe35 are the rectangle's x and y in the
 *   level and 0xe38 its size word, h << 16 | w. ivview's levels came
 *   whole, 0, 0, w, h with the size word of the level; perfly's 256-wide
 *   levels came as strips 64 texels wide inside one bracket, at x 0, 0x40,
 *   0x80 and 0xc0, each its own DMA transfer whose source stepped 64
 *   texels along the image's rows (established as words; that the board
 *   places each strip at x, y is inferred from them). The model stores
 *   each strip into the level, keeping the strips loaded before it, and
 *   refuses one that does not lie inside the level and the one-texel
 *   border around it. A bordered image (distort's) is sent at x = y = -1
 *   with the level's size plus two in each direction, its strips at -1,
 *   64 ... 448 and 512 for a 512-wide level; the model stores the texels
 *   inside the level and drops the border, approximated: whether the board
 *   keeps them (the load's fifth GE word, the TXBORDER word, may name
 *   their block, inferred) and how a clamped read uses them are unknown.
 *   Words 0xe36 and 0xe37,
 *   the rectangle's size again where the image path loads inclusive
 *   corners, are not read, and neither are the bytes a pixel in XFRMODE,
 *   0xc00080 on these four-byte levels. The kept format is TL_MODE's
 *   layout (one component, luminance alpha, RGB, RGBA) and depth (4, 8 or
 *   12 bits), the host bytes the components in order, a byte each at 4
 *   and 8 bits and a big-endian halfword at 12, the bottom row (t = 0)
 *   first; established for RGBA unsigned bytes, inferred for the rest.
 *   The transfer must be exactly the rectangle's width times height times
 *   the image's bytes a texel, which the unpack block below sizes
 *   (components times bytes a component), not the kept format. Texels of
 *   a level no transfer has reached hold whatever TRAM held there, 0
 *   after a reset (the model's choice; the board's TRAM contents at
 *   reset are unknown).
 *   The pixel program's unpack block is read for the image's shape: its
 *   routine word gives the components (0x4064 four, 0x40f2 three, 0x417e
 *   two, 0x420c one) and its scale word the bytes a component (1/255 one,
 *   1/65535 two), established as words beside matching byte counts in
 *   ivview's, buttonfly's and blast's loads, the meanings inferred. An
 *   image of the kept layout's components, or of three into RGBA (alpha
 *   1, as OpenGL 1.2.1 3.6.4 converts an RGB group), is taken; two-byte
 *   components are read big-endian and reduced to the kept depth (see
 *   "Depth"). Anything else is logged and nothing is stored. The rest of the
 *   program is not read, approximated: only the four- and
 *   three-unsigned-byte ones were traced, so others are taken as the
 *   identity, as routine 0x511a is at the default pixel transfer state, as
 *   the image path takes 0x49d0. TL_MODE is state that persists across
 *   loads, since the client skips it when reloading the image it loaded
 *   last. An unpaired 0x9c00, which the client's wide-level branch sends,
 *   and a 0x511a transfer outside a bracket are logged and not rejected,
 *   except the branch's own transfer, under a TL_SPEC twice as wide as
 *   the level before it, which is refused and stores nothing. An inline
 *   image or a transfer of another byte count is logged and nothing is
 *   stored. The HQ pixel-format word's GL_ABGR_EXT bit (0xe8d
 *   for bytes, 0xe8e for two-byte components) on an image of four components
 *   kept as RGBA reverses each texel's components; the same word reorders
 *   the image path's draw (inferred to apply here too). On two components it
 *   was never traced and is logged, nothing stored.
 *
 * - Storage. A level's texels go into TRAM (see "TRAM" below) at the
 *   block the context's 0xeb04 base and the load's TXMIPMAP word name, in
 *   the model's own layout. The load's GE words at 0xe3d carry the
 *   level's TXMIPMAP word, the first block in 7:0 and bit 8, which every
 *   traced load set on the odd levels of a mipmapped texture (ivview's
 *   three levels loaded 0, 0x100 and 0, perfly's and ivview's longer
 *   pyramids alternate it level by level), and the draw sends the same
 *   word for the level through the TXMIPMAP table: perfly's three
 *   textures loaded and drew 0, 4 and 0xc for level 0 (established as
 *   words; that the board reads the level from the block it was loaded to
 *   is inferred). The draw finds level n by TXMIPMAP entry n, the client
 *   writing the table from the first level, 0 in every trace. Several
 *   textures of a context, and the textures of every context, share the
 *   one TRAM, so a load overwrites whatever another level or another
 *   context kept where it lands; the kernel's TRAM save and restore are
 *   what keeps a context's texels across another context's loads, and
 *   the client's own save and restore what keeps a texture its own
 *   manager evicted.
 *   TRAM select. On a four-TRAM board the client gives every one- and
 *   two-component texture a TRAM select, sent in TL_MODE bits 4:3 on its
 *   loads and TEXMODE1 bits 8:7 on its draws, and places up to four such
 *   textures at the same blocks, told apart only by it (established from
 *   the client; perfly's town database loads two 32-by-32 levels at
 *   TXMIPMAP word 0x1fe with selects 0 and 3). For RGB and RGBA the bits
 *   carry an undefined value and are ignored (inferred: the stock client
 *   would otherwise draw at random). The model keys a one- or
 *   two-component level by its select as well as its block: each select
 *   has its own TRAM-sized image in the model's own layout, so levels
 *   sharing a block under different selects never overlap, and levels
 *   under one select overlap exactly as before. Approximated, the model's
 *   choice: which TRAM or lane each select names on the board is
 *   unknown, and so is how far the textures of different selects share
 *   capacity, so the model gives each select a whole TRAM's room. A
 *   one- or two-component texel takes one TRAM word, its components in
 *   the word's leading bytes: the client saves and restores such textures
 *   one or two bytes a word over 64 rows of 64 words (traced), so a level
 *   must lie one texel a word for its save to read it back.
 *   Save and restore (mgras_tram_transfer_select()): a save of one to
 *   three bytes a word reads the image of the select TEXMODE1 names and
 *   such a restore writes the image of the select TL_MODE names, as the
 *   client sends them (established from its listing); a transfer of
 *   whole words, the kernel's and the client's RGB or RGBA save and
 *   restore, moves select 0's image only. Whole-word writes into select
 *   0's image (those restores and RGB or RGBA loads) mark the same blocks
 *   unwritten in the other selects' images, as the board would have
 *   overwritten every TRAM there. Limitation, approximated: a kernel save
 *   and restore around a context switch therefore drops the other
 *   selects' levels at those blocks rather than carrying them, and a draw
 *   naming them finds no level and is logged; it never samples texels the
 *   board would no longer hold.
 *   MXI and EMXI have a TE1 and TRAM on each of two REs. The model
 *   keeps one TRAM for both, approximated: each path holding the full
 *   texture is inferred, so a load, a base write and a draw reach the
 *   rows of both REs, and the capacity stays one TRAM's 4 MiB, not 8.
 *   The software cannot tell the difference; only the diagnostics'
 *   per-RE TRAM tests could. One kernel save and one restore of a block
 *   serve both paths, as on the board.
 *   A context opener discards that context's texture registers and
 *   tables, approximated: whether the board's TE1 state survives a
 *   context switch or an opener is unknown. TRAM survives both.
 *
 * - Depth. Approximated, the model's choice: an 8-bit host component
 *   kept at 4 bits is truncated (v >> 4) and widened back to 8 bits by
 *   replication (t * 17). The traced RGBA4 texels come out (1, 4, 8, 12)
 *   of 15 under it, and alpha 11 under round(v * 15 / 255), so the
 *   choice is visible; the board's conversion is unknown, and a physical
 *   render of texels differing in their low four bits would settle it.
 *   An 8-bit depth keeps the host byte. A 12-bit component keeps the top
 *   12 bits of its host halfword (approximated; the board's reduction is
 *   unknown, and a render of texels differing in the dropped bits would
 *   settle it) and is drawn as the nearest of 255, the sampler's texels
 *   being 8-bit. A two-byte host component kept at 8 bits is
 *   round(c * 255 / 65535) and at 4 bits that value's top four bits,
 *   approximated for the same reason; OpenGL 1.2.1 3.6.4 defines the
 *   component as c / 65535. One-byte components into a 12-bit level are
 *   refused, logged: no such load was traced.
 *
 * - Bind and draw. 0x0b00 with TEXMODE1 bit 0 set turns texturing on,
 *   0x0c00 or bit 0 clear off. A draw reads level 0 out of TRAM under the
 *   context's base and TXMIPMAP entry 0, as TXSIZE's base size (its two
 *   copies alike) in the layout and depth TEXMODE1 bits 4:3 and TEXMODE2
 *   bits 3:2 name, when a load or a restore has written that block since
 *   reset, the environment (TEXMODE1 bits 2:1, see mgras_tex_environment) is
 *   modulate, decal or blend, and nothing in TEXMODE2 asks for detail, a
 *   volume, a one-dimensional texture or the multi-buffer read: sampling
 *   by bits 5 to 8 and 19 (mgras_tex_pyramid), with the axes' wrap
 *   (mgras_tex_wrap), through the shared rasterizer's texture sampling.
 *   Anything else draws untextured, logged.
 *
 * - Class. A one-component level is stored once; TEXMODE1 11:9 of the
 *   draw says whether it is alpha (1), luminance (2) or intensity (3),
 *   and 4 is RGB or RGBA by layout. Alpha comes with environment code 3,
 *   which the model reads as the OpenGL alpha function (modulate) for
 *   every code. Established as words; the texel to fragment arithmetic
 *   is the shared rasterizer's, OpenGL 1.2.1, approximated.
 *
 * Not implemented, and logged with the draw untextured: the multi-buffer
 * read (TEXMODE2 bit 4 and TL_MODE bit 8), the border texels of a
 * bordered image (see "Load"), detail,
 * the texture colour table, one-dimensional and volume textures,
 * SGIS_texture_select's dual and quad groups (their draws name a
 * select their RGBA load did not use, so they usually find no level and
 * are logged, but may sample another one- or two-component texture kept
 * at the same block under that select) and two-component GL_ABGR_EXT
 * loads.
 *
 * The discriminating tests are gl-texture-load-draws-the-level,
 * gl-texture-size-from-tl-spec, gl-texture-tl-mode-persists,
 * gl-texture-rgba4-truncates, gl-texture-unsupported-draws-untextured,
 * gl-texture-textureless-board-ignores, gl-texture-migration, the replay
 * fixture gl-texture-rgba4-quad, gl-texture-many-levels-in-one-context,
 * for the ABGR, short and bordered loads gl-texture-load-abgr-bytes,
 * gl-texture-load-unsigned-shorts, gl-texture-load-abgr-shorts,
 * gl-texture-shorts-reduce-rounded, gl-texture-load-rgb-shorts,
 * gl-texture-bordered-image-stores-interior and
 * gl-texture-bordered-strips, and for the formats, environments, wraps
 * and filters gl-texture-class-intensity-vs-luminance,
 * gl-texture-class-alpha, gl-texture-env-blend-constant,
 * gl-texture-replace-is-modulate, gl-texture-env-decal-rgba,
 * gl-texture-clamp-border-nearest, gl-texture-wrap-mixed-axes,
 * gl-texture-border-per-class, gl-texture-linear-midpoint,
 * gl-texture-linear-edge-wrap, gl-texture-depth12-halfword,
 * gl-texture-mip-level-select and gl-texture-trilinear-blend.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/misc/sgi-mgras-texture.h"
#include "hw/misc/sgi-mgras-gl.h"
#include "hw/misc/sgi-mgras-raster.h"
#include "trace.h"

/* Class-0 texture tokens. */
#define MGRAS_TEX_ON             0x00000b00
#define MGRAS_TEX_OFF            0x00000c00
#define MGRAS_TEX_T_OFFSET       0x00005f04
#define MGRAS_TEX_LOAD_OPEN      0x00009b00
#define MGRAS_TEX_LOAD_CLOSE     0x00009c00

/*
 * The client's wide-level branch (see sgi_mgras_texture_load_start()):
 * none, a bracketed load of a level 2^9 texel units wide, then the bare
 * 0x9c00 that precedes its unbracketed transfers.
 */
#define MGRAS_TEX_WIDE_NONE      0
#define MGRAS_TEX_WIDE_LOADED    1
#define MGRAS_TEX_WIDE_CLOSED    2
#define MGRAS_TEX_WIDE_LOG2W     9
#define MGRAS_TEX_BASE           0x0000eb04
#define MGRAS_TEX_RECORDS        0x00007c08
#define MGRAS_TEX_TABLE          0x0000ec10

/*
 * TRAM. The IRIX 6.5.30 IP27 kernel manages each TRAM path as 256 blocks
 * of 16 KiB and gives each GL context a run of them, set on the board as
 * the 0xeb04 base (the kernel driver's texture-space arithmetic in
 * mgras.a, established). The client places each level at a block of its
 * own run, sized by the kept format: in traced runs an RGBA4 level took
 * two bytes a texel (perfly's 256 by 128 one four blocks and its 256 by
 * 256 one eight) and RGB8 and RGBA8 levels four (ivview's 512 by 512 RGBA
 * level 0 64 blocks, its 128 by 128 RGB level 0 four), with a block
 * between a large level and the next for its border. The kernel's own
 * save reads a block back as 64 rows of 64 locations at four bytes a
 * location (0xe50c's row count 0x40 and a 16 KiB DMA, format word
 * 0x400080), and the client's read of an RGB texture takes three bytes a
 * location (format 0x400070, 12 KiB for 64 rows): established as the
 * words of a traced IRIX guest run.
 *
 * The model keeps the 4 MiB as those 32-bit words, 64 to a row and 64
 * rows to a block. How the board lays texels into them is unknown, so the
 * layout below is the model's own, approximated; what it guarantees is
 * that the model's own saves and restores invert each other and that the
 * texture a client loaded draws the same after them.
 *
 * - A texel of the kept format takes a slot of the next power of two at
 *   or above its bits (components times 4, 8 or 12): eight 4-bit slots
 *   to a word for one 4-bit component, two 16-bit ones for RGBA4 (the
 *   traced two bytes), one word for RGB8 and RGBA8 (the traced four), two
 *   words for 36- and 48-bit texels. Components are packed from the
 *   slot's high end in order, so an RGB8 texel is the first three bytes
 *   of its word, the bytes a three-byte read of the location returns.
 * - A level wider than half a block starts at its first block's first
 *   word and runs on through the next ones, its texels in rows from t =
 *   0. A smaller one, which may share its block with the smaller levels
 *   after it, sits so that it ends where twice its own size from the
 *   block's end would put it: a level of n bits occupies [16 KiB - 2n,
 *   16 KiB - n) of the block. Each level of a pyramid holds at most half
 *   of the one before, so the levels sharing a block never overlap. Bit 8
 *   of the TXMIPMAP word is not used (its meaning is unknown; that it
 *   follows the level's parity is established only as what every traced
 *   pyramid sent).
 * - The block is the 8-bit base plus the word's 7:0; a level that would
 *   run past TRAM's 256 blocks is refused and logged.
 * - A draw reads from TRAM only where a load or a restore has written the
 *   level's first block since reset; elsewhere it is untextured, logged,
 *   as a draw with no level was before TRAM existed.
 *
 * A 36- or 48-bit texel's two words may be more than the client allots
 * (the report's 48-bit texels); no traced program kept one.
 *
 * TRAM save and restore, established from the kernel's texture I/O code
 * (a member of mgras.a, SHA-256 5cd14335...) and from the traced
 * words. When a new context's run overlaps another's, the kernel writes
 * base 0 and saves each of the other's blocks: as 0x7f08 register pairs
 * TXSIZE 0xd6d6 (a 64 by 8,192 shape), TXLOD 0, TXCLAMPSIZE 0x80000,
 * TL_MODE, TEXMODE1, TEXMODE2, TXADDR 0 and TXMIPMAP = the block, then
 * 0xd214, 0xe50c with the rows (0x40), a format word and 0x400d (the
 * header's 12 bytes are those three words), and 0xf200 (a sync request,
 * sgi-mgras-raster.c), it kicks a read of the rows through DMA-map slot
 * 2 with (2 << 1) | 0xb9, 0xbd, and ends with 0xe0100, 0xa0704 0,
 * 0xc0004 0xe00 (the HQ pixel-format word back to its rest value, which
 * the model ignores), 0xda00 and 0xd300. When the context runs again the
 * kernel, with no GL context loaded and base 0, restores them: pairs
 * TL_MODE, TL_WBUFFER, TEXRBUFFER, TEXMODE2, TEXMODE1, TL_ADDR 0,
 * TL_MIPMAP = the block and
 * TL_SPEC 0xd60 (the same shape), 0xd214, the copy rectangle 64 by 64,
 * XFRMODE, XFRSIZE and XFRCOUNTERS, then 0xe300, 0xd80c with the
 * transfer's word count in three 9-bit parts (0, 8, 0), the DMA from slot
 * 2 kicked (2 << 1) | 0xa1, 0xa5, the 0xe0400 fence, XFRMASKLOW and
 * 0xd300. Neither reads the board's RE count or writes RE select 0x112,
 * so on MXI and EMXI one save and one restore serve both TRAM paths.
 *
 * The client's own texture manager, when a context's textures outgrow its
 * run, reads a texture back through the kernel (the same 0xe50c, kicked
 * 0xb9 on slot 0) with TXADDR 0 and TXMIPMAP = its first block as 0x7c08
 * triples and 0xec10, in passes of at most 0x2000 rows, and later puts it
 * back as a texture load: TL_MODE, TL_ADDR 0 and TL_MIPMAP = its first
 * block, TL_SPEC 0xd60, the load's rectangle at GE parameters 0xe34 to
 * 0xe38 (x 0, the first row, 64 wide and the rows), then 0x9d10 with the
 * word count in three 9-bit parts and the load routine 0x511a, the HQ
 * page-list words and a kick 0xa3 on slot 1, 32,640 and 28,800 bytes a
 * transfer in that run (170 and 150 rows of 64 three-byte locations).
 *
 * The model moves those bytes. A save copies the rows from the block the
 * last TXMIPMAP entry 0 named, under the base then current, taking from
 * each word as many of its first bytes as the DMA has room for a
 * location (four on the kernel's read, three on the client's read of an
 * RGB texture); a restore writes them back to the block the last
 * TL_MIPMAP entry 0 named. The kernel's register pairs set the transfer's
 * table indices and rows outside any GL context: they come with no GL
 * context loaded on a restore and only from the kernel, so the model
 * keeps them apart from the client's texture state, inferred from that
 * order in the trace. A 0x9d10 for routine 0x511a under TL_SPEC 0xd60 is
 * the client's restore; any other 0x9d10 opens a transfer to the GE the
 * model does not take, whose kick moves nothing and is logged. The
 * OpenGL client's fast pixel path also sends 0x9d10 before its kick
 * (from the client's code); it was not traced, and the load routine and the
 * 64-by-8,192 shape are what the model keys on to keep it out. That a
 * transfer neither reads nor writes the colour planes is inferred: each
 * names texture memory and no raster box.
 *
 * The open save or restore is bounded: a transfer ends, logged, at a kick
 * of the other direction or a read whose bytes are not whole words of its
 * rows, and a second open replaces the first, logged; a save whose 0xda00
 * never comes is forgotten at the next kick. That is the model's choice,
 * made so a stray kick cannot be taken as a transfer's own; whether the
 * board ends a transfer so is unknown, and a trace of a kernel read
 * between a save's set-up and its kick would settle it.
 *
 * The discriminating tests are tram-load-is-one-store,
 * tram-save-returns-the-block, tram-client-restore-round-trip,
 * tram-ge-dma-is-not-a-raster-image, tram-open-save-ends-at-other-kick
 * and mxi-tram-save-restore, on MXI, EMXI and SI+T.
 */
#define MGRAS_TEX_TRAM_READ      0x0000e50c
#define MGRAS_TEX_TRAM_RESTORE   0x0000e300
#define MGRAS_TEX_TRAM_GE_COUNT  0x0000d80c
#define MGRAS_TEX_GE_DMA         0x00009d10
/* 0x9d10's fourth word: the GE routine its transfer feeds. */
#define MGRAS_TEX_GE_DMA_ROUTINE 3
/* TL_SPEC's 64-by-8,192 shape: level 0, 64 wide, 8,192 tall. */
#define MGRAS_TEX_SPEC_WHOLE     0xd60
#define MGRAS_TRAM_ROW_WORDS     64
#define MGRAS_TRAM_BLOCK_ROWS    64
#define MGRAS_TRAM_WORD_BYTES    4
#define MGRAS_TRAM_BLOCK_BITS    ((uint64_t)SGI_MGRAS_TRAM_BLOCK_BYTES * 8)
#define MGRAS_TRAM_ROWS          (SGI_MGRAS_TRAM_BLOCKS * MGRAS_TRAM_BLOCK_ROWS)

/* Where a TRAM transfer stands between the word that opens it and its kick. */
enum {
    MGRAS_TRAM_IDLE,
    MGRAS_TRAM_SAVE_OPEN,       /* 0xe50c came, its read kick is next */
    MGRAS_TRAM_SAVE_READ,       /* the read was kicked; 0xda00 closes it */
    MGRAS_TRAM_RESTORE_OPEN,    /* 0xd80c came, its write kick is next */
    MGRAS_TRAM_CLIENT_OPEN,     /* the client's restore, kicked next */
    MGRAS_TRAM_GE_DMA_OPEN,     /* another 0x9d10, not modelled */
    MGRAS_TRAM_STATES,
};

/* Texture registers. */
#define MGRAS_TEX_TEXMODE1       0x111
#define MGRAS_TEX_TEXMODE2       0x180
#define MGRAS_TEX_TXSIZE         0x181
#define MGRAS_TEX_TXLOD          0x183
#define MGRAS_TEX_MDDMA_CNTRL    0x18a
#define MGRAS_TEX_TXENV_RG       0x142
#define MGRAS_TEX_TXENV_B        0x143
#define MGRAS_TEX_TXBCOLOR_RG    0x184
#define MGRAS_TEX_TXBCOLOR_BA    0x185
#define MGRAS_TEX_TEXRBUFFER     0x18b
#define MGRAS_TEX_TXCLAMPSIZE    0x18c
#define MGRAS_TEX_TXADDR         0x190
#define MGRAS_TEX_TXMIPMAP       0x191
#define MGRAS_TEX_TXBORDER       0x193
#define MGRAS_TEX_TL_WBUFFER     0x1a0
#define MGRAS_TEX_TL_MODE        0x1a1
#define MGRAS_TEX_TL_SPEC        0x1a2
#define MGRAS_TEX_TL_ADDR        0x1a3
#define MGRAS_TEX_TL_MIPMAP      0x1a4
#define MGRAS_TEX_TL_S_SIZE      0x1a9
#define MGRAS_TEX_TL_T_SIZE      0x1aa
#define MGRAS_TEX_TL_S_LEFT      0x1ab
#define MGRAS_TEX_TL_T_BOTTOM    0x1ac
#define MGRAS_TEX_COLOR_TABLE_SELECTOR 0x15c
#define MGRAS_TEX_COLOR_TABLE_DATA     0x15d
#define MGRAS_TEX_COLOR_TABLE_SELECT  0x90000000u
#define MGRAS_TEX_COLOR_TABLE_MODE    0x0000e000u

/*
 * Raster register records the pixel read-back applies in
 * sgi_mgras_raster_record(): the box corners, transfer size and counters
 * and the byte packing.
 */
#define MGRAS_TEX_RSS_BLOCKXYSTARTI     0x046
#define MGRAS_TEX_RSS_BLOCKXYENDI       0x047
#define MGRAS_TEX_RSS_XFRSIZE           0x153
#define MGRAS_TEX_RSS_XFRCOUNTERS       0x158
#define MGRAS_TEX_RSS_BYTE_XFER_PACKING 0x159

/* TEXMODE1 fields: bits 2:1 environment, 4:3 layout, 11:9 class. */
#define MGRAS_TEXMODE1_CLASS(m)    (((m) >> 9) & 7)
#define MGRAS_TEXMODE1_LAYOUT(m)   (((m) >> 3) & 3)
#define MGRAS_TEXMODE1_ENV(m)      (((m) >> 1) & 3)
#define MGRAS_TEX_CLASS_ALPHA      1
#define MGRAS_TEX_CLASS_LUMINANCE  2
#define MGRAS_TEX_CLASS_INTENSITY  3
#define MGRAS_TEX_CLASS_COLOUR     4
#define MGRAS_TEX_ENV_MODULATE     0
#define MGRAS_TEX_ENV_DECAL        1
#define MGRAS_TEX_ENV_BLEND        2
#define MGRAS_TEX_ENV_ALPHA        3

#define MGRAS_TEXMODE1_ENABLE    0x00000001u
#define MGRAS_TEXMODE1_SELECT    0x00000180u
#define MGRAS_TEXMODE1_TABLE     0x0000f000u
/*
 * TEXMODE2 bits the model does not draw: 4 the multi-buffer read, 9
 * detail, 10 unknown, 23 a volume and 24 a one-dimensional texture. Bits
 * 5 and 19 together say a mipmap filter over more than one level, 6 and 8
 * the blend between and within levels, 7 the magnification filter and 11
 * to 14 the wrap bits, below. Only one of 5 and 19 is not drawn.
 */
#define MGRAS_TEXMODE2_UNDRAWN   0x01000610u
#define MGRAS_TEXMODE2_MIP       0x00080020u
#define MGRAS_TEXMODE2_MIP_BETWEEN 0x00000040u
#define MGRAS_TEXMODE2_MAG_LINEAR 0x00000080u
#define MGRAS_TEXMODE2_MIN_LINEAR 0x00000100u
#define MGRAS_TEXMODE2_WRAP_S    0x00000800u
#define MGRAS_TEXMODE2_WRAP_T    0x00001000u
#define MGRAS_TEXMODE2_WRAP_R    0x00002000u
#define MGRAS_TEXMODE2_CLAMP     0x00004000u
#define MGRAS_TEXMODE2_VOLUME    0x00800000u

/* TXLOD: bits 25:16 are 64 times the minimum LOD, 3:0 the last level. */
#define MGRAS_TXLOD_MIN(v)       (((v) >> 16) & 0x3ff)
#define MGRAS_TXLOD_LAST(v)      ((v) & 0xf)

/* The pixel transfer's routine word in 0x8d20's payload, and its value. */
#define MGRAS_TEX_ROUTINE_WORD   6
#define MGRAS_TEX_ROUTINE_LOAD   0x511a
#define MGRAS_TEX_FLAGS_WORD     7
#define MGRAS_TEX_FLAGS_BY_DMA   0x80

/*
 * The rectangle's x and y in the level at GE parameters 0xe34 and 0xe35,
 * its size word at 0xe34 + 4, and the HQ pixel-format word's GL_ABGR_EXT
 * bit.
 */
#define MGRAS_TEX_GE_X           0xe34
#define MGRAS_TEX_GE_Y           (0xe34 + 1)
#define MGRAS_TEX_GE_SIZE        (0xe34 + 4)
#define MGRAS_TEX_HQ_ABGR        0x80
/* The HQ word's bits 3:2 are the image's components less one; 3 is four. */
#define MGRAS_TEX_HQ_COMPS(hq)   (((hq) >> 2) & 3)
/*
 * The GE words the pixel program names its unpack block with: the block's
 * address is the program's fourth word. The block's second word is the
 * unpack routine, which gives the image's component count, and its sixth
 * a float scale, 1/255 for components of one byte and 1/65535 for two
 * bytes. Both were read from the client's loads, the routine words beside
 * images of matching sizes: established as words, the meanings inferred.
 */
#define MGRAS_TEX_GE_PROGRAM     0x13b
#define MGRAS_TEX_GE_UNPACK_ROUTINE 1
#define MGRAS_TEX_GE_UNPACK_SCALE 5
#define MGRAS_TEX_UNPACK_4       0x4064
#define MGRAS_TEX_UNPACK_3       0x40f2
#define MGRAS_TEX_UNPACK_2       0x417e
#define MGRAS_TEX_UNPACK_1       0x420c
#define MGRAS_TEX_SCALE_BYTE     0x3b808081u
#define MGRAS_TEX_SCALE_SHORT    0x37800080u

/*
 * The load's six GE words at 0xe3d: word 3 is the level's TXMIPMAP word,
 * its first block in 7:0. Word 4 is the level's TXBORDER word, the block
 * that holds its border: perfly's nine mipmapped levels loaded 0x10,
 * 0x13, 0x15, 0x16 and then 0x17 there and drew the same through the
 * TXBORDER table, an unbordered image whose transfers covered each level
 * exactly. A one-level texture loads its first block less one and draws
 * no TXBORDER. Word 4 does not say whether the image has a border, and
 * the model does not read it.
 */
#define MGRAS_TEX_GE_LOAD        0xe3d
#define MGRAS_TEX_GE_BLOCK       (MGRAS_TEX_GE_LOAD + 3)

/* TL_MODE and TL_SPEC fields, and the kept formats the model stores. */
#define MGRAS_TEX_LAYOUT(mode)   (((mode) >> 1) & 3)
#define MGRAS_TEX_DEPTH(mode)    (((mode) >> 5) & 3)
#define MGRAS_TEX_LAYOUT_ONE     0
#define MGRAS_TEX_LAYOUT_LA      1
#define MGRAS_TEX_LAYOUT_RGB     2
#define MGRAS_TEX_LAYOUT_RGBA    3
#define MGRAS_TEX_DEPTH_4        0
#define MGRAS_TEX_DEPTH_8        1
#define MGRAS_TEX_DEPTH_12       2
#define MGRAS_TEX_SPEC_LEVEL(s)  ((s) & 0xf)
#define MGRAS_TEX_SPEC_LOG2W(s)  (((s) >> 4) & 0xf)
#define MGRAS_TEX_SPEC_LOG2H(s)  (((s) >> 8) & 0xf)
/*
 * Valid border caches fit in half their disjoint TRAM image bits plus six
 * texels each: w + h <= w * h + 1, and each image texel uses at least four
 * bits. Bound the migration array by all TRAM bits and cache slots.
 */
#define MGRAS_TEX_BORDER_WORDS_MAX \
    ((uint64_t)SGI_MGRAS_TRAM_IMAGE_BYTES * 4 + \
     (uint64_t)SGI_MGRAS_TEX_BORDER_CACHES * 6)

/* Parser payload slots a 0x7c08 block keeps its place in. */
#define MGRAS_TEX_REC_COUNT      0
#define MGRAS_TEX_REC_WORDS      1
#define MGRAS_TEX_REC_NEXT       2
#define MGRAS_TEX_REC_REG        3
#define MGRAS_TEX_REC_VALUE      4

static bool mgras_tex_present(SGIMgrasState *s)
{
    return s->board && s->board->tram_count;
}

static SGIMgrasTexture *mgras_tex(SGIMgrasState *s)
{
    return &s->gl_context[s->gl_current].tex;
}

/* Components a texel of a kept layout: L, LA, RGB, RGBA. */
static uint32_t mgras_tex_components(uint32_t mode)
{
    return MGRAS_TEX_LAYOUT(mode) + 1;
}

/* Host bytes a texel of the image the pending load carries. */
static uint32_t mgras_tex_load_texel(const SGIMgrasTexture *t)
{
    return t->load_comps * t->load_csize;
}

/*
 * The image the GE's pixel program unpacks, from its unpack block: its
 * components a texel and bytes a component. False for a program with no
 * block or a routine or scale word not seen in a traced load.
 */
static bool mgras_tex_unpack(SGIMgrasState *s, uint32_t *comps,
                             uint32_t *csize)
{
    uint32_t params = s->ge_param[MGRAS_TEX_GE_PROGRAM + 3];
    uint32_t routine, scale;

    if (params >= SGI_MGRAS_GE_PARAM_WORDS - MGRAS_TEX_GE_UNPACK_SCALE) {
        return false;
    }
    routine = s->ge_param[params + MGRAS_TEX_GE_UNPACK_ROUTINE];
    scale = s->ge_param[params + MGRAS_TEX_GE_UNPACK_SCALE];
    switch (routine) {
    case MGRAS_TEX_UNPACK_4:
        *comps = 4;
        break;
    case MGRAS_TEX_UNPACK_3:
        *comps = 3;
        break;
    case MGRAS_TEX_UNPACK_2:
        *comps = 2;
        break;
    case MGRAS_TEX_UNPACK_1:
        *comps = 1;
        break;
    default:
        return false;
    }
    switch (scale) {
    case MGRAS_TEX_SCALE_BYTE:
        *csize = 1;
        break;
    case MGRAS_TEX_SCALE_SHORT:
        *csize = 2;
        break;
    default:
        return false;
    }
    return true;
}

/*
 * Whether an image of comps components of csize bytes lands in a level
 * kept as mode: the same components, or RGB into RGBA with alpha 1. A
 * 12-bit level is fed from two-byte components only.
 */
static bool mgras_tex_image_fits(uint32_t mode, uint32_t comps,
                                 uint32_t csize)
{
    uint32_t kept = mgras_tex_components(mode);

    if (comps != kept && !(comps == 3 && kept == 4)) {
        return false;
    }
    return csize == 2 || MGRAS_TEX_DEPTH(mode) != MGRAS_TEX_DEPTH_12;
}

/*
 * Whether a rectangle of rw by rh at x, y, the guest's signed words, lies
 * inside a w by h level and the one-texel border around it. A bordered
 * image is sent at -1, -1 with two texels more in each direction (distort
 * and its 512-by-512 level's strips at -1, 64 to 448 and 512).
 */
static bool mgras_tex_rect_fits(uint32_t x, uint32_t y, uint32_t rw,
                                uint32_t rh, uint32_t w, uint32_t h)
{
    int64_t sx = (int32_t)x, sy = (int32_t)y;

    return rw && rh && sx >= -1 && sy >= -1 && sx + rw <= (int64_t)w + 1 &&
           sy + rh <= (int64_t)h + 1;
}

/* Bits a component of a kept format: 4, 8 or 12. */
static uint32_t mgras_tex_depth_bits(uint32_t mode)
{
    return 4 * (MGRAS_TEX_DEPTH(mode) + 1);
}

/*
 * A texel's slot in TRAM: the next power of two at or above its bits
 * (see "TRAM" above).
 */
static uint32_t mgras_tex_slot_bits(uint32_t mode)
{
    if (MGRAS_TEX_LAYOUT(mode) <= MGRAS_TEX_LAYOUT_LA) {
        /* One TRAM word a texel (see "Storage", TRAM select). */
        return MGRAS_TRAM_WORD_BYTES * 8;
    }
    return pow2ceil(mgras_tex_components(mode) * mgras_tex_depth_bits(mode));
}

static bool mgras_tram_written(SGIMgrasState *s, uint32_t block)
{
    return s->tram_written[block / 32] & (1u << (block % 32));
}

/* Mark the blocks the bit range [bit, bit + bits) touches as written. */
static void mgras_tram_mark(SGIMgrasState *s, uint64_t bit, uint64_t bits)
{
    uint64_t last = (bit + bits - 1) / MGRAS_TRAM_BLOCK_BITS;

    for (uint64_t b = bit / MGRAS_TRAM_BLOCK_BITS; b <= last; b++) {
        s->tram_written[b / 32] |= 1u << (b % 32);
    }
}

/*
 * The TRAM select of a level of the kept format mode, in TL_MODE's
 * fields: bits 4:3 for a one- or two-component layout, 0 for RGB and
 * RGBA, whose select bits the client leaves undefined (see "Storage").
 */
static uint32_t mgras_tex_select(uint32_t mode)
{
    return MGRAS_TEX_LAYOUT(mode) <= MGRAS_TEX_LAYOUT_LA ?
           (mode >> 3) & 3 : 0;
}

/* The first bit of a select's image in the TRAM image. */
static uint64_t mgras_tram_image_bit(uint32_t select)
{
    return (uint64_t)select * SGI_MGRAS_TRAM_BYTES * 8;
}

/*
 * A write of whole TRAM words into select 0's image, bits [bit, bit +
 * bits): a three- or four-component level, or a save-shaped restore of
 * four bytes a word. On the board it rewrites every TRAM of those words,
 * so whatever the other selects kept at those blocks is gone; the model
 * marks their blocks unwritten, and a draw that names them finds no
 * level and is logged, instead of sampling texels the board would no
 * longer hold.
 */
static void mgras_tram_unmark_selects(SGIMgrasState *s, uint64_t bit,
                                      uint64_t bits)
{
    uint64_t last = (bit + bits - 1) / MGRAS_TRAM_BLOCK_BITS;

    for (uint32_t select = 1; select < SGI_MGRAS_TRAM_SELECTS; select++) {
        uint64_t image = mgras_tram_image_bit(select) / MGRAS_TRAM_BLOCK_BITS;

        for (uint64_t b = bit / MGRAS_TRAM_BLOCK_BITS; b <= last; b++) {
            s->tram_written[(image + b) / 32] &= ~(1u << ((image + b) % 32));
        }
    }
}

/*
 * Where a level of the kept format mode, 1 << log2w by 1 << log2h, starts
 * in the TRAM image, in bits, under base and TXMIPMAP word, in the image
 * of its TRAM select: false, with nothing set, when it would run past
 * that image's end.
 */
static bool mgras_tram_place(uint32_t base, uint32_t word, uint32_t log2w,
                             uint32_t log2h, uint32_t mode, uint64_t *bit)
{
    uint64_t bits = ((uint64_t)mgras_tex_slot_bits(mode) << log2w) << log2h;
    uint64_t block = (uint64_t)(base & 0xff) + (word & 0xff);
    uint64_t start = block * MGRAS_TRAM_BLOCK_BITS;
    uint64_t image = mgras_tram_image_bit(mgras_tex_select(mode));

    if (bits <= MGRAS_TRAM_BLOCK_BITS / 2) {
        start += MGRAS_TRAM_BLOCK_BITS - 2 * bits;
    }
    if (start + bits > (uint64_t)SGI_MGRAS_TRAM_BYTES * 8) {
        return false;
    }
    *bit = image + start;
    return true;
}

/*
 * A component of depth 4, 8 or 12 bits at a bit offset in TRAM. Every
 * slot, level start and component offset is a multiple of four bits, so
 * a component lies within the byte or halfword it starts in.
 */
static uint32_t mgras_tram_get(const uint8_t *tram, uint64_t bit,
                               uint32_t depth)
{
    const uint8_t *p = tram + bit / 8;
    uint32_t shift = bit % 8;

    switch (depth) {
    case 4:
        return (p[0] >> (4 - shift)) & 0xf;
    case 8:
        return p[0];
    default:
        return (lduw_be_p(p) >> (4 - shift)) & 0xfff;
    }
}

static void mgras_tram_put(uint8_t *tram, uint64_t bit, uint32_t depth,
                           uint32_t value)
{
    uint8_t *p = tram + bit / 8;
    uint32_t shift = bit % 8;
    uint32_t mask;

    switch (depth) {
    case 4:
        mask = 0xf << (4 - shift);
        p[0] = (p[0] & ~mask) | ((value << (4 - shift)) & mask);
        return;
    case 8:
        p[0] = value;
        return;
    default:
        mask = 0xfff << (4 - shift);
        stw_be_p(p, (lduw_be_p(p) & ~mask) | ((value << (4 - shift)) & mask));
        return;
    }
}

static void mgras_tex_staging_free(SGIMgrasTexture *t)
{
    g_free(t->staging);
    t->staging = NULL;
    t->staged = t->staging_bytes = 0;
}

static uint32_t mgras_tex_border_pixels(uint32_t w, uint32_t h)
{
    return 2 * (w + 2) + 2 * h;
}

static uint32_t mgras_tex_border_key(uint32_t base, uint32_t level)
{
    return (base & 0xff) * SGI_MGRAS_TEX_LEVELS + level;
}

static void mgras_tex_border_compact(SGIMgrasTexture *t)
{
    uint32_t words = 0;
    uint64_t *data = NULL;

    for (uint32_t i = 0; i < SGI_MGRAS_TEX_BORDER_CACHES; i++) {
        words += t->border_count[i];
    }
    if (words) {
        data = g_new(uint64_t, words);
        words = 0;
        for (uint32_t i = 0; i < SGI_MGRAS_TEX_BORDER_CACHES; i++) {
            if (!t->border_count[i]) {
                t->border_offset[i] = 0;
                continue;
            }
            memcpy(data + words, t->border_data + t->border_offset[i],
                   t->border_count[i] * sizeof(*data));
            t->border_offset[i] = words;
            words += t->border_count[i];
        }
    } else {
        for (uint32_t i = 0; i < SGI_MGRAS_TEX_BORDER_CACHES; i++) {
            t->border_offset[i] = 0;
        }
    }
    g_free(t->border_data);
    t->border_data = data;
    t->border_words = words;
}

static void mgras_tex_border_clear(SGIMgrasTexture *t, uint32_t key)
{
    t->border_base[key] = 0;
    t->border_mode[key] = 0;
    t->border_width[key] = 0;
    t->border_height[key] = 0;
    t->border_offset[key] = 0;
    t->border_count[key] = 0;
    t->border_valid_count[key] = 0;
    t->border_start[key] = 0;
    t->border_bits[key] = 0;
}

static bool mgras_tex_border_overlap(uint64_t a, uint64_t asize,
                                     uint64_t b, uint64_t bsize)
{
    return a < b + bsize && b < a + asize;
}

static void mgras_tex_border_invalidate_range(SGIMgrasTexture *t,
                                               uint64_t start,
                                               uint64_t bits)
{
    bool compact = false;

    for (uint32_t i = 0; i < SGI_MGRAS_TEX_BORDER_CACHES; i++) {
        if (t->border_count[i] &&
            mgras_tex_border_overlap(start, bits, t->border_start[i],
                                     t->border_bits[i])) {
            mgras_tex_border_clear(t, i);
            compact = true;
        }
    }
    if (compact) {
        mgras_tex_border_compact(t);
    }
}

static void mgras_tex_border_invalidate_tram(SGIMgrasState *s,
                                               SGIMgrasTexture *keep,
                                               uint64_t start,
                                               uint64_t bits)
{
    for (uint32_t i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        SGIMgrasTexture *t = &s->gl_context[i].tex;

        if (t != keep) {
            mgras_tex_border_invalidate_range(t, start, bits);
        }
    }
}

static void mgras_tex_border_reset(SGIMgrasTexture *t)
{
    g_free(t->border_data);
    t->border_data = NULL;
    t->border_words = 0;
    memset(t->border_base, 0, sizeof(t->border_base));
    memset(t->border_mode, 0, sizeof(t->border_mode));
    memset(t->border_width, 0, sizeof(t->border_width));
    memset(t->border_height, 0, sizeof(t->border_height));
    memset(t->border_offset, 0, sizeof(t->border_offset));
    memset(t->border_count, 0, sizeof(t->border_count));
    memset(t->border_valid_count, 0, sizeof(t->border_valid_count));
    memset(t->border_start, 0, sizeof(t->border_start));
    memset(t->border_bits, 0, sizeof(t->border_bits));
}

static void mgras_tex_border_prepare(SGIMgrasTexture *t, uint32_t key,
                                     uint32_t base, uint32_t mode,
                                     uint32_t w, uint32_t h, uint64_t start,
                                     uint64_t bits, bool touches_border)
{
    uint32_t count = mgras_tex_border_pixels(w, h);
    bool compact = false;

    for (uint32_t i = 0; i < SGI_MGRAS_TEX_BORDER_CACHES; i++) {
        bool same;

        if (!t->border_count[i]) {
            continue;
        }
        same = i == key && t->border_base[i] == base &&
               t->border_mode[i] == mode && t->border_width[i] == w &&
               t->border_height[i] == h && t->border_start[i] == start &&
               t->border_bits[i] == bits;
        if ((i == key && !same) ||
            (i != key && mgras_tex_border_overlap(start, bits,
                                                   t->border_start[i],
                                                   t->border_bits[i]))) {
            mgras_tex_border_clear(t, i);
            compact = true;
        }
    }
    if (compact) {
        mgras_tex_border_compact(t);
    }
    if (!touches_border || t->border_count[key]) {
        return;
    }
    t->border_offset[key] = t->border_words;
    t->border_count[key] = count;
    t->border_valid_count[key] = 0;
    t->border_base[key] = base;
    t->border_mode[key] = mode;
    t->border_width[key] = w;
    t->border_height[key] = h;
    t->border_start[key] = start;
    t->border_bits[key] = bits;
    t->border_data = g_realloc(t->border_data,
                               (t->border_words + count) *
                               sizeof(*t->border_data));
    memset(t->border_data + t->border_words, 0,
           count * sizeof(*t->border_data));
    t->border_words += count;
}

static bool mgras_tex_border_index(uint32_t w, uint32_t h, int32_t x,
                                   int32_t y, uint32_t *index)
{
    uint32_t row = w + 2;

    if (x < -1 || y < -1 || x > (int32_t)w || y > (int32_t)h ||
        (x >= 0 && x < (int32_t)w && y >= 0 && y < (int32_t)h)) {
        return false;
    }
    if (y == -1) {
        *index = x + 1;
    } else if (y == (int32_t)h) {
        *index = row + h + x + 1;
    } else if (x == (int32_t)w) {
        *index = row + y;
    } else {
        *index = 2 * row + h + y;
    }
    return true;
}

static void mgras_tex_border_store(SGIMgrasTexture *t, uint32_t key,
                                   uint32_t index,
                                   const uint16_t component[4])
{
    uint64_t value = 1ull << 48;
    uint64_t *slot = &t->border_data[t->border_offset[key] + index];

    if (!(*slot & (1ull << 48))) {
        t->border_valid_count[key]++;
    }
    for (uint32_t i = 0; i < 4; i++) {
        value |= (uint64_t)(component[i] & 0xfff) << (12 * i);
    }
    *slot = value;
}

void sgi_mgras_texture_reset(SGIMgrasTexture *t)
{
    mgras_tex_staging_free(t);
    mgras_tex_border_reset(t);
    memset(t, 0, sizeof(*t));
}

/*
 * The shared draw state's GL_TEXTURE_2D follows 0x0b00 and 0x0c00 with
 * TEXMODE1 bit 0: the client sends 0x0b00 then bit 0 clear while its
 * texture is incomplete, and bit 0 set for the draw.
 */
static void mgras_tex_update_enable(SGIMgrasState *s)
{
    SGIMgrasTexture *t = mgras_tex(s);

    s->gl_context[s->gl_current].gl->cur.texture_2d =
        t->on && (t->texmode1 & MGRAS_TEXMODE1_ENABLE);
}

bool sgi_mgras_texture_header(uint32_t header)
{
    switch (header) {
    case MGRAS_TEX_ON:
    case MGRAS_TEX_OFF:
    case MGRAS_TEX_T_OFFSET:
    case MGRAS_TEX_LOAD_OPEN:
    case MGRAS_TEX_LOAD_CLOSE:
    case MGRAS_TEX_BASE:
    case MGRAS_TEX_RECORDS:
    case MGRAS_TEX_TRAM_READ:
    case MGRAS_TEX_TRAM_RESTORE:
    case MGRAS_TEX_TRAM_GE_COUNT:
    case MGRAS_TEX_GE_DMA:
        return true;
    default:
        return false;
    }
}

static void mgras_tex_register(SGIMgrasState *s, uint32_t reg, uint32_t value)
{
    SGIMgrasTexture *t = mgras_tex(s);

    switch (reg) {
    case MGRAS_TEX_TEXMODE1:
        t->texmode1 = value;
        mgras_tex_update_enable(s);
        return;
    case MGRAS_TEX_TEXMODE2:
        t->texmode2 = value;
        return;
    case MGRAS_TEX_TXSIZE:
        t->txsize = value;
        return;
    case MGRAS_TEX_MDDMA_CNTRL:
        t->mddma_cntrl = value;
        return;
    case MGRAS_TEX_TXLOD:
        t->txlod = value;
        return;
    case MGRAS_TEX_TXENV_RG:
    case MGRAS_TEX_TXENV_B:
        t->txenv[reg - MGRAS_TEX_TXENV_RG] = value;
        return;
    case MGRAS_TEX_TXBCOLOR_RG:
    case MGRAS_TEX_TXBCOLOR_BA:
        t->txbcolor[reg - MGRAS_TEX_TXBCOLOR_RG] = value;
        return;
    case MGRAS_TEX_TEXRBUFFER:
        t->texrbuffer = value;
        return;
    case MGRAS_TEX_TXCLAMPSIZE:
        t->txclampsize = value;
        return;
    case MGRAS_TEX_TXADDR:
        t->txaddr = value % SGI_MGRAS_TEX_TABLE;
        return;
    case MGRAS_TEX_TL_WBUFFER:
        t->tl_wbuffer = value;
        return;
    case MGRAS_TEX_TL_MODE:
        t->tl_mode = value;
        return;
    case MGRAS_TEX_TL_SPEC:
        t->tl_spec = value;
        return;
    case MGRAS_TEX_TL_ADDR:
        t->tl_addr = value % SGI_MGRAS_TEX_TABLE;
        return;
    case MGRAS_TEX_COLOR_TABLE_SELECTOR:
        t->color_table_selector = value;
        t->color_table_written = 0;
        t->color_table_valid = 0;
        t->color_table_active = value == MGRAS_TEX_COLOR_TABLE_SELECT;
        if (!t->color_table_active) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture colour-table "
                          "selector 0x%x is not modelled\n", value);
        }
        return;
    case MGRAS_TEX_COLOR_TABLE_DATA:
        if (!t->color_table_active) {
            return;
        }
        if (value > 0xff ||
            t->color_table_written >= SGI_MGRAS_TEXTURE_COLOR_TABLE_BYTES) {
            t->color_table_active = 0;
            t->color_table_valid = 0;
            t->color_table_written = 0;
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: incomplete texture "
                          "colour-table upload\n");
            return;
        }
        t->color_table_data[t->color_table_written++] = value;
        t->color_table_valid =
            t->color_table_written == SGI_MGRAS_TEXTURE_COLOR_TABLE_BYTES;
        return;
    case MGRAS_TEX_RSS_BLOCKXYSTARTI:
    case MGRAS_TEX_RSS_BLOCKXYENDI:
    case MGRAS_TEX_RSS_XFRSIZE:
    case MGRAS_TEX_RSS_XFRCOUNTERS:
    case MGRAS_TEX_RSS_BYTE_XFER_PACKING:
        /* The raster engine applies these from the same block. */
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: raster register record 0x%x "
                      "value 0x%x is not modelled\n", reg, value);
        return;
    }
}

/* The absolute block a TXMIPMAP or TL_MIPMAP word names under base. */
static uint32_t mgras_tram_block(uint32_t base, uint32_t word)
{
    return (base & 0xff) + (word & 0xff);
}

/*
 * The 0xec10 table writes (shadow index, shadow table, register, value)
 * into TXMIPMAP, TXBORDER and TL_MIPMAP; the GL path keeps the
 * detail-scale table's defaults, and TL_BORDER is logged. Entry 0 of
 * TXMIPMAP and TL_MIPMAP is also the block a TRAM save or restore moves
 * (see "TRAM" above).
 */
static bool mgras_tex_table(SGIMgrasState *s, const uint32_t *payload)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t reg = payload[2];

    switch (reg) {
    case MGRAS_TEX_TXMIPMAP:
        t->txmipmap[t->txaddr] = payload[3];
        if (!t->txaddr) {
            s->tram_read_block = mgras_tram_block(t->base, payload[3]);
        }
        break;
    case MGRAS_TEX_TXBORDER:
        t->txborder[t->txaddr] = payload[3];
        break;
    case MGRAS_TEX_TL_MIPMAP:
        t->tl_mipmap[t->tl_addr] = payload[3];
        if (!t->tl_addr) {
            s->tram_load_block = mgras_tram_block(t->base, payload[3]);
        }
        t->tl_addr = (t->tl_addr + 1) % SGI_MGRAS_TEX_TABLE;
        return true;
    default:
        return false;
    }
    t->txaddr = (t->txaddr + 1) % SGI_MGRAS_TEX_TABLE;
    return true;
}

bool sgi_mgras_texture_kernel_record(SGIMgrasState *s, uint32_t reg,
                                     uint32_t value)
{
    SGIMgrasTexture *t = mgras_tex(s);

    if (!mgras_tex_present(s)) {
        return false;
    }
    switch (reg) {
    case MGRAS_TEX_TXADDR:
        s->tram_kernel_txaddr = value % SGI_MGRAS_TEX_TABLE;
        return true;
    case MGRAS_TEX_TXMIPMAP:
        if (!s->tram_kernel_txaddr) {
            s->tram_read_block = mgras_tram_block(t->base, value);
        }
        return true;
    case MGRAS_TEX_TL_ADDR:
        s->tram_kernel_tladdr = value % SGI_MGRAS_TEX_TABLE;
        return true;
    case MGRAS_TEX_TL_MIPMAP:
        if (!s->tram_kernel_tladdr) {
            s->tram_load_block = mgras_tram_block(t->base, value);
        }
        return true;
    case MGRAS_TEX_TL_T_SIZE:
        s->tram_kernel_rows = value;
        return true;
    case MGRAS_TEX_TEXMODE1:
    case MGRAS_TEX_TEXMODE2:
    case MGRAS_TEX_TXSIZE:
    case MGRAS_TEX_TXLOD:
    case MGRAS_TEX_TEXRBUFFER:
    case MGRAS_TEX_TXCLAMPSIZE:
    case MGRAS_TEX_TL_WBUFFER:
    case MGRAS_TEX_TL_MODE:
    case MGRAS_TEX_TL_SPEC:
    case MGRAS_TEX_TL_S_SIZE:
    case MGRAS_TEX_TL_S_LEFT:
    case MGRAS_TEX_TL_T_BOTTOM:
        /* The transfer's shape, which the model takes from its DMA. */
        return true;
    default:
        return false;
    }
}

static const char *const mgras_tram_names[MGRAS_TRAM_STATES] = {
    [MGRAS_TRAM_IDLE] = "nothing",
    [MGRAS_TRAM_SAVE_OPEN] = "a TRAM save",
    [MGRAS_TRAM_SAVE_READ] = "a TRAM save's read",
    [MGRAS_TRAM_RESTORE_OPEN] = "a TRAM restore",
    [MGRAS_TRAM_CLIENT_OPEN] = "the client's TRAM restore",
    [MGRAS_TRAM_GE_DMA_OPEN] = "a 0x9d10 transfer",
};

/* Open a transfer; one still open is dropped, logged. */
static void mgras_tram_open(SGIMgrasState *s, uint32_t state)
{
    if (s->tram_transfer != MGRAS_TRAM_IDLE &&
        s->tram_transfer != MGRAS_TRAM_SAVE_READ) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: %s opened while %s was "
                      "waiting for its kick; that one is dropped\n",
                      mgras_tram_names[state],
                      mgras_tram_names[s->tram_transfer]);
    }
    s->tram_transfer = state;
}

/* A GE word count in three 9-bit parts, as 0xd80c and 0x9d10 carry it. */
static uint32_t mgras_tram_words(const uint32_t *payload)
{
    return (payload[0] & 0x1ff) << 18 | (payload[1] & 0x1ff) << 9 |
           (payload[2] & 0x1ff);
}

/*
 * 0x9d10: word count in three 9-bit parts and the GE routine. For the
 * load routine under TL_SPEC's 64-by-8,192 shape it is the client's TRAM
 * restore, whose rows the load's rectangle at 0xe34 names: x 0, the first
 * row, 64 wide and the rows tall. Anything else is a transfer to the GE
 * the model does not take.
 */
static void mgras_tex_ge_dma(SGIMgrasState *s, const uint32_t *payload)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t size = s->ge_param[MGRAS_TEX_GE_SIZE];
    uint32_t w = size & 0xffff, h = size >> 16;

    if (payload[MGRAS_TEX_GE_DMA_ROUTINE] != MGRAS_TEX_ROUTINE_LOAD ||
        t->tl_spec != MGRAS_TEX_SPEC_WHOLE) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: 0x9d10 transfer to GE routine "
                      "0x%x under TL_SPEC 0x%x is not modelled; its kick "
                      "moves nothing\n", payload[MGRAS_TEX_GE_DMA_ROUTINE],
                      t->tl_spec);
        mgras_tram_open(s, MGRAS_TRAM_GE_DMA_OPEN);
        return;
    }
    if (s->ge_param[MGRAS_TEX_GE_X] || w != MGRAS_TRAM_ROW_WORDS || !h ||
        s->ge_param[MGRAS_TEX_GE_Y] > MGRAS_TRAM_ROWS - h) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: TRAM restore of %u by %u "
                      "at %u, %u is not whole rows of TRAM; its kick moves "
                      "nothing\n", w, h, s->ge_param[MGRAS_TEX_GE_X],
                      s->ge_param[MGRAS_TEX_GE_Y]);
        mgras_tram_open(s, MGRAS_TRAM_GE_DMA_OPEN);
        return;
    }
    mgras_tram_open(s, MGRAS_TRAM_CLIENT_OPEN);
    s->tram_words = mgras_tram_words(payload);
    s->tram_first_row = s->ge_param[MGRAS_TEX_GE_Y];
    s->tram_rows = h;
}

bool sgi_mgras_texture_execute(SGIMgrasState *s, uint32_t header,
                               const uint32_t *payload, uint32_t words)
{
    SGIMgrasTexture *t = mgras_tex(s);

    if (header == MGRAS_TEX_TABLE) {
        return mgras_tex_present(s) && mgras_tex_table(s, payload);
    }
    if (!sgi_mgras_texture_header(header)) {
        return false;
    }
    if (!mgras_tex_present(s)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: class-0 packet 0x%08x is not "
                      "modelled on a board without TRAM\n", header);
        return true;
    }
    switch (header) {
    case MGRAS_TEX_ON:
    case MGRAS_TEX_OFF:
        t->on = header == MGRAS_TEX_ON;
        mgras_tex_update_enable(s);
        return true;
    case MGRAS_TEX_T_OFFSET:
        t->t_offset = payload[0];
        return true;
    case MGRAS_TEX_BASE:
        t->base = payload[0];
        return true;
    case MGRAS_TEX_LOAD_OPEN:
        if (t->bracket) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: texture load opened "
                          "inside an open one\n");
        }
        t->bracket = 1;
        t->wide_branch = MGRAS_TEX_WIDE_NONE;
        return true;
    case MGRAS_TEX_LOAD_CLOSE:
        if (!t->bracket) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture load closed "
                          "without an opening 0x9b00\n");
            /* The bare close of the wide-level branch (see load_start). */
            t->wide_branch = t->wide_branch == MGRAS_TEX_WIDE_LOADED ?
                             MGRAS_TEX_WIDE_CLOSED : MGRAS_TEX_WIDE_NONE;
        }
        if (t->load) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: texture load closed "
                          "before its transfer; nothing stored\n");
            t->load = 0;
        }
        t->bracket = 0;
        return true;
    case MGRAS_TEX_TRAM_READ:
        mgras_tram_open(s, MGRAS_TRAM_SAVE_OPEN);
        s->tram_rows = payload[0];
        return true;
    case MGRAS_TEX_TRAM_GE_COUNT:
        mgras_tram_open(s, MGRAS_TRAM_RESTORE_OPEN);
        s->tram_words = mgras_tram_words(payload);
        return true;
    case MGRAS_TEX_GE_DMA:
        mgras_tex_ge_dma(s, payload);
        return true;
    case MGRAS_TEX_TRAM_RESTORE:
        return true;
    case MGRAS_TEX_RECORDS:
        /* The client rounds the record words up to an even count. */
        if (words != 2 || payload[1] != ((payload[0] * 3 + 1) & ~1u)) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: raster register "
                          "records packet of %u records in %u words\n",
                          payload[0], payload[1]);
        }
        return true;
    default:
        g_assert_not_reached();
    }
}

/*
 * The bytes a word of TRAM gives a transfer of `bytes` over `rows` rows of
 * 64 words: 1 to 4, or 0 when the two do not fit (see "TRAM" above).
 */
static uint32_t mgras_tram_word_bytes(uint64_t bytes, uint32_t rows)
{
    uint64_t words = (uint64_t)rows * MGRAS_TRAM_ROW_WORDS;

    if (!words || bytes % words || bytes / words < 1 ||
        bytes / words > MGRAS_TRAM_WORD_BYTES) {
        return 0;
    }
    return bytes / words;
}

/*
 * The select image a save or restore of k bytes a TRAM word moves. The
 * client saves and restores a one- or two-component texture one or two
 * bytes a word over 64 rows of 64 words (traced on perfly's town
 * database). Its save (mgrTM_SaveData) sends the texture's select in
 * TEXMODE1 bits 8:7 and, in TL_MODE bits 4:3, a value it makes differ
 * from the texture's select (the select XOR 1 when the last one sent was
 * the texture's): established from the client's listing at
 * 0xd99fcc8-0xd99fd4c, and traced as saves under TL_MODE 0x18 and
 * TEXMODE1 0x140 right after a load under TL_MODE 0x34e30. Its restore
 * (0xd99fe10) is a load and sends the texture's select in TL_MODE bits
 * 4:3 (0xd99fe78-0xd99fe94; traced under 0x4038, 0x4030 and 0x402a). So a
 * save of fewer than four bytes a word reads the image of TEXMODE1's
 * select and such a restore writes the image of TL_MODE's. That the
 * board reads by the TEXMODE1 select is inferred from the client. A
 * transfer of whole words, the kernel's and the client's save of an RGB
 * or RGBA texture, takes select 0's image: on the board it moves every
 * TRAM of those words, but the model keeps the other selects apart and
 * cannot carry them in one word's bytes, so it moves select 0 alone
 * (approximated; see "Storage").
 */
static uint32_t mgras_tram_transfer_select(SGIMgrasState *s, uint32_t k,
                                           bool save)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t mode = save ? MGRAS_TEXMODE1_LAYOUT(t->texmode1) << 1 |
                           ((t->texmode1 & MGRAS_TEXMODE1_SELECT) >> 7) << 3
                         : t->tl_mode;

    return k < MGRAS_TRAM_WORD_BYTES ? mgras_tex_select(mode) : 0;
}

/* Read rows of TRAM from first_row back to host memory. */
static void mgras_tram_save(SGIMgrasState *s, uint32_t kick_byte)
{
    uint64_t first = (uint64_t)s->tram_read_block * MGRAS_TRAM_BLOCK_ROWS;
    uint64_t bytes = sgi_mgras_raster_dma_bytes(s);
    uint32_t k = mgras_tram_word_bytes(bytes, s->tram_rows);
    uint64_t words = (uint64_t)s->tram_rows * MGRAS_TRAM_ROW_WORDS;
    uint32_t select = mgras_tram_transfer_select(s, k, true);
    uint64_t image_bit = mgras_tram_image_bit(select);
    uint8_t *image = s->tram + image_bit / 8;
    g_autofree uint8_t *buf = NULL;

    if (first + s->tram_rows > MGRAS_TRAM_ROWS) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: TRAM save of %u rows from "
                      "block %u runs past TRAM; nothing moved\n",
                      s->tram_rows, s->tram_read_block);
        return;
    }
    /*
     * A save of a select's blocks nothing has written since they were
     * last cleared reads zeros, which a later restore would put back as
     * texels: keep it observable.
     */
    for (uint64_t b = first / MGRAS_TRAM_BLOCK_ROWS;
         b * MGRAS_TRAM_BLOCK_ROWS < first + s->tram_rows; b++) {
        if (select &&
            !mgras_tram_written(s, image_bit / MGRAS_TRAM_BLOCK_BITS + b)) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: TRAM save of block %"
                          PRIu64 " under select %u, which holds no level\n",
                          b, select);
            break;
        }
    }
    buf = g_malloc(bytes);
    for (uint64_t i = 0; i < words; i++) {
        memcpy(buf + i * k, image + (first * MGRAS_TRAM_ROW_WORDS + i) *
               MGRAS_TRAM_WORD_BYTES, k);
    }
    if (sgi_mgras_raster_dma_copy(s, kick_byte, buf, bytes, true)) {
        trace_sgi_mgras_tram_transfer("save", s->tram_read_block, s->tram_rows,
                                      k, select, mgras_tex(s)->tl_mode);
    }
}

/*
 * Write rows of TRAM from host memory: the kernel's from its block's first
 * row through the descriptor's rows, the client's from its rectangle's
 * first row through the page-list form.
 */
static void mgras_tram_restore(SGIMgrasState *s, uint32_t kick_byte,
                               bool client)
{
    uint64_t bytes = (uint64_t)s->tram_words * sizeof(uint32_t);
    uint32_t rows = client ? s->tram_rows : s->tram_kernel_rows;
    uint64_t first = (uint64_t)s->tram_load_block * MGRAS_TRAM_BLOCK_ROWS +
                     (client ? s->tram_first_row : 0);
    uint32_t k = mgras_tram_word_bytes(bytes, rows);
    uint64_t words = (uint64_t)rows * MGRAS_TRAM_ROW_WORDS;
    g_autofree uint8_t *buf = NULL;
    uint64_t image;
    bool ok;

    if (!k || first + rows > MGRAS_TRAM_ROWS) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: TRAM restore of %u rows "
                      "from block %u row %" PRIu64 " out of %" PRIu64 " bytes "
                      "does not fit TRAM; nothing moved\n", rows,
                      s->tram_load_block,
                      first % MGRAS_TRAM_BLOCK_ROWS, bytes);
        return;
    }
    buf = g_malloc(bytes);
    ok = client ? sgi_mgras_raster_dma_page_list_read(s, kick_byte, buf, bytes)
                : sgi_mgras_raster_dma_copy(s, kick_byte, buf, bytes, false);
    if (!ok) {
        return;
    }
    image = mgras_tram_image_bit(mgras_tram_transfer_select(s, k, false));
    mgras_tex_border_invalidate_tram(
        s, NULL, image + first * MGRAS_TRAM_ROW_WORDS * 32, words * 32);
    for (uint64_t i = 0; i < words; i++) {
        memcpy(s->tram + image / 8 + (first * MGRAS_TRAM_ROW_WORDS + i) *
               MGRAS_TRAM_WORD_BYTES, buf + i * k, k);
    }
    mgras_tram_mark(s, image + first * MGRAS_TRAM_ROW_WORDS * 32, words * 32);
    if (k == MGRAS_TRAM_WORD_BYTES) {
        mgras_tram_unmark_selects(s, first * MGRAS_TRAM_ROW_WORDS * 32,
                                  words * 32);
    }
    trace_sgi_mgras_tram_transfer(client ? "client restore" : "restore",
                                  s->tram_load_block, rows, k,
                                  mgras_tram_transfer_select(s, k, false),
                                  mgras_tex(s)->tl_mode);
}

bool sgi_mgras_texture_tram_kick(SGIMgrasState *s, uint32_t kick_byte,
                                 bool read)
{
    uint32_t state = s->tram_transfer;

    if (state == MGRAS_TRAM_IDLE) {
        return false;
    }
    if (state == MGRAS_TRAM_SAVE_READ) {
        /* Its 0xda00 never came; this kick is another transfer's. */
        s->tram_transfer = MGRAS_TRAM_IDLE;
        return false;
    }
    s->tram_transfer = MGRAS_TRAM_IDLE;
    /*
     * An open transfer whose kick never came must not take another
     * transfer's: a kick of the other direction, or a read whose bytes do
     * not fill whole words of its rows, ends it, logged, and is taken as
     * any other kick.
     */
    if (read != (state == MGRAS_TRAM_SAVE_OPEN) ||
        (read && !mgras_tram_word_bytes(sgi_mgras_raster_dma_bytes(s),
                                        s->tram_rows))) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: %s was ended by a %s "
                      "kick (0x%02x) of %" PRIu64 " bytes not its own; "
                      "dropped, and the kick taken as any other\n",
                      mgras_tram_names[state], read ? "read" : "write",
                      kick_byte, sgi_mgras_raster_dma_bytes(s));
        return false;
    }
    switch (state) {
    case MGRAS_TRAM_SAVE_OPEN:
        mgras_tram_save(s, kick_byte);
        s->tram_transfer = MGRAS_TRAM_SAVE_READ;
        break;
    case MGRAS_TRAM_RESTORE_OPEN:
        mgras_tram_restore(s, kick_byte, false);
        break;
    case MGRAS_TRAM_CLIENT_OPEN:
        mgras_tram_restore(s, kick_byte, true);
        break;
    default:
        /* A 0x9d10 transfer the model does not take, logged at 0x9d10. */
        break;
    }
    return true;
}

bool sgi_mgras_texture_tram_read_done(SGIMgrasState *s)
{
    if (s->tram_transfer != MGRAS_TRAM_SAVE_READ) {
        return false;
    }
    s->tram_transfer = MGRAS_TRAM_IDLE;
    return true;
}

bool sgi_mgras_texture_transfer_valid(const SGIMgrasState *s)
{
    return s->tram_transfer < MGRAS_TRAM_STATES &&
           s->tram_read_block < 2 * SGI_MGRAS_TRAM_BLOCKS &&
           s->tram_load_block < 2 * SGI_MGRAS_TRAM_BLOCKS &&
           s->tram_first_row < MGRAS_TRAM_ROWS &&
           s->tram_kernel_txaddr < SGI_MGRAS_TEX_TABLE &&
           s->tram_kernel_tladdr < SGI_MGRAS_TEX_TABLE;
}

void sgi_mgras_texture_board_reset(SGIMgrasState *s)
{
    if (s->tram) {
        memset(s->tram, 0, SGI_MGRAS_TRAM_IMAGE_BYTES);
    }
    memset(s->tram_written, 0, sizeof(s->tram_written));
    for (uint32_t i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        mgras_tex_border_reset(&s->gl_context[i].tex);
    }
    s->tram_transfer = MGRAS_TRAM_IDLE;
    s->tram_rows = s->tram_words = s->tram_first_row = 0;
    s->tram_read_block = s->tram_load_block = 0;
    s->tram_kernel_txaddr = s->tram_kernel_tladdr = 0;
    s->tram_kernel_rows = 0;
}

bool sgi_mgras_texture_records(SGIMgrasState *s, uint32_t header)
{
    uint32_t *slot = s->cmd_token_payload;
    uint32_t offset = (header & SGI_MGRAS_CMD_BLOCK_OFFSET) >> 22;
    uint32_t bytes = header & SGI_MGRAS_CMD_BLOCK_BYTES;

    if (!mgras_tex_present(s) || s->cmd_token_class != 0 ||
        s->cmd_token_reg != SGI_MGRAS_TEX_RECORDS_OP ||
        s->cmd_token_payload_count != 2) {
        return false;
    }
    if (offset || bytes != slot[MGRAS_TEX_REC_WORDS] * sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: raster register records "
                      "block of %u bytes at offset %u after a packet giving "
                      "%u words\n", bytes, offset, slot[MGRAS_TEX_REC_WORDS]);
    }
    s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_GL_BLOCK;
    s->cmd_token_words_remaining = DIV_ROUND_UP(offset + bytes,
                                                sizeof(uint32_t));
    s->cmd_token_payload_count = SGI_MGRAS_TEX_RECORD_SLOTS;
    slot[MGRAS_TEX_REC_NEXT] = 0;
    if (!s->cmd_token_words_remaining) {
        s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_DISCARD;
    }
    return true;
}

void sgi_mgras_texture_record_word(SGIMgrasState *s, uint32_t word)
{
    uint32_t *slot = s->cmd_token_payload;
    uint32_t k = slot[MGRAS_TEX_REC_NEXT]++;

    /* Records count off first; the pad word after them is dropped. */
    if (k / 3 < slot[MGRAS_TEX_REC_COUNT]) {
        switch (k % 3) {
        case 0:
            slot[MGRAS_TEX_REC_REG] = word;
            break;
        case 1:
            slot[MGRAS_TEX_REC_VALUE] = word;
            break;
        default:
            mgras_tex_register(s, slot[MGRAS_TEX_REC_REG],
                               slot[MGRAS_TEX_REC_VALUE]);
            break;
        }
    }
    if (!s->cmd_token_words_remaining) {
        s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_DISCARD;
    }
}

static bool mgras_tex_volume_spec_shape(uint32_t spec, uint32_t width,
                                        uint32_t height)
{
    uint32_t log2w = MGRAS_TEX_SPEC_LOG2W(spec);
    uint32_t log2h = MGRAS_TEX_SPEC_LOG2H(spec);

    /* The captured volume spec has one extra width exponent. */
    return MGRAS_TEX_SPEC_LEVEL(spec) == 0 && log2w >= 2 && log2h >= 1 &&
           width == 1u << (log2w - 1) && height == 1u << log2h;
}

/* Stock-client packet selectors observed for these volume dimensions. */
static bool mgras_tex_stock_volume_packet(const uint32_t *payload,
                                           uint32_t width, uint32_t height)
{
    static const struct {
        uint8_t width, height, box_x, box_y;
    } shapes[] = {
        { 16, 16, 1, 1 }, { 32, 16, 2, 2 },
        { 8, 32, 1, 1 }, { 32, 8, 1, 1 }, { 16, 32, 2, 2 },
        { 32, 32, 1, 5 }, { 64, 16, 1, 5 }, { 64, 64, 1, 0x15 },
    };

    if (payload[0] != 0x40) {
        return false;
    }
    for (size_t i = 0; i < ARRAY_SIZE(shapes); i++) {
        if (shapes[i].width == width && shapes[i].height == height &&
            shapes[i].box_x == payload[3] &&
            shapes[i].box_y == payload[4]) {
            return true;
        }
    }
    return false;
}

static bool mgras_tex_captured_volume_depth(uint32_t depth)
{
    return depth == 1 || depth == 2 || depth == 4 || depth == 8 ||
           depth == 16 || depth == 32 || depth == 64;
}

static bool mgras_tex_captured_deep_volume_shape(uint32_t width,
                                                 uint32_t height,
                                                 uint32_t depth)
{
    return (width == 8 && height == 8 &&
            (depth == 8 || depth == 16 || depth == 32 || depth == 64)) ||
           (width == 32 && height == 32 && depth == 16) ||
           (width == 16 && height == 16 && depth == 64);
}

static bool mgras_tex_volume_packet_valid(const uint32_t *payload,
                                          uint32_t depth, uint32_t spec,
                                          uint32_t width, uint32_t height)
{
    if (payload[1] || payload[2] || payload[5] != depth ||
        payload[6] != 0x511a) {
        return false;
    }
    if (depth == 1) {
        return spec == 0x120 && width == 2 && height == 2 &&
               payload[0] == 4 && payload[3] == 1 && payload[4] == 0 &&
               payload[7] == 0x98;
    }
    if (!mgras_tex_captured_volume_depth(depth) || payload[7] != 0x198 ||
        !mgras_tex_volume_spec_shape(spec, width, height) ||
        (depth > 4 &&
         !mgras_tex_captured_deep_volume_shape(width, height, depth))) {
        return false;
    }
    if (depth == 4 &&
        mgras_tex_stock_volume_packet(payload, width, height)) {
        return true;
    }
    if (depth > 4 && (width != 8 || height != 8)) {
        return mgras_tex_stock_volume_packet(payload, width, height);
    }
    return payload[0] == (uint64_t)width * height && payload[3] == 1 &&
           payload[4] == 0;
}

/* Keep the observed volume upload encodings separate from ordinary TRAM loads. */
static bool mgras_tex_captured_volume(SGIMgrasTexture *t,
                                      const SGIMgrasState *s,
                                      const uint32_t *payload)
{
    uint32_t size = s->ge_param[MGRAS_TEX_GE_SIZE];
    uint32_t depth = payload[5];
    uint32_t width = size & 0xffff, height = size >> 16;
    uint32_t box_width = s->ge_param[MGRAS_TEX_GE_X + 2];
    uint32_t box_height = s->ge_param[MGRAS_TEX_GE_X + 3];

    if (t->tl_mode != 0x34026 || s->ge_param[MGRAS_TEX_GE_X] ||
        s->ge_param[MGRAS_TEX_GE_Y] ||
        !mgras_tex_volume_packet_valid(payload, depth, t->tl_spec,
                                       width, height)) {
        return false;
    }
    if (depth == 1) {
        return box_width == 1 && (box_height == 1 || box_height == 2);
    }
    return box_width == width && box_height == height;
}

bool sgi_mgras_texture_load_start(SGIMgrasState *s, const uint32_t *payload)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t spec = t->tl_spec, mode = t->tl_mode;
    uint32_t w = 1u << MGRAS_TEX_SPEC_LOG2W(spec);
    uint32_t h = 1u << MGRAS_TEX_SPEC_LOG2H(spec);
    uint32_t x = s->ge_param[MGRAS_TEX_GE_X];
    uint32_t y = s->ge_param[MGRAS_TEX_GE_Y];
    uint32_t size = s->ge_param[MGRAS_TEX_GE_SIZE];
    uint32_t rw = size & 0xffff, rh = size >> 16;
    uint32_t hq = s->dma_desc_30000[0];
    uint32_t comps, csize;
    bool abgr, volume;
    uint64_t bit;


    if (!mgras_tex_present(s) ||
        payload[MGRAS_TEX_ROUTINE_WORD] != MGRAS_TEX_ROUTINE_LOAD) {
        return false;
    }
    if (t->load) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: texture load ended "
                      "before its transfer by the next\n");
        t->load = 0;
    }
    /*
     * The client's wide-level branch: after a level whose width is 512
     * texel units it sends a bare 0x9c00, rewrites TL_SPEC with the width
     * doubled and sends a second transfer with no 0x9b00 (established
     * from the client). distort's 512-wide bordered level was traced
     * doing so: eight 64-by-1 rows at x 512 to 704 under TL_SPEC 0x9a0
     * right after its bracketed loads under 0x990. What the board makes
     * of them is unknown, so they are refused and nothing is stored;
     * taking them as a load of a level twice as wide would replace the
     * level the bracket just loaded, its ring included. Only that traced
     * shape is refused: wide_branch says a bracketed load of a 512-wide
     * level was followed by a bare 0x9c00 and nothing else since, and the
     * transfer's TL_SPEC must be that level's with the width doubled.
     * Any other unbracketed transfer is taken, as before.
     */
    if (!t->bracket && t->wide_branch == MGRAS_TEX_WIDE_CLOSED &&
        MGRAS_TEX_SPEC_LEVEL(spec) == MGRAS_TEX_SPEC_LEVEL(t->load_spec) &&
        MGRAS_TEX_SPEC_LOG2H(spec) == MGRAS_TEX_SPEC_LOG2H(t->load_spec) &&
        MGRAS_TEX_SPEC_LOG2W(spec) == MGRAS_TEX_WIDE_LOG2W + 1) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture transfer outside a "
                      "0x9b00 bracket under TL_SPEC 0x%x, twice the width "
                      "of the level loaded before it (0x%x), is not "
                      "modelled; nothing stored\n", spec, t->load_spec);
        return true;
    }
    t->wide_branch = MGRAS_TEX_WIDE_NONE;
    if (!t->bracket) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture transfer outside a "
                      "0x9b00 bracket, taken as a load\n");
    }
    if (!(payload[MGRAS_TEX_FLAGS_WORD] & MGRAS_TEX_FLAGS_BY_DMA)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture load with its image "
                      "inline is not modelled; nothing stored\n");
        return true;
    }
    /*
     * The image's shape comes from the pixel program's unpack block (see
     * mgras_tex_unpack()), not from decoding the program, so the load
     * is keyed on routine 0x511a and sized by the rectangle the size
     * word names. Approximated: the rest of the program is not checked,
     * so one that reorders or converts components beyond what is handled
     * here would be taken as the identity. A block with an unseen word
     * is refused.
     *
     * Components of one byte are the kept depth's own bytes. Components
     * of two bytes are read big-endian and reduced to the kept depth:
     * round(c * 255 / 65535) for 8 bits, that value's top four for 4
     * bits, and c >> 4 for 12 bits. Approximated: the board's own
     * conversion is inferred (OpenGL 1.2.1 defines a component as
     * c / 65535, which the reduction follows).
     *
     * The HQ pixel format's GL_ABGR_EXT bit is taken for an image of four
     * components kept as RGBA, with HQ bits 1:0 (bytes a component)
     * agreeing with the scale word, and stored with the components
     * reversed: the same word and program draw as alpha, blue, green, red
     * through the pixel path. That the texture load reorders as the pixel
     * path does is inferred, the word, program and kick being the same.
     * An ABGR image of two components (luminance alpha) was never traced
     * and is refused, logged.
    */
    volume = mgras_tex_captured_volume(t, s, payload);
    if (payload[5] != 1 && !volume) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture upload descriptor "
                      "with unsupported depth %u is not modelled; "
                      "nothing stored\n", payload[5]);
        return true;
    }
    if (!mgras_tex_unpack(s, &comps, &csize)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture load through a pixel "
                      "program whose unpack block is not one seen in a "
                      "traced load is not modelled; nothing stored\n");
        return true;
    }
    abgr = hq & MGRAS_TEX_HQ_ABGR;
    if (!mgras_tex_image_fits(mode, comps, csize) ||
        (abgr && (comps != 4 || MGRAS_TEX_HQ_COMPS(hq) != 3 ||
                  (hq & 3) != csize))) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture load of an image of %u "
                      "components of %u bytes (HQ pixel format 0x%x) kept "
                      "as TL_MODE 0x%x is not modelled; nothing stored\n",
                      comps, csize, hq, mode);
        return true;
    }
    if (!mgras_tex_rect_fits(x, y, rw, rh, w, h)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture load of %u by %u at "
                      "%d, %d (size word 0x%x) is not inside TL_SPEC 0x%x's "
                      "%u by %u and its one-texel border; nothing stored\n",
                      rw, rh, (int32_t)x, (int32_t)y, size, spec, w, h);
        return true;
    }
    if (MGRAS_TEX_DEPTH(mode) > MGRAS_TEX_DEPTH_12) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture kept as TL_MODE 0x%x "
                      "(layout %u, depth code %u) is not modelled; nothing "
                      "stored\n", mode, MGRAS_TEX_LAYOUT(mode),
                      MGRAS_TEX_DEPTH(mode));
        return true;
    }
    if (!volume &&
        !mgras_tram_place(t->base, s->ge_param[MGRAS_TEX_GE_BLOCK],
                          MGRAS_TEX_SPEC_LOG2W(spec),
                          MGRAS_TEX_SPEC_LOG2H(spec), mode, &bit)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: texture level of %u by "
                      "%u at base %u, TXMIPMAP word 0x%x runs past TRAM's "
                      "end; nothing stored\n", w, h, t->base,
                      s->ge_param[MGRAS_TEX_GE_BLOCK]);
        return true;
    }
    t->load = 1;
    t->wide_branch = t->bracket && !volume &&
                     MGRAS_TEX_SPEC_LOG2W(spec) == MGRAS_TEX_WIDE_LOG2W ?
                     MGRAS_TEX_WIDE_LOADED : MGRAS_TEX_WIDE_NONE;
    t->load_depth = volume ? payload[5] : 1;
    t->load_volume = volume;
    memcpy(t->load_packet, payload, sizeof(t->load_packet));
    t->load_dma_bytes = 0;
    t->load_base = t->base;
    t->load_block = s->ge_param[MGRAS_TEX_GE_BLOCK];
    t->load_spec = spec;
    t->load_mode = mode;
    t->load_x = x;
    t->load_y = y;
    t->load_size = size;
    t->load_comps = comps;
    t->load_csize = csize;
    t->load_abgr = abgr;
    return true;
}

bool sgi_mgras_texture_dma(SGIMgrasState *s, uint32_t rows, uint32_t bpr,
                           bool *take)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint64_t host = (uint64_t)(t->load_size & 0xffff) *
                    (t->load_size >> 16) * mgras_tex_load_texel(t) *
                    t->load_depth;


    *take = false;
    if (!t->load) {
        return false;
    }
    t->load_dma_bytes = (uint64_t)rows * bpr;
    /*
     * ivview's levels came as one row and perfly's strips a row of the
     * strip at a time; rows are taken end to end, and the whole
     * rectangle's bytes must arrive: width times height times the image's
     * bytes a texel (the unpack block's components times bytes a
     * component), which also refuses row padding.
     */
    if ((t->load_volume && host > SGI_MGRAS_VOLUME_TEX_BYTES) ||
        (uint64_t)rows * bpr != host) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: DMA of %u rows of %u "
                      "bytes for a texture level of %" PRIu64 " bytes; "
                      "nothing stored\n", rows, bpr, host);
        t->load = 0;
        t->load_volume = t->load_depth = 0;
        return true;
    }
    mgras_tex_staging_free(t);
    t->staging = g_malloc(host);
    t->staging_bytes = host;
    *take = true;
    return true;
}

/*
 * Put the gathered host image into TRAM as the load's rectangle of its
 * level, converted to the kept format (see "Depth" above), at the place
 * the level's base, TXMIPMAP word and size give it.
 */
static uint32_t mgras_tex_upload_component(const uint8_t *in, uint32_t n,
                                           uint32_t size, uint32_t depth);
static void mgras_tex_upload_components(const uint8_t *in, uint32_t src,
                                       uint32_t size, uint32_t comps,
                                       uint32_t depth, bool abgr,
                                       uint32_t texel, uint16_t component[4]);

static void mgras_tex_commit(SGIMgrasState *s)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t level = MGRAS_TEX_SPEC_LEVEL(t->load_spec);
    uint32_t log2w = MGRAS_TEX_SPEC_LOG2W(t->load_spec);
    uint32_t log2h = MGRAS_TEX_SPEC_LOG2H(t->load_spec);
    uint32_t mode = t->load_mode;
    uint32_t comps = mgras_tex_components(mode);
    uint32_t depth = mgras_tex_depth_bits(mode);
    uint32_t slot = mgras_tex_slot_bits(mode);
    /* The image's components a texel, and bytes a component. */
    uint32_t src = t->load_comps;
    uint32_t size = t->load_csize;
    uint32_t rw = t->load_size & 0xffff, rh = t->load_size >> 16;
    const uint8_t *in = t->staging;
    uint32_t w = 1u << log2w, h = 1u << log2h;
    bool stored = false, border_stored = false;
    bool touches_border;
    uint64_t start;
    uint64_t image_bits = ((uint64_t)slot << log2w) << log2h;
    uint32_t border_key = mgras_tex_border_key(t->load_base, level);

    if (t->load_volume) {
        uint64_t bytes = (uint64_t)rw * rh * src * size * t->load_depth;


        if (mode != 0x34026 || src != 4 || size != 1 ||
            !mgras_tex_volume_spec_shape(t->load_spec, rw, rh) ||
            t->load_x || t->load_y ||
            bytes != t->staging_bytes || bytes != t->load_dma_bytes ||
            bytes > SGI_MGRAS_VOLUME_TEX_BYTES) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: captured volume upload "
                          "does not match the bounded RGBA8 layout; "
                          "nothing stored\n");
            goto done;
        }
        size_t head_bytes = MIN((size_t)bytes, sizeof(t->volume_data));

        memcpy(t->volume_data, t->staging, head_bytes);
        if (bytes > head_bytes) {
            memcpy(t->volume_data_extra, t->staging + head_bytes,
                   bytes - head_bytes);
        } else {
            memset(t->volume_data_extra, 0, sizeof(t->volume_data_extra));
        }
        t->volume_bytes = bytes;
        t->volume_width = rw;
        t->volume_height = rh;
        t->volume_depth = t->load_depth;
        t->volume_base = t->load_base;
        t->volume_mode = mode & 0xff;
        t->volume_level = level;
        t->loaded_levels |= 1u << level;
        goto done;
    }
    t->volume_bytes = 0;
    t->volume_width = t->volume_height = t->volume_depth = 0;
    t->volume_base = t->volume_mode = t->volume_level = 0;
    touches_border = (int32_t)t->load_x < 0 ||
                     (int32_t)t->load_y < 0 ||
                     (int64_t)(int32_t)t->load_x + rw > w ||
                     (int64_t)(int32_t)t->load_y + rh > h;
    if (!mgras_tram_place(t->load_base, t->load_block, log2w, log2h, mode,
                          &start)) {
        /* load_start placed it; only a migrated load can get here. */
        goto done;
    }
    /*
     * The model caches uploaded border texels outside the TRAM image and
     * offers them for sampling only when the ring is complete. The board's
     * placement and sampling are unknown.
     */
    mgras_tex_border_invalidate_tram(s, t, start, image_bits);
    mgras_tex_border_prepare(t, border_key, t->load_base, mode, w, h,
                             start, image_bits, touches_border);
    for (uint32_t row = 0; row < rh; row++, in += rw * src * size) {
        int64_t ty = (int64_t)(int32_t)t->load_y + row;

        for (uint32_t texel = 0; texel < rw; texel++) {
            int64_t tx = (int64_t)(int32_t)t->load_x + texel;
            uint32_t border_index;

            if (mgras_tex_border_index(w, h, tx, ty, &border_index)) {
                uint16_t component[4];

                mgras_tex_upload_components(in, src, size, comps, depth,
                                            t->load_abgr, texel, component);
                mgras_tex_border_store(t, border_key, border_index, component);
                border_stored = true;
            }
            if (tx < 0 || tx >= w || ty < 0 || ty >= h) {
                continue;
            }
            for (uint32_t component = 0; component < comps; component++) {
                uint32_t k = t->load_abgr ? comps - 1 - component : component;
                uint32_t n = texel * src + k;
                uint64_t bit;
                uint32_t v;

                bit = start + (((uint64_t)ty << log2w) + tx) * slot +
                      component * depth;
                if (k >= src) {
                    v = (1u << depth) - 1;
                } else {
                    v = mgras_tex_upload_component(in, n, size, depth);
                }
                mgras_tram_put(s->tram, bit, depth, v);
                stored = true;
            }
        }
    }
    if (!stored && !border_stored) {
        goto done;
    }
    if (stored) {
        mgras_tram_mark(s, start, image_bits);
        if (MGRAS_TEX_LAYOUT(mode) > MGRAS_TEX_LAYOUT_LA) {
            mgras_tram_unmark_selects(s, start, image_bits);
        }
        t->loaded_levels |= 1u << level;
        trace_sgi_mgras_texture_level(t->load_base, t->load_block, level,
                                      1u << log2w, 1u << log2h, mode);
    }
    goto done;

done:
    mgras_tex_staging_free(t);
    t->load = 0;
    t->load_volume = t->load_depth = 0;
}

bool sgi_mgras_texture_row(SGIMgrasState *s, const uint8_t *bytes,
                           uint32_t n)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t take;

    if (!t->staging) {
        return false;
    }
    if (n > t->staging_bytes - t->staged) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: texture DMA row exceeds "
                      "the staged image; nothing stored\n");
        mgras_tex_staging_free(t);
        t->load = 0;
        t->load_volume = t->load_depth = 0;
        t->load_dma_bytes = 0;
        return true;
    }
    take = n;
    memcpy(t->staging + t->staged, bytes, take);
    t->staged += take;
    if (t->staged == t->staging_bytes) {
        mgras_tex_commit(s);
    }
    return true;
}

void sgi_mgras_texture_abort(SGIMgrasState *s)
{
    SGIMgrasTexture *t = mgras_tex(s);

    mgras_tex_staging_free(t);
    t->load = 0;
    t->load_volume = t->load_depth = 0;
}

/*
 * A stored texel as the sampler's canonical RRGGBBAA, a 4-bit component
 * replicated to 8 bits and a 12-bit one scaled to 8. A one-component
 * texel is set in both the red and alpha bytes and a two-component one's
 * luminance in red, since the class that reads it is the draw's and the
 * sampler takes the byte it needs.
 */
static uint32_t mgras_tex_upload_component(const uint8_t *in, uint32_t n,
                                           uint32_t size, uint32_t depth)
{
    if (size == 1) {
        return depth == 4 ? in[n] >> 4 : in[n];
    }
    uint32_t c = lduw_be_p(&in[2 * n]);
    return depth == 12 ? c >> 4 :
           depth == 8 ? (c + 128) / 257 : (c + 128) / 257 >> 4;
}

static void mgras_tex_upload_components(const uint8_t *in, uint32_t src,
                                       uint32_t size, uint32_t comps,
                                       uint32_t depth, bool abgr,
                                       uint32_t texel, uint16_t component[4])
{
    memset(component, 0, 4 * sizeof(*component));
    for (uint32_t i = 0; i < comps; i++) {
        uint32_t k = abgr ? comps - 1 - i : i;
        uint32_t n = texel * src + k;

        component[i] = k >= src ? (1u << depth) - 1 :
                       mgras_tex_upload_component(in, n, size, depth);
    }
}

static uint32_t mgras_tex_components_to_rgba(uint32_t mode, uint32_t format,
                                             const uint16_t component[4])
{
    uint32_t depth = mgras_tex_depth_bits(mode);
    uint32_t c[4] = { 0, 0, 0, 0 };

    for (uint32_t i = 0; i < 4; i++) {
        c[i] = depth == 4 ? component[i] * 0x11 :
               depth == 8 ? component[i] :
               (component[i] * 255 + 2047) / 4095;
    }
    switch (format) {
    case SGI_GL_ALPHA:
        return c[3];
    case SGI_GL_LUMINANCE:
    case SGI_GL_INTENSITY:
        return c[0] << 24 | c[0];
    case SGI_GL_LUMINANCE_ALPHA:
        return c[0] << 24 | c[3];
    case SGI_GL_RGB:
        return c[0] << 24 | c[1] << 16 | c[2] << 8;
    default:
        return c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3];
    }
}

static bool mgras_tex_read_volume_texel(void *opaque, uint32_t level,
                                        uint32_t x, uint32_t y, uint32_t z,
                                        uint32_t *rgba)
{
    const SGIMgrasTexView *l = opaque;
    uint32_t comps = mgras_tex_components(l->mode);
    uint32_t depth = mgras_tex_depth_bits(l->mode);
    uint64_t bit = l->bit + (((uint64_t)y << l->log2w) + x) *
                   mgras_tex_slot_bits(l->mode);
    uint32_t c[4];

    if (l->volume_data) {
        uint32_t width = 1u << l->log2w;
        uint32_t height = 1u << l->log2h;
        uint64_t offset;
        const uint8_t *p;

        if (z >= l->depth || x >= width || y >= height) {
            return false;
        }
        offset = ((uint64_t)z * height + y) * width * 4 + x * 4;
        if (offset < SGI_MGRAS_VOLUME_TEX_BASE_BYTES) {
            p = &l->volume_data[offset];
        } else {
            p = &l->volume_data_extra[
                offset - SGI_MGRAS_VOLUME_TEX_BASE_BYTES];
        }
        *rgba = p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
        return true;
    }
    if (z) {
        return false;
    }
    for (uint32_t k = 0; k < comps; k++) {
        uint32_t v = mgras_tram_get(l->tram, bit + k * depth, depth);

        switch (depth) {
        case 4:
            c[k] = v * 0x11;
            break;
        case 8:
            c[k] = v;
            break;
        default:
            /*
             * The sampler's texels are 8-bit, so a 12-bit component is
             * rounded to the nearest of 255 (approximated): precision
             * beyond the 8-bit store is lost before filtering.
             */
            c[k] = (v * 255 + 2047) / 4095;
            break;
        }
    }
    switch (MGRAS_TEX_LAYOUT(l->mode)) {
    case MGRAS_TEX_LAYOUT_ONE:
        *rgba = c[0] << 24 | c[0];
        break;
    case MGRAS_TEX_LAYOUT_LA:
        *rgba = c[0] << 24 | c[1];
        break;
    case MGRAS_TEX_LAYOUT_RGB:
        *rgba = c[0] << 24 | c[1] << 16 | c[2] << 8;
        break;
    default:
        *rgba = c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3];
        break;
    }
    return true;
}

static bool mgras_tex_read_volume_components(void *opaque, uint32_t level,
                                             uint32_t x, uint32_t y,
                                             uint32_t z,
                                             uint16_t component[4])
{
    const SGIMgrasTexView *l = opaque;
    uint32_t comps = mgras_tex_components(l->mode);
    uint32_t depth = mgras_tex_depth_bits(l->mode);
    uint64_t bit = l->bit + (((uint64_t)y << l->log2w) + x) *
                   mgras_tex_slot_bits(l->mode);
    uint16_t stored[4] = { 0, 0, 0, 0 };

    if (level != l->level) {
        return false;
    }
    memset(component, 0, 4 * sizeof(*component));
    if (l->volume_data) {
        uint32_t width = 1u << l->log2w;
        uint32_t height = 1u << l->log2h;
        uint64_t offset;
        const uint8_t *p;

        if (z >= l->depth || x >= width || y >= height) {
            return false;
        }
        offset = ((uint64_t)z * height + y) * width * 4 + x * 4;
        p = offset < SGI_MGRAS_VOLUME_TEX_BASE_BYTES ?
            &l->volume_data[offset] :
            &l->volume_data_extra[offset - SGI_MGRAS_VOLUME_TEX_BASE_BYTES];
        for (uint32_t i = 0; i < 4; i++) {
            component[i] = p[i];
        }
        return true;
    }
    if (z) {
        return false;
    }
    for (uint32_t i = 0; i < comps; i++) {
        stored[i] = mgras_tram_get(l->tram, bit + i * depth, depth);
    }
    switch (l->format) {
    case SGI_GL_ALPHA:
        component[3] = stored[0];
        break;
    case SGI_GL_LUMINANCE:
    case SGI_GL_INTENSITY:
        component[0] = stored[0];
        break;
    case SGI_GL_LUMINANCE_ALPHA:
        component[0] = stored[0];
        component[3] = stored[1];
        break;
    case SGI_GL_RGB:
    case SGI_GL_RGBA:
        component[0] = stored[0];
        component[1] = stored[1];
        component[2] = stored[2];
        if (l->format == SGI_GL_RGBA) {
            component[3] = stored[3];
        }
        break;
    default:
        return false;
    }
    return true;
}

static bool mgras_tex_read_texel_components(void *opaque, uint32_t level,
                                            uint32_t x, uint32_t y,
                                            uint16_t component[4])
{
    return mgras_tex_read_volume_components(opaque, level, x, y, 0,
                                            component);
}

static bool mgras_tex_read_texel(void *opaque, uint32_t level, uint32_t x,
                                 uint32_t y, uint32_t *rgba)
{
    return mgras_tex_read_volume_texel(opaque, level, x, y, 0, rgba);
}

static bool mgras_tex_read_image_texel_3d(void *opaque, uint32_t x,
                                          uint32_t y, uint32_t z,
                                          uint32_t *rgba)
{
    SGIMgrasTexture *t = opaque;

    return mgras_tex_read_volume_texel(&t->views[0], 0, x, y, z, rgba);
}

static bool mgras_tex_read_image_texel(void *opaque, uint32_t x, uint32_t y,
                                       uint32_t *rgba)
{
    return mgras_tex_read_image_texel_3d(opaque, x, y, 0, rgba);
}

static bool mgras_tex_read_image_components(void *opaque, uint32_t x,
                                            uint32_t y,
                                            uint16_t component[4])
{
    SGIMgrasTexture *t = opaque;

    return mgras_tex_read_volume_components(&t->views[0], 0, x, y, 0,
                                            component);
}

static bool mgras_tex_read_image_components_3d(void *opaque, uint32_t x,
                                               uint32_t y, uint32_t z,
                                               uint16_t component[4])
{
    SGIMgrasTexture *t = opaque;

    return mgras_tex_read_volume_components(&t->views[0], 0, x, y, z,
                                            component);
}

static bool mgras_tex_color_table_ready(const SGIMgrasTexture *t,
                                        uint32_t mode)
{
    return mode == MGRAS_TEX_COLOR_TABLE_MODE &&
           SGI_MGRAS_TEXTURE_COLOR_TABLE_ENTRIES == 256 &&
           SGI_MGRAS_TEXTURE_COLOR_TABLE_BYTES ==
               3 * SGI_MGRAS_TEXTURE_COLOR_TABLE_ENTRIES &&
           t->color_table_selector == MGRAS_TEX_COLOR_TABLE_SELECT &&
           t->color_table_active && t->color_table_valid &&
           t->color_table_written == sizeof(t->color_table_data);
}

static void mgras_tex_color_table_post_filter(void *opaque, double rgba[4])
{
    SGIMgrasTexture *t = opaque;

    for (uint32_t channel = 0; channel < 3; channel++) {
        double scaled = MIN(MAX(rgba[channel], 0.0), 1.0) *
                        (SGI_MGRAS_TEXTURE_COLOR_TABLE_ENTRIES - 1);
        uint32_t index = floor(scaled + 0.5);

        rgba[channel] = t->color_table_data[
            channel * SGI_MGRAS_TEXTURE_COLOR_TABLE_ENTRIES + index] / 255.0;
    }
}

/* The sampler's base format for a layout and the class the draw names. */
static bool mgras_tex_read_border_components(void *opaque, uint32_t level,
                                             int32_t x, int32_t y,
                                             uint16_t component[4])
{
    const SGIMgrasTexView *l = opaque;
    uint32_t w = 1u << l->log2w, h = 1u << l->log2h, index;
    uint64_t value;
    uint16_t stored[4];

    if (level != l->level || !l->border_data ||
        !mgras_tex_border_index(w, h, x, y, &index) ||
        index >= l->border_count) {
        return false;
    }
    value = l->border_data[l->border_offset + index];
    if (!(value & (1ull << 48))) {
        return false;
    }
    for (uint32_t i = 0; i < 4; i++) {
        stored[i] = (value >> (12 * i)) & 0xfff;
    }
    memset(component, 0, 4 * sizeof(*component));
    switch (l->format) {
    case SGI_GL_ALPHA:
        component[3] = stored[0];
        break;
    case SGI_GL_LUMINANCE:
    case SGI_GL_INTENSITY:
        component[0] = stored[0];
        break;
    case SGI_GL_LUMINANCE_ALPHA:
        component[0] = stored[0];
        component[3] = stored[1];
        break;
    case SGI_GL_RGB:
    case SGI_GL_RGBA:
        memcpy(component, stored, sizeof(stored));
        if (l->format == SGI_GL_RGB) {
            component[3] = 0;
        }
        break;
    default:
        return false;
    }
    return true;
}

static bool mgras_tex_read_border_texel(void *opaque, uint32_t level,
                                        int32_t x, int32_t y,
                                        uint32_t *rgba)
{
    const SGIMgrasTexView *l = opaque;
    uint16_t component[4];

    if (!mgras_tex_read_border_components(opaque, level, x, y, component)) {
        return false;
    }
    *rgba = mgras_tex_components_to_rgba(l->mode, l->format, component);
    return true;
}

static void mgras_tex_component_bits(uint32_t format, uint32_t depth,
                                     uint8_t bits[4])
{
    memset(bits, 0, 4 * sizeof(*bits));
    switch (format) {
    case SGI_GL_ALPHA:
        bits[3] = depth;
        break;
    case SGI_GL_LUMINANCE:
    case SGI_GL_INTENSITY:
        bits[0] = depth;
        break;
    case SGI_GL_LUMINANCE_ALPHA:
        bits[0] = bits[3] = depth;
        break;
    case SGI_GL_RGB:
        bits[0] = bits[1] = bits[2] = depth;
        break;
    case SGI_GL_RGBA:
        bits[0] = bits[1] = bits[2] = bits[3] = depth;
        break;
    }
}

static bool mgras_tex_read_image_border_components(void *opaque, int32_t x,
                                                    int32_t y,
                                                    uint16_t component[4])
{
    SGIMgrasTexture *t = opaque;

    return mgras_tex_read_border_components(&t->views[0], 0, x, y, component);
}

static bool mgras_tex_format(uint32_t layout, uint32_t cls, uint32_t *format)
{
    switch (layout) {
    case MGRAS_TEX_LAYOUT_ONE:
        *format = cls == MGRAS_TEX_CLASS_ALPHA ? SGI_GL_ALPHA :
                  cls == MGRAS_TEX_CLASS_LUMINANCE ? SGI_GL_LUMINANCE :
                  cls == MGRAS_TEX_CLASS_INTENSITY ? SGI_GL_INTENSITY : 0;
        break;
    case MGRAS_TEX_LAYOUT_LA:
        *format = cls == MGRAS_TEX_CLASS_LUMINANCE ?
                  SGI_GL_LUMINANCE_ALPHA : 0;
        break;
    case MGRAS_TEX_LAYOUT_RGB:
        *format = cls == MGRAS_TEX_CLASS_COLOUR ? SGI_GL_RGB : 0;
        break;
    default:
        *format = cls == MGRAS_TEX_CLASS_COLOUR ? SGI_GL_RGBA : 0;
        break;
    }
    return *format;
}

/*
 * The wrap of one axis. A set axis bit clamps, and bit 14 says whether it
 * is GL_CLAMP (set) or clamp to border (clear) for every clamped axis at
 * once, as the client's one flag does. With a nearest filter a clamp
 * reads the edge texel and clamp to border the border colour outside the
 * image; with a linear one the neighbours beyond the edge are the border
 * colour for both. Those semantics are OpenGL 1.2.1 3.8.4 and the
 * SGIS_texture_border_clamp rules, the latter not read: approximated, as
 * the board's coordinate rounding is unknown.
 */
static uint32_t mgras_tex_wrap(uint32_t texmode2, uint32_t axis_bit)
{
    if (!(texmode2 & axis_bit)) {
        return SGI_GL_REPEAT;
    }
    return texmode2 & MGRAS_TEXMODE2_CLAMP ? SGI_GL_CLAMP :
                                             SGI_GL_CLAMP_TO_BORDER;
}

/* A 12-bit colour field, 0 to 4095, as a fraction. */
static float mgras_tex_c12(uint32_t v)
{
    return (v & 0xfff) / 4095.0f;
}

/*
 * Give the shared draw state the environment the draw's TEXMODE1 names.
 * The state's environment is the sampler's input and the board's is
 * TEXMODE1 bits 2:1, so the two are made one here, at the draw.
 *
 * The client sends GL_REPLACE as modulate (code 0) and the model draws
 * what the word says, which differs from OpenGL replace unless the
 * fragment is white: a stated limitation. Alpha textures come with code
 * 3, drawn as the OpenGL alpha function, which is modulate's. Decal on a
 * one- or two-component texel is undefined in OpenGL; the model draws
 * modulate. The environment colour is 0x142 = R12 | G12 << 12 and 0x143 =
 * B12; the client never sends its alpha, so the model uses 0, which
 * only an intensity texture under blend reads (approximated). Code 3 with
 * a colour class is not one the client sends and is not drawn.
 */
static bool mgras_tex_environment(SGIMgrasState *s, uint32_t cls)
{
    SGIMgrasTexture *t = mgras_tex(s);
    SGIGLDrawState *st = &s->gl_context[s->gl_current].gl->cur;
    uint32_t env = MGRAS_TEXMODE1_ENV(t->texmode1);

    if (cls == MGRAS_TEX_CLASS_ALPHA) {
        env = MGRAS_TEX_ENV_MODULATE;
    } else if (env == MGRAS_TEX_ENV_ALPHA) {
        return false;
    } else if (env == MGRAS_TEX_ENV_DECAL && cls != MGRAS_TEX_CLASS_COLOUR) {
        env = MGRAS_TEX_ENV_MODULATE;
    }
    st->texture_env_mode = env == MGRAS_TEX_ENV_DECAL ? SGI_GL_DECAL :
                           env == MGRAS_TEX_ENV_BLEND ? SGI_GL_BLEND :
                                                        SGI_GL_MODULATE;
    st->texture_env_color[0] = mgras_tex_c12(t->txenv[0]);
    st->texture_env_color[1] = mgras_tex_c12(t->txenv[0] >> 12);
    st->texture_env_color[2] = mgras_tex_c12(t->txenv[1]);
    st->texture_env_color[3] = 0;
    return true;
}

/*
 * Level n of the draw, of the kept format mode, out of TRAM: TXMIPMAP
 * entry n under the context's base, the base size halved n times. False
 * when it would run past TRAM or no load or restore has written its first
 * block since reset.
 */
static bool mgras_tex_draw_level(SGIMgrasState *s, uint32_t n, uint32_t mode,
                                 SGIMgrasTexView *view)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t log2w = t->txsize & 0xf, log2h = (t->txsize >> 4) & 0xf;

    log2w = log2w > n ? log2w - n : 0;
    log2h = log2h > n ? log2h - n : 0;
    view->border_data = NULL;
    view->level = n;
    view->border_offset = view->border_count = 0;
    if ((t->texmode2 & MGRAS_TEXMODE2_VOLUME) && t->volume_bytes &&
        n == t->volume_level && t->volume_base == t->base &&
        t->volume_mode == mode) {
        view->tram = NULL;
        view->volume_data = t->volume_data;
        view->volume_data_extra = t->volume_data_extra;
        view->log2w = ctz32(t->volume_width);
        view->log2h = ctz32(t->volume_height);
        view->mode = mode;
        view->depth = t->volume_depth;
        return true;
    }
    if (!(t->loaded_levels & (1u << n)) ||
        !mgras_tram_place(t->base, t->txmipmap[n], log2w, log2h, mode,
                          &view->bit) ||
        !mgras_tram_written(s, view->bit / MGRAS_TRAM_BLOCK_BITS)) {
        return false;
    }
    view->tram = s->tram;
    view->log2w = log2w;
    view->log2h = log2h;
    view->mode = mode;
    view->depth = 1;
    view->volume_data = NULL;
    view->volume_data_extra = NULL;
    uint32_t border_key = mgras_tex_border_key(t->base, n);
    uint64_t image_bits = ((uint64_t)mgras_tex_slot_bits(mode) << log2w)
                          << log2h;

    if (t->border_count[border_key] &&
        t->border_base[border_key] == t->base &&
        MGRAS_TEX_LAYOUT(t->border_mode[border_key]) ==
            MGRAS_TEX_LAYOUT(mode) &&
        MGRAS_TEX_DEPTH(t->border_mode[border_key]) ==
            MGRAS_TEX_DEPTH(mode) &&
        t->border_width[border_key] == (1u << log2w) &&
        t->border_height[border_key] == (1u << log2h) &&
        t->border_start[border_key] == view->bit &&
        t->border_bits[border_key] == image_bits &&
        t->border_valid_count[border_key] ==
        t->border_count[border_key]) {
        view->border_data = t->border_data;
        view->border_offset = t->border_offset[border_key];
        view->border_count = t->border_count[border_key];
    }
    return true;
}

/*
 * Mipmapped draws. The client sends bits 5 and 19 when a mipmap filter
 * draws more than one level; bit 8 is then the blend within a level and
 * bit 6 between levels, the four filters of the datasheet's table.
 * TXLOD's last level (3:0) is the highest level drawn and its bits 25:16
 * the minimum LOD in 64ths, clamped to the last level; both are the
 * client's own arithmetic and established as words. A non-mipmap draw
 * uses level 0 alone. The sampler derives lambda from the iterators as
 * the board does, takes the nearest level d = ceil(lambda + 0.5) - 1
 * floored at the base level, and for the blend between levels uses
 * d1 = d2 = last at or past the last level, so no level beyond it is
 * read: all OpenGL 1.2.1 3.8.5 (equations 3.21 to 3.23), approximated
 * since the board's lambda, rounding and blend weights are unknown.
 * SGIS_texture_lod, which the TXLOD bits follow, was not read. A level
 * the draw selects but that was never loaded draws untextured, logged.
 * Levels whose kept format differs from level 0's are left out, which
 * leaves a hole.
 */
static void mgras_tex_pyramid(SGIMgrasState *s, uint32_t mode,
                              uint32_t format, bool mip)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t n = 0, last = mip ? MGRAS_TXLOD_LAST(t->txlod) : 0;
    double min_lod = mip ? MIN(MGRAS_TXLOD_MIN(t->txlod) / 64.0, last) : 0;

    for (uint32_t level = 0; level <= last; level++) {
        SGIMgrasTexView *e = &t->views[n];

        if (!mgras_tex_draw_level(s, level, mode, e)) {
            continue;
        }
        uint8_t component_bits[4];
        SGIGLTexLevel *sampler_level = &t->sampler_levels[n++];

        e->format = format;
        mgras_tex_component_bits(format, mgras_tex_depth_bits(mode),
                                 e->component_bits);
        memcpy(component_bits, e->component_bits, sizeof(component_bits));
        *sampler_level = (SGIGLTexLevel) {
            .level = level,
            .width = 1u << e->log2w,
            .height = 1u << e->log2h,
            .depth = e->depth,
            .base_format = format,
            .internal_format = format,
            .border = e->border_data ? 1 : 0,
            .read_texel = mgras_tex_read_texel,
            .read_border_texel = e->border_data ?
                                 mgras_tex_read_border_texel : NULL,
            .read_texel_3d = mgras_tex_read_volume_texel,
            .opaque = e,
            .component_bits = { component_bits[0], component_bits[1],
                                component_bits[2], component_bits[3] },
            .read_texel_components = mgras_tex_read_texel_components,
            .read_border_texel_components = e->border_data ?
                mgras_tex_read_border_components : NULL,
            .read_texel_3d_components = mgras_tex_read_volume_components,
        };
    }
    t->pyramid = (SGIGLTexPyramid) {
        .levels = t->sampler_levels,
        .count = n,
        .max_level = last,
        .min_lod = min_lod,
        .max_lod = last,
    };
}

/* TXSIZE repeats the volume axes; TXCLAMPSIZE gives their texel counts. */
static bool mgras_tex_volume_draw_shape(const SGIMgrasTexture *t,
                                        uint32_t *width, uint32_t *height)
{
    uint32_t w = t->txclampsize >> 13;
    uint32_t h = t->txclampsize & 0x1fff;
    uint32_t log2w, log2h, spec_size;

    if (w < 2 || h < 2 || (w & (w - 1)) || (h & (h - 1)) ||
        t->txclampsize != (w << 13 | h)) {
        return false;
    }
    log2w = ctz32(w);
    log2h = ctz32(h);
    if (log2w > 14 || log2h > 15) {
        return false;
    }
    spec_size = ((log2h << 4 | log2w) << 8) |
                (log2h << 4 | (log2w + 1));
    if ((t->txsize & 0xffff) != spec_size) {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

static uint32_t mgras_tex_volume_txsize(uint32_t width, uint32_t height,
                                        uint32_t depth)
{
    uint32_t log2w = ctz32(width);
    uint32_t log2h = ctz32(height);
    uint32_t spec_size = ((log2h << 4 | log2w) << 8) |
                         (log2h << 4 | (log2w + 1));

    return (ctz32(depth) << 19) | spec_size;
}

/* Keep captured pixel draws bounded to RGBA8 volumes with known controls. */
static bool mgras_tex_captured_pixel_endpoint(const SGIMgrasTexture *t)
{
    uint32_t width, height, depth_size, control;

    if (t->texmode1 != 0x40859 || t->texmode2 != 0x815804 ||
        t->txlod != 1 || !mgras_tex_volume_draw_shape(t, &width, &height)) {
        return false;
    }
    depth_size = 1u << ((t->txsize >> 19) & 0xf);
    if (depth_size == 1) {
        return t->txsize == 0x1112 && width == 2 && height == 2 &&
               t->mddma_cntrl == 0x3c040045;
    }
    if (depth_size > 4) {
        if (!mgras_tex_captured_deep_volume_shape(width, height,
                                                   depth_size)) {
            return false;
        }
        if (width == 32 && height == 32) {
            control = 0x0c041155;
        } else if (width == 16 && height == 16) {
            control = 0x14041911;
        } else {
            switch (depth_size) {
            case 8:
                control = 0x08040ccd;
                break;
            case 16:
                control = 0x0c0410cd;
                break;
            case 32:
                control = 0x100414cd;
                break;
            case 64:
                control = 0x140418cd;
                break;
            default:
                return false;
            }
        }
        return
               t->txsize == mgras_tex_volume_txsize(width, height,
                                                    depth_size) &&
               t->txclampsize == (width << 13 | height) &&
               t->mddma_cntrl == control &&
               t->volume_width == width && t->volume_height == height &&
               t->volume_depth == depth_size && t->volume_mode == 0x26 &&
               t->volume_level == 0 && t->volume_base == t->base &&
               t->volume_bytes == (uint64_t)width * height * depth_size * 4;
    }
    if (depth_size != 2 && depth_size != 4) {
        return false;
    }
    control = depth_size == 4 ? 0x4040845 : 0x40445;
    control += (ctz32(width) - 1) * 4 + (ctz32(height) - 1) * 0x40;
    if (width == 64 && (height == 16 || height == 64)) {
        if (depth_size != 4) {
            return false;
        }
        control += 0x04800000;
    }
    return t->mddma_cntrl == control && t->volume_width == width &&
           t->volume_height == height && t->volume_depth == depth_size &&
           t->volume_mode == 0x26 && t->volume_level == 0 &&
           t->volume_base == t->base &&
           t->volume_bytes == (uint64_t)width * height * depth_size * 4;
}

/* Ordinary point captures select only the exact uploaded volume tuple. */
static bool mgras_tex_captured_ordinary_volume(const SGIMgrasTexture *t,
                                                uint32_t depth_size)
{
    uint32_t width, height, control;

    if (depth_size == 1) {
        return t->texmode1 == 0x40859 && t->texmode2 == 0x815804 &&
               t->txlod == 1 && t->txsize == 0x1112 &&
               t->txclampsize == 0x4002 &&
               t->mddma_cntrl == 0x3c000044 &&
               t->volume_width == 2 && t->volume_height == 2 &&
               t->volume_depth == 1 && t->volume_mode == 0x26 &&
               t->volume_level == 0 && t->volume_base == t->base &&
               t->volume_bytes == 16;
    }
    if ((depth_size != 2 && depth_size != 4 && depth_size != 8 &&
         depth_size != 16 && depth_size != 32 && depth_size != 64) ||
        !mgras_tex_volume_draw_shape(t, &width, &height)) {
        return false;
    }
    if (depth_size > 4) {
        if (!mgras_tex_captured_deep_volume_shape(width, height,
                                                   depth_size)) {
            return false;
        }
        if (width == 32 && height == 32) {
            control = 0x0c001154;
        } else if (width == 16 && height == 16) {
            control = 0x14001910;
        } else {
            switch (depth_size) {
            case 8:
                control = 0x08000ccc;
                break;
            case 16:
                control = 0x0c0010cc;
                break;
            case 32:
                control = 0x100014cc;
                break;
            case 64:
                control = 0x140018cc;
                break;
            default:
                return false;
            }
        }
        return
               t->texmode1 == 0x40859 && t->texmode2 == 0x815804 &&
               t->txsize == mgras_tex_volume_txsize(width, height,
                                                    depth_size) &&
               t->txclampsize == (width << 13 | height) && t->txlod == 1 &&
               t->mddma_cntrl == control && t->volume_width == width &&
               t->volume_height == height && t->volume_depth == depth_size &&
               t->volume_mode == 0x26 && t->volume_level == 0 &&
               t->volume_base == t->base &&
               t->volume_bytes == (uint64_t)width * height * depth_size * 4;
    }
    if (width == 64 && (height == 16 || height == 64)) {
        if (depth_size != 4) {
            return false;
        }
        control = 0x844 + (ctz32(width) - 1) * 4 +
                  (ctz32(height) - 1) * 0x40;
        control |= 0x08800000;
    } else {
        control = (depth_size == 4 ? 0x44 : 0x444) +
                  (ctz32(width) - 1) * 4 +
                  (ctz32(height) - 1) * 0x40;
        if (depth_size == 4) {
            control |= 0x04000800;
        }
    }

    return t->texmode1 == 0x40859 && t->texmode2 == 0x815804 &&
           t->txlod == 1 && t->mddma_cntrl == control &&
           t->volume_width == width && t->volume_height == height &&
           t->volume_depth == depth_size &&
           t->volume_mode == 0x26 && t->volume_level == 0 &&
           t->volume_base == t->base &&
           t->volume_bytes == (uint64_t)width * height * depth_size * 4;
}

const SGIGLTexImage *sgi_mgras_texture_image(SGIMgrasState *s,
                                             SGIGLTexImage *image)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t size = t->txsize & 0xff, format;
    uint32_t depth_size = 1u << ((t->txsize >> 19) & 0xf);
    bool volume = !!(t->texmode2 & MGRAS_TEXMODE2_VOLUME);
    uint32_t cls = MGRAS_TEXMODE1_CLASS(t->texmode1);
    uint32_t layout = MGRAS_TEXMODE1_LAYOUT(t->texmode1);
    uint32_t depth = (t->texmode2 >> 2) & 3;
    /*
     * The kept format the draw names, in TL_MODE's fields, with the TRAM
     * select of TEXMODE1 bits 8:7 for a one- or two-component layout.
     */
    uint32_t mode = layout << 1 | depth << 5 |
                    (layout <= MGRAS_TEX_LAYOUT_LA ?
                     ((t->texmode1 & MGRAS_TEXMODE1_SELECT) >> 7) << 3 : 0);
    uint32_t mip = t->texmode2 & MGRAS_TEXMODE2_MIP;
    bool pixel_endpoint = mgras_tex_captured_pixel_endpoint(t);
    bool ordinary_volume = mgras_tex_captured_ordinary_volume(t,
                                                               depth_size);
    bool captured_endpoint = pixel_endpoint || ordinary_volume;
    uint32_t color_table_mode = t->texmode1 & MGRAS_TEXMODE1_TABLE;
    bool color_table;
    uint32_t min_filter;
    if (!mgras_tex_present(s) ||
        !s->gl_context[s->gl_current].gl->cur.texture_2d) {
        return NULL;
    }
    /* These tuples select the bounded logical-volume software view. */
    if (captured_endpoint) {
        mip = 0;
    }
    /*
     * TXSIZE's two copies of the base size must agree, as every traced
     * two-dimensional draw sent them. The class is the draw's (TEXMODE1
     * 11:9) and must suit the layout. Class 1, alpha, comes with
     * environment code 3 from the client, whatever glTexEnv set; the
     * model reads the alpha class as its function for every code.
     */
    /*
     * Captures include depth-two and depth-four descriptors declaring 32 and
     * 64 bytes. The depth-two second-slice payload used in tests is synthetic;
     * physical slice placement remains unknown.
     */
    if ((!captured_endpoint &&
         (t->txsize & 0xffff) != (size << 8 | size)) ||
        (!volume && depth_size != 1) || depth_size > 64 ||
        depth > MGRAS_TEX_DEPTH_12 ||
        (color_table_mode && color_table_mode != MGRAS_TEX_COLOR_TABLE_MODE) ||
        !mgras_tex_environment(s, cls) ||
        !mgras_tex_format(layout, cls, &format) ||
        (t->texmode2 & MGRAS_TEXMODE2_UNDRAWN) ||
        (volume && mip) ||
        (mip && mip != MGRAS_TEXMODE2_MIP) ||
        t->t_offset) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture state TEXMODE1 0x%x "
                      "TEXMODE2 0x%x TXSIZE 0x%x offset 0x%x is not "
                      "modelled; drawn untextured\n", t->texmode1,
                      t->texmode2, t->txsize, t->t_offset);
        return NULL;
    }
    if (!mgras_tex_draw_level(s, 0, mode, &t->views[0]) ||
        (volume && t->views[0].depth != depth_size)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: textured draw with no level 0 "
                      "loaded under base %u at TXMIPMAP 0x%x\n", t->base,
                      t->txmipmap[0]);
        return NULL;
    }
    mgras_tex_pyramid(s, mode, format, mip);
    if (mip) {
        min_filter = t->texmode2 & MGRAS_TEXMODE2_MIN_LINEAR ?
            (t->texmode2 & MGRAS_TEXMODE2_MIP_BETWEEN ?
             SGI_GL_LINEAR_MIPMAP_LINEAR : SGI_GL_LINEAR_MIPMAP_NEAREST) :
            (t->texmode2 & MGRAS_TEXMODE2_MIP_BETWEEN ?
             SGI_GL_NEAREST_MIPMAP_LINEAR : SGI_GL_NEAREST_MIPMAP_NEAREST);
    } else {
        /* Bit 6 without bits 5 and 19 is a one-level draw or a 3-D one. */
        min_filter = t->texmode2 & MGRAS_TEXMODE2_MIN_LINEAR ?
                     SGI_GL_LINEAR : SGI_GL_NEAREST;
    }
    color_table = mgras_tex_color_table_ready(t, color_table_mode);
    *image = (SGIGLTexImage) {
        .width = 1u << t->views[0].log2w,
        .height = 1u << t->views[0].log2h,
        .base_format = format,
        .border = 0,
        .read_texel = mgras_tex_read_image_texel,
        .read_texel_3d = mgras_tex_read_image_texel_3d,
        .opaque = t,
        /*
         * Bit 8 is linear inside a level and 7 linear magnification:
         * inferred from the client's filter table, and the weights are
         * OpenGL 1.2.1 3.8.5's, approximated since the board's blend
         * precision is unknown.
         */
        .min_filter = min_filter,
        .mag_filter = t->texmode2 & MGRAS_TEXMODE2_MAG_LINEAR ?
                      SGI_GL_LINEAR : SGI_GL_NEAREST,
        .wrap_s = mgras_tex_wrap(t->texmode2, MGRAS_TEXMODE2_WRAP_S),
        .wrap_t = mgras_tex_wrap(t->texmode2, MGRAS_TEXMODE2_WRAP_T),
        .wrap_r = mgras_tex_wrap(t->texmode2, MGRAS_TEXMODE2_WRAP_R),
        .depth = depth_size,
        .volume = volume,
        /*
         * TXBCOLOR is R12 | G12 << 12 and B12 | A12 << 12. The sampler
         * reduces it to the texture's own format, which is the model's
         * choice (approximated; the board's reduction is unknown).
         */
        .border_color = {
            mgras_tex_c12(t->txbcolor[0]), mgras_tex_c12(t->txbcolor[0] >> 12),
            mgras_tex_c12(t->txbcolor[1]), mgras_tex_c12(t->txbcolor[1] >> 12),
        },
        .pyramid = &t->pyramid,
        .post_filter = color_table ? mgras_tex_color_table_post_filter : NULL,
        .read_texel_components = mgras_tex_read_image_components,
        .read_texel_3d_components = mgras_tex_read_image_components_3d,
        .read_border_texel_components = mgras_tex_read_image_border_components,
    };
    mgras_tex_component_bits(format, mgras_tex_depth_bits(mode),
                             image->component_bits);
    return image;
}

bool sgi_mgras_texture_pixel(SGIMgrasState *s, const float rgba[4],
                             float out[4])
{
    static const float white[4] = { 1, 1, 1, 1 };
    SGIGLTexImage image;
    const SGIGLTexImage *texture;

    if (!(mgras_tex(s)->mddma_cntrl & 1)) {
        return false;
    }
    texture = sgi_mgras_texture_image(s, &image);
    return texture && sgi_gl_texture_apply_pixel(
        &s->gl_context[s->gl_current].gl->cur, texture, rgba, white, out);
}

bool sgi_mgras_texture_valid(const SGIMgrasTexture *t)
{
    uint64_t bit;

    if (t->on > 1 || t->bracket > 1 || t->load > 1 ||
        t->wide_branch > MGRAS_TEX_WIDE_CLOSED ||
        (t->loaded_levels & ~((1u << SGI_MGRAS_TEX_LEVELS) - 1)) ||
        t->txaddr >= SGI_MGRAS_TEX_TABLE || t->tl_addr >= SGI_MGRAS_TEX_TABLE ||
        t->staging || t->volume_bytes > SGI_MGRAS_VOLUME_TEX_BYTES ||
        t->color_table_active > 1 || t->color_table_valid > 1 ||
        t->color_table_written > SGI_MGRAS_TEXTURE_COLOR_TABLE_BYTES ||
        (t->color_table_active &&
         t->color_table_selector != MGRAS_TEX_COLOR_TABLE_SELECT) ||
        (t->color_table_valid &&
         (!t->color_table_active ||
          t->color_table_written != SGI_MGRAS_TEXTURE_COLOR_TABLE_BYTES)) ||
        (!t->color_table_active && t->color_table_written)) {
        return false;
    }
    uint32_t border_words = 0;

    if ((t->border_words && !t->border_data) ||
        (!t->border_words && t->border_data)) {
        return false;
    }
    for (uint32_t i = 0; i < SGI_MGRAS_TEX_BORDER_CACHES; i++) {
        uint32_t count = t->border_count[i];
        uint32_t valid = 0;

        if (!count) {
            if (t->border_valid_count[i]) {
                return false;
            }
            continue;
        }
        if ((i / SGI_MGRAS_TEX_LEVELS) != (t->border_base[i] & 0xff) ||
            !t->border_width[i] || !t->border_height[i] ||
            t->border_width[i] > (1u << 15) ||
            t->border_height[i] > (1u << 15) ||
            (t->border_width[i] & (t->border_width[i] - 1)) ||
            (t->border_height[i] & (t->border_height[i] - 1)) ||
            count != mgras_tex_border_pixels(t->border_width[i],
                                              t->border_height[i]) ||
            !t->border_bits[i] ||
            t->border_start[i] > (uint64_t)SGI_MGRAS_TRAM_IMAGE_BYTES * 8 ||
            t->border_bits[i] >
                (uint64_t)SGI_MGRAS_TRAM_IMAGE_BYTES * 8 -
                t->border_start[i] ||
            (uint64_t)t->border_offset[i] + count > t->border_words) {
            return false;
        }
        if (MGRAS_TEX_DEPTH(t->border_mode[i]) > MGRAS_TEX_DEPTH_12 ||
            t->border_bits[i] !=
            (uint64_t)mgras_tex_slot_bits(t->border_mode[i]) *
            t->border_width[i] * t->border_height[i]) {
            return false;
        }
        for (uint32_t j = 0; j < count; j++) {
            uint64_t value = t->border_data[t->border_offset[i] + j];

            uint32_t depth = mgras_tex_depth_bits(t->border_mode[i]);
            uint32_t comps = mgras_tex_components(t->border_mode[i]);
            uint32_t maximum = (1u << depth) - 1;

            if (value >> 49) {
                return false;
            }
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t component = (value >> (12 * k)) & 0xfff;

                if (component > maximum || (k >= comps && component)) {
                    return false;
                }
            }
            valid += !!(value & (1ull << 48));
        }
        if (valid != t->border_valid_count[i]) {
            return false;
        }
        border_words += count;
        for (uint32_t j = i + 1; j < SGI_MGRAS_TEX_BORDER_CACHES; j++) {
            if (t->border_count[j] &&
                (mgras_tex_border_overlap(t->border_start[i],
                                          t->border_bits[i],
                                          t->border_start[j],
                                          t->border_bits[j]) ||
                 mgras_tex_border_overlap(t->border_offset[i], count,
                                          t->border_offset[j],
                                          t->border_count[j]))) {
                return false;
            }
        }
    }
    if (border_words != t->border_words) {
        return false;
    }
    if (t->load) {
        uint32_t w = 1u << MGRAS_TEX_SPEC_LOG2W(t->load_spec);
        uint32_t h = 1u << MGRAS_TEX_SPEC_LOG2H(t->load_spec);
        uint32_t rw = t->load_size & 0xffff, rh = t->load_size >> 16;

        if (MGRAS_TEX_DEPTH(t->load_mode) > MGRAS_TEX_DEPTH_12 ||
            (!t->load_volume &&
             !mgras_tram_place(t->load_base, t->load_block,
                               MGRAS_TEX_SPEC_LOG2W(t->load_spec),
                               MGRAS_TEX_SPEC_LOG2H(t->load_spec),
                               t->load_mode, &bit)) ||
            !mgras_tex_rect_fits(t->load_x, t->load_y, rw, rh, w, h) ||
            t->load_abgr > 1 || t->load_comps < 1 || t->load_comps > 4 ||
            t->load_csize < 1 || t->load_csize > 2 ||
            !mgras_tex_image_fits(t->load_mode, t->load_comps,
                                  t->load_csize) ||
            (t->load_abgr && t->load_comps != 4) ||
            (t->load_volume &&
             (!mgras_tex_volume_packet_valid(t->load_packet, t->load_depth,
                                             t->load_spec, rw, rh) ||
              t->load_mode != 0x34026 || t->load_x || t->load_y ||
              t->load_comps != 4 || t->load_csize != 1))) {
            return false;
        }
    }
    if (t->load_volume > 1 ||
        (t->load && !mgras_tex_captured_volume_depth(t->load_depth)) ||
        (!t->load && t->load_volume) ||
        (!t->volume_bytes &&
         (t->volume_width || t->volume_height || t->volume_depth)) ||
        (t->volume_bytes &&
        (t->volume_width < 2 || t->volume_width > 64 ||
         t->volume_height < 2 || t->volume_height > 64 ||
         (t->volume_width & (t->volume_width - 1)) ||
         (t->volume_height & (t->volume_height - 1)) ||
         !mgras_tex_captured_volume_depth(t->volume_depth) ||
         t->volume_mode != 0x26 ||
         t->volume_bytes != (uint64_t)t->volume_width * t->volume_height *
                            t->volume_depth * 4 ||
         t->volume_level >= SGI_MGRAS_TEX_LEVELS))) {
        return false;
    }
    return true;
}

static bool mgras_tex_border_words_valid(SGIMgrasTexture *t, Error **errp)
{
    uint64_t count = 0;

    if (t->border_words > MGRAS_TEX_BORDER_WORDS_MAX) {
        error_setg(errp, "sgi-mgras: border cache exceeds migration bound");
        return false;
    }
    for (uint32_t i = 0; i < SGI_MGRAS_TEX_BORDER_CACHES; i++) {
        count += t->border_count[i];
        if (count > MGRAS_TEX_BORDER_WORDS_MAX) {
            error_setg(errp, "sgi-mgras: border cache count exceeds "
                       "migration bound");
            return false;
        }
    }
    if (count != t->border_words) {
        error_setg(errp, "sgi-mgras: border cache word count is inconsistent");
        return false;
    }
    return true;
}

static bool mgras_tex_border_words_load(QEMUFile *f, void *pv, size_t size,
                                        const VMStateField *field,
                                        Error **errp)
{
    SGIMgrasTexture *t = (void *)((uint8_t *)pv -
                                  offsetof(SGIMgrasTexture, border_words));

    if (!vmstate_info_uint32.load(f, pv, size, field, errp)) {
        return false;
    }
    /* This field runs before VARRAY_ALLOC, so reject before allocation. */
    return mgras_tex_border_words_valid(t, errp);
}

static bool mgras_tex_border_words_save(QEMUFile *f, void *pv, size_t size,
                                        const VMStateField *field,
                                        JSONWriter *vmdesc, Error **errp)
{
    SGIMgrasTexture *t = (void *)((uint8_t *)pv -
                                  offsetof(SGIMgrasTexture, border_words));

    if (!mgras_tex_border_words_valid(t, errp)) {
        return false;
    }
    return vmstate_info_uint32.save(f, pv, size, field, vmdesc, errp);
}

static const VMStateInfo mgras_tex_border_words_info = {
    .name = "sgi-mgras-texture-border-words",
    .load = mgras_tex_border_words_load,
    .save = mgras_tex_border_words_save,
};

static int mgras_tex_state_post_load(void *opaque, int version_id)
{
    SGIMgrasTexture *t = opaque;

    return sgi_mgras_texture_valid(t) ? 0 : -EINVAL;
}

static const VMStateDescription vmstate_sgi_mgras_tex = {
    .name = TYPE_SGI_MGRAS "/texture-state",
    .version_id = 16,
    .minimum_version_id = 16,
    .post_load = mgras_tex_state_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(texmode1, SGIMgrasTexture),
        VMSTATE_UINT32(texmode2, SGIMgrasTexture),
        VMSTATE_UINT32(txsize, SGIMgrasTexture),
        VMSTATE_UINT32(mddma_cntrl, SGIMgrasTexture),
        VMSTATE_UINT32(txlod, SGIMgrasTexture),
        VMSTATE_UINT32(txclampsize, SGIMgrasTexture),
        VMSTATE_UINT32_ARRAY(txenv, SGIMgrasTexture, 2),
        VMSTATE_UINT32_ARRAY(txbcolor, SGIMgrasTexture, 2),
        VMSTATE_UINT32(txaddr, SGIMgrasTexture),
        VMSTATE_UINT32_ARRAY(txmipmap, SGIMgrasTexture, SGI_MGRAS_TEX_TABLE),
        VMSTATE_UINT32_ARRAY(txborder, SGIMgrasTexture, SGI_MGRAS_TEX_TABLE),
        VMSTATE_UINT32(tl_mode, SGIMgrasTexture),
        VMSTATE_UINT32(tl_spec, SGIMgrasTexture),
        VMSTATE_UINT32(tl_wbuffer, SGIMgrasTexture),
        VMSTATE_UINT32(texrbuffer, SGIMgrasTexture),
        VMSTATE_UINT32(tl_addr, SGIMgrasTexture),
        VMSTATE_UINT32_ARRAY(tl_mipmap, SGIMgrasTexture, SGI_MGRAS_TEX_TABLE),
        VMSTATE_UINT32(base, SGIMgrasTexture),
        VMSTATE_UINT32(t_offset, SGIMgrasTexture),
        VMSTATE_UINT32(on, SGIMgrasTexture),
        VMSTATE_UINT32(bracket, SGIMgrasTexture),
        VMSTATE_UINT32(wide_branch, SGIMgrasTexture),
        VMSTATE_UINT32(loaded_levels, SGIMgrasTexture),
        VMSTATE_UINT32(load, SGIMgrasTexture),
        VMSTATE_UINT32(load_base, SGIMgrasTexture),
        VMSTATE_UINT32(load_block, SGIMgrasTexture),
        VMSTATE_UINT32(load_spec, SGIMgrasTexture),
        VMSTATE_UINT32(load_mode, SGIMgrasTexture),
        VMSTATE_UINT32(load_x, SGIMgrasTexture),
        VMSTATE_UINT32(load_y, SGIMgrasTexture),
        VMSTATE_UINT32(load_size, SGIMgrasTexture),
        VMSTATE_UINT32(load_depth, SGIMgrasTexture),
        VMSTATE_UINT32(load_volume, SGIMgrasTexture),
        VMSTATE_UINT32_ARRAY(load_packet, SGIMgrasTexture, 8),
        VMSTATE_UINT64(load_dma_bytes, SGIMgrasTexture),
        VMSTATE_UINT32(load_comps, SGIMgrasTexture),
        VMSTATE_UINT32(load_csize, SGIMgrasTexture),
        VMSTATE_UINT32(load_abgr, SGIMgrasTexture),
        VMSTATE_UINT32(volume_base, SGIMgrasTexture),
        VMSTATE_UINT32(volume_mode, SGIMgrasTexture),
        VMSTATE_UINT32(volume_level, SGIMgrasTexture),
        VMSTATE_UINT32(volume_width, SGIMgrasTexture),
        VMSTATE_UINT32(volume_height, SGIMgrasTexture),
        VMSTATE_UINT32(volume_depth, SGIMgrasTexture),
        VMSTATE_UINT32(volume_bytes, SGIMgrasTexture),
        VMSTATE_BUFFER(volume_data, SGIMgrasTexture),
        VMSTATE_UINT32(color_table_selector, SGIMgrasTexture),
        VMSTATE_UINT32(color_table_written, SGIMgrasTexture),
        VMSTATE_UINT32(color_table_active, SGIMgrasTexture),
        VMSTATE_UINT32(color_table_valid, SGIMgrasTexture),
        VMSTATE_BUFFER(color_table_data, SGIMgrasTexture),
        VMSTATE_BUFFER(volume_data_extra, SGIMgrasTexture),
        VMSTATE_UINT32_ARRAY(border_base, SGIMgrasTexture,
                             SGI_MGRAS_TEX_BORDER_CACHES),
        VMSTATE_UINT32_ARRAY(border_mode, SGIMgrasTexture,
                             SGI_MGRAS_TEX_BORDER_CACHES),
        VMSTATE_UINT32_ARRAY(border_width, SGIMgrasTexture,
                             SGI_MGRAS_TEX_BORDER_CACHES),
        VMSTATE_UINT32_ARRAY(border_height, SGIMgrasTexture,
                             SGI_MGRAS_TEX_BORDER_CACHES),
        VMSTATE_UINT32_ARRAY(border_offset, SGIMgrasTexture,
                             SGI_MGRAS_TEX_BORDER_CACHES),
        VMSTATE_UINT32_ARRAY(border_count, SGIMgrasTexture,
                             SGI_MGRAS_TEX_BORDER_CACHES),
        VMSTATE_UINT32_ARRAY(border_valid_count, SGIMgrasTexture,
                             SGI_MGRAS_TEX_BORDER_CACHES),
        VMSTATE_UINT64_ARRAY(border_start, SGIMgrasTexture,
                             SGI_MGRAS_TEX_BORDER_CACHES),
        VMSTATE_UINT64_ARRAY(border_bits, SGIMgrasTexture,
                             SGI_MGRAS_TEX_BORDER_CACHES),
        VMSTATE_SINGLE(border_words, SGIMgrasTexture, 0,
                       mgras_tex_border_words_info, uint32_t),
        VMSTATE_VARRAY_ALLOC(border_data, SGIMgrasTexture, border_words,
                             0, vmstate_info_uint64, uint64_t),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_tex = {
    .name = TYPE_SGI_MGRAS "/gl-context-texture",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(tex, SGIMgrasGLContext, 1, vmstate_sgi_mgras_tex,
                       SGIMgrasTexture),
        VMSTATE_END_OF_LIST()
    }
};

/* Whether a context's texture state is a reset's, field by field. */
static bool mgras_tex_is_reset(const SGIMgrasTexture *t)
{
    uint32_t any = t->texmode1 | t->texmode2 | t->txsize | t->txlod |
                   t->mddma_cntrl |
                   t->txclampsize | t->txenv[0] | t->txenv[1] |
                   t->txbcolor[0] | t->txbcolor[1] |
                   t->txaddr | t->tl_mode | t->tl_spec | t->tl_wbuffer |
                   t->texrbuffer | t->tl_addr | t->base | t->t_offset | t->on |
                   t->bracket | t->wide_branch | t->load |
                   t->loaded_levels | t->load_base |
                   t->load_block |
                   t->load_spec | t->load_mode | t->load_x | t->load_y |
                   t->load_size | t->load_depth | t->load_volume |
                   t->load_comps | t->load_csize | t->load_abgr |
                   t->load_dma_bytes | t->volume_bytes |
                   t->volume_width | t->volume_height |
                   t->volume_depth | t->volume_base | t->volume_mode |
                   t->volume_level | t->color_table_selector |
                   t->border_words | t->color_table_written |
                   t->color_table_active |
                   t->color_table_valid;

    for (int i = 0; i < SGI_MGRAS_TEX_TABLE; i++) {
        any |= t->txmipmap[i] | t->txborder[i] | t->tl_mipmap[i];
    }
    return !any;
}

/* Sent once any context has texture state other than a reset's. */
static bool mgras_tex_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (!mgras_tex_is_reset(&s->gl_context[i].tex)) {
            return true;
        }
    }
    return false;
}

/* A transfer's staging buffer is host-only; drop any held here first. */
static int mgras_tex_pre_load(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        sgi_mgras_texture_reset(&s->gl_context[i].tex);
    }
    return 0;
}

const VMStateDescription vmstate_sgi_mgras_texture = {
    .name = TYPE_SGI_MGRAS "/texture",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_tex_needed,
    .pre_load = mgras_tex_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl_context, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS,
                             1, vmstate_sgi_mgras_gl_context_tex,
                             SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/* TRAM's bytes, sent once a load or a restore has written a block. */
static bool mgras_tram_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < ARRAY_SIZE(s->tram_written); i++) {
        if (s->tram_written[i]) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_tram = {
    .name = TYPE_SGI_MGRAS "/tram",
    .version_id = 2,
    .minimum_version_id = 2,
    .needed = mgras_tram_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(tram_written, SGIMgrasState,
                             SGI_MGRAS_TRAM_SELECTS *
                             SGI_MGRAS_TRAM_BLOCKS / 32),
        VMSTATE_BUFFER_POINTER_UNSAFE(tram, SGIMgrasState, 0,
                                      SGI_MGRAS_TRAM_IMAGE_BYTES),
        VMSTATE_END_OF_LIST()
    }
};
