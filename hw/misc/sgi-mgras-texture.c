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
 *   the format's bytes a texel. Texels of a level no transfer has reached
 *   hold whatever TRAM held there, 0 after a reset (the model's choice;
 *   the board's TRAM contents at reset are unknown).
 *   The pixel program's unpack block is read for the image's shape: its
 *   routine word gives the components (0x4064 four, 0x40f2 three, 0x417e
 *   two, 0x420c one) and its scale word the bytes a component (1/255 one,
 *   1/65535 two), established as words beside matching byte counts in
 *   perfly's, atlantis's, buttonfly's and blast's loads, the meanings
 *   inferred. An image of the kept layout's components, or of three into
 *   RGBA (alpha 1, as OpenGL 1.2.1 3.6.4 converts an RGB group), is
 *   taken; two-byte components are read big-endian and reduced to the
 *   kept depth (see "Depth"). Anything else is logged and nothing is
 *   stored. The rest of the program is not read, approximated: only the
 *   four- and three-unsigned-byte ones were traced, so others are taken
 *   as the identity, as routine 0x511a is at the default pixel transfer
 *   state, as the image path takes 0x49d0. TL_MODE is state that
 *   persists across loads, since the client skips it when reloading the
 *   image it loaded last. An unpaired 0x9c00, which the client's
 *   untraced wide-level branch sends, and a 0x511a transfer outside a
 *   bracket are logged and not rejected. An inline image or a transfer
 *   of another byte count is logged and nothing is stored. The HQ
 *   pixel-format word's GL_ABGR_EXT bit (0xe8d for bytes, 0xe8e for
 *   two-byte components) on an image of four components kept as RGBA
 *   reverses each texel's components; the same word reorders the image
 *   path's draw (inferred to apply here too). On two components it was
 *   never traced and is logged, nothing stored.
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
 * texture-select groups and two-component GL_ABGR_EXT loads.
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
#include "qemu/log.h"
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
 * TRAM save and restore, established from the disassembly of the kernel's
 * texture I/O member of mgras.a (SHA-256 5cd14335...) and from the traced
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
 * (from its disassembly); it was not traced, and the load routine and the
 * whole-TRAM shape are what the model keys on to keep it out. That a
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
/* TL_SPEC's whole-TRAM shape: level 0, 64 wide, 8,192 tall. */
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
#define MGRAS_TEXMODE2_UNDRAWN   0x01800610u
#define MGRAS_TEXMODE2_MIP       0x00080020u
#define MGRAS_TEXMODE2_MIP_BETWEEN 0x00000040u
#define MGRAS_TEXMODE2_MAG_LINEAR 0x00000080u
#define MGRAS_TEXMODE2_MIN_LINEAR 0x00000100u
#define MGRAS_TEXMODE2_WRAP_S    0x00000800u
#define MGRAS_TEXMODE2_WRAP_T    0x00001000u
#define MGRAS_TEXMODE2_CLAMP     0x00004000u

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
 * Where a level of the kept format mode, 1 << log2w by 1 << log2h, starts
 * in TRAM, in bits, under base and TXMIPMAP word: false, with nothing
 * set, when it would run past TRAM's end.
 */
static bool mgras_tram_place(uint32_t base, uint32_t word, uint32_t log2w,
                             uint32_t log2h, uint32_t mode, uint64_t *bit)
{
    uint64_t bits = ((uint64_t)mgras_tex_slot_bits(mode) << log2w) << log2h;
    uint64_t block = (uint64_t)(base & 0xff) + (word & 0xff);
    uint64_t start = block * MGRAS_TRAM_BLOCK_BITS;

    if (bits <= MGRAS_TRAM_BLOCK_BITS / 2) {
        start += MGRAS_TRAM_BLOCK_BITS - 2 * bits;
    }
    if (start + bits > (uint64_t)SGI_MGRAS_TRAM_BYTES * 8) {
        return false;
    }
    *bit = start;
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

void sgi_mgras_texture_reset(SGIMgrasTexture *t)
{
    mgras_tex_staging_free(t);
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
 * load routine under TL_SPEC's whole-TRAM shape it is the client's TRAM
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
        return true;
    case MGRAS_TEX_LOAD_CLOSE:
        if (!t->bracket) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture load closed "
                          "without an opening 0x9b00\n");
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

/* Read rows of TRAM from first_row back to host memory. */
static void mgras_tram_save(SGIMgrasState *s, uint32_t kick_byte)
{
    uint64_t first = (uint64_t)s->tram_read_block * MGRAS_TRAM_BLOCK_ROWS;
    uint64_t bytes = sgi_mgras_raster_dma_bytes(s);
    uint32_t k = mgras_tram_word_bytes(bytes, s->tram_rows);
    uint64_t words = (uint64_t)s->tram_rows * MGRAS_TRAM_ROW_WORDS;
    g_autofree uint8_t *buf = NULL;

    if (first + s->tram_rows > MGRAS_TRAM_ROWS) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: TRAM save of %u rows from "
                      "block %u runs past TRAM; nothing moved\n",
                      s->tram_rows, s->tram_read_block);
        return;
    }
    buf = g_malloc(bytes);
    for (uint64_t i = 0; i < words; i++) {
        memcpy(buf + i * k, s->tram + (first * MGRAS_TRAM_ROW_WORDS + i) *
               MGRAS_TRAM_WORD_BYTES, k);
    }
    if (sgi_mgras_raster_dma_copy(s, kick_byte, buf, bytes, true)) {
        trace_sgi_mgras_tram_transfer("save", s->tram_read_block, s->tram_rows,
                                      k);
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
    for (uint64_t i = 0; i < words; i++) {
        memcpy(s->tram + (first * MGRAS_TRAM_ROW_WORDS + i) *
               MGRAS_TRAM_WORD_BYTES, buf + i * k, k);
    }
    mgras_tram_mark(s, first * MGRAS_TRAM_ROW_WORDS * 32, words * 32);
    trace_sgi_mgras_tram_transfer(client ? "client restore" : "restore",
                                  s->tram_load_block, rows, k);
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
        memset(s->tram, 0, SGI_MGRAS_TRAM_BYTES);
    }
    memset(s->tram_written, 0, sizeof(s->tram_written));
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
    bool abgr;
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
    if (!mgras_tram_place(t->base, s->ge_param[MGRAS_TEX_GE_BLOCK],
                          MGRAS_TEX_SPEC_LOG2W(spec),
                          MGRAS_TEX_SPEC_LOG2H(spec), mode, &bit)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: texture level of %u by "
                      "%u at base %u, TXMIPMAP word 0x%x runs past TRAM's "
                      "end; nothing stored\n", w, h, t->base,
                      s->ge_param[MGRAS_TEX_GE_BLOCK]);
        return true;
    }
    t->load = 1;
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
                    (t->load_size >> 16) * mgras_tex_load_texel(t);

    *take = false;
    if (!t->load) {
        return false;
    }
    /*
     * ivview's levels came as one row and perfly's strips a row of the
     * strip at a time; rows are taken end to end, and the whole
     * rectangle's bytes must arrive: width times height times the kept
     * format's bytes a texel, which also refuses row padding.
     */
    if ((uint64_t)rows * bpr != host) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: DMA of %u rows of %u "
                      "bytes for a texture level of %" PRIu64 " bytes; "
                      "nothing stored\n", rows, bpr, host);
        t->load = 0;
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
    bool stored = false;
    uint64_t start;

    if (!mgras_tram_place(t->load_base, t->load_block, log2w, log2h, mode,
                          &start)) {
        /* load_start placed it; only a migrated load can get here. */
        goto done;
    }
    /*
     * The model drops the border: a texel outside the level, in the
     * one-texel ring around it that a bordered image carries, is read
     * and not stored. Approximated: that the board keeps those texels
     * (in the block the load's TXBORDER word names, which is inferred
     * from the word) and how a clamped read uses them are unknown, so
     * a draw samples the border colour register, not the image's border.
     */
    for (uint32_t row = 0; row < rh; row++, in += rw * src * size) {
        int64_t ty = (int64_t)(int32_t)t->load_y + row;

        if (ty < 0 || ty >= h) {
            continue;
        }
        for (uint32_t i = 0; i < rw * comps; i++) {
            /* An ABGR image's components come in reverse order. */
            uint32_t k = t->load_abgr ? comps - 1 - i % comps : i % comps;
            uint32_t n = i / comps * src + k;
            int64_t tx = (int64_t)(int32_t)t->load_x + i / comps;
            uint64_t bit;
            uint32_t v;

            if (tx < 0 || tx >= w) {
                continue;
            }
            bit = start + (((uint64_t)ty << log2w) + tx) * slot +
                  i % comps * depth;
            if (k >= src) {
                /* An RGB image's alpha is 1 (OpenGL 1.2.1, 3.6.4). */
                v = (1u << depth) - 1;
            } else if (size == 1) {
                v = depth == 4 ? in[n] >> 4 : in[n];
            } else {
                /* Two-byte components: see sgi_mgras_texture_load_start. */
                uint32_t c = lduw_be_p(&in[2 * n]);

                v = depth == 12 ? c >> 4 :
                    depth == 8 ? (c + 128) / 257 : (c + 128) / 257 >> 4;
            }
            mgras_tram_put(s->tram, bit, depth, v);
            stored = true;
        }
    }
    if (!stored) {
        goto done;
    }
    mgras_tram_mark(s, start, ((uint64_t)slot << log2w) << log2h);
    trace_sgi_mgras_texture_level(t->load_base, t->load_block, level,
                                  1u << log2w, 1u << log2h, mode);
done:
    mgras_tex_staging_free(t);
    t->load = 0;
}

bool sgi_mgras_texture_row(SGIMgrasState *s, const uint8_t *bytes,
                           uint32_t n)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t take;

    if (!t->staging) {
        return false;
    }
    take = MIN(n, t->staging_bytes - t->staged);
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
}

/*
 * A stored texel as the sampler's canonical RRGGBBAA, a 4-bit component
 * replicated to 8 bits and a 12-bit one scaled to 8. A one-component
 * texel is set in both the red and alpha bytes and a two-component one's
 * luminance in red, since the class that reads it is the draw's and the
 * sampler takes the byte it needs.
 */
static bool mgras_tex_read_texel(void *opaque, uint32_t level, uint32_t x,
                                 uint32_t y, uint32_t *rgba)
{
    const SGIMgrasTexView *l = opaque;
    uint32_t comps = mgras_tex_components(l->mode);
    uint32_t depth = mgras_tex_depth_bits(l->mode);
    uint64_t bit = l->bit + (((uint64_t)y << l->log2w) + x) *
                   mgras_tex_slot_bits(l->mode);
    uint32_t c[4];

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

/* The sampler's base format for a layout and the class the draw names. */
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
    if (!mgras_tram_place(t->base, t->txmipmap[n], log2w, log2h, mode,
                          &view->bit) ||
        !mgras_tram_written(s, view->bit / MGRAS_TRAM_BLOCK_BITS)) {
        return false;
    }
    view->tram = s->tram;
    view->log2w = log2w;
    view->log2h = log2h;
    view->mode = mode;
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
        t->sampler_levels[n++] = (SGIGLTexLevel) {
            .level = level,
            .width = 1u << e->log2w,
            .height = 1u << e->log2h,
            .base_format = format,
            .internal_format = format,
            .read_texel = mgras_tex_read_texel,
            .opaque = e,
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

const SGIGLTexImage *sgi_mgras_texture_image(SGIMgrasState *s,
                                             SGIGLTexImage *image)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t size = t->txsize & 0xff, format;
    uint32_t cls = MGRAS_TEXMODE1_CLASS(t->texmode1);
    uint32_t layout = MGRAS_TEXMODE1_LAYOUT(t->texmode1);
    uint32_t depth = (t->texmode2 >> 2) & 3;
    /* The kept format the draw names, in TL_MODE's fields. */
    uint32_t mode = layout << 1 | depth << 5;
    uint32_t mip = t->texmode2 & MGRAS_TEXMODE2_MIP;
    uint32_t min_filter;

    if (!mgras_tex_present(s) ||
        !s->gl_context[s->gl_current].gl->cur.texture_2d) {
        return NULL;
    }
    /*
     * TXSIZE's two copies of the base size must agree, as every traced
     * two-dimensional draw sent them. The class is the draw's (TEXMODE1
     * 11:9) and must suit the layout. Class 1, alpha, comes with
     * environment code 3 from the client, whatever glTexEnv set; the
     * model reads the alpha class as its function for every code.
     */
    if ((t->txsize & 0xffff) != (size << 8 | size) ||
        depth > MGRAS_TEX_DEPTH_12 ||
        (t->texmode1 & (MGRAS_TEXMODE1_SELECT | MGRAS_TEXMODE1_TABLE)) ||
        !mgras_tex_environment(s, cls) ||
        !mgras_tex_format(layout, cls, &format) ||
        (t->texmode2 & MGRAS_TEXMODE2_UNDRAWN) ||
        (mip && mip != MGRAS_TEXMODE2_MIP) ||
        t->t_offset) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture state TEXMODE1 0x%x "
                      "TEXMODE2 0x%x TXSIZE 0x%x offset 0x%x is not "
                      "modelled; drawn untextured\n", t->texmode1,
                      t->texmode2, t->txsize, t->t_offset);
        return NULL;
    }
    if (!mgras_tex_draw_level(s, 0, mode, &t->views[0])) {
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
    *image = (SGIGLTexImage) {
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
    };
    return image;
}

bool sgi_mgras_texture_valid(const SGIMgrasTexture *t)
{
    uint64_t bit;

    if (t->on > 1 || t->bracket > 1 || t->load > 1 ||
        t->txaddr >= SGI_MGRAS_TEX_TABLE || t->tl_addr >= SGI_MGRAS_TEX_TABLE ||
        t->staging) {
        return false;
    }
    if (t->load) {
        uint32_t w = 1u << MGRAS_TEX_SPEC_LOG2W(t->load_spec);
        uint32_t h = 1u << MGRAS_TEX_SPEC_LOG2H(t->load_spec);
        uint32_t rw = t->load_size & 0xffff, rh = t->load_size >> 16;

        if (MGRAS_TEX_DEPTH(t->load_mode) > MGRAS_TEX_DEPTH_12 ||
            !mgras_tram_place(t->load_base, t->load_block,
                              MGRAS_TEX_SPEC_LOG2W(t->load_spec),
                              MGRAS_TEX_SPEC_LOG2H(t->load_spec),
                              t->load_mode, &bit) ||
            !mgras_tex_rect_fits(t->load_x, t->load_y, rw, rh, w, h) ||
            t->load_abgr > 1 || t->load_comps < 1 || t->load_comps > 4 ||
            t->load_csize < 1 || t->load_csize > 2 ||
            !mgras_tex_image_fits(t->load_mode, t->load_comps,
                                  t->load_csize) ||
            (t->load_abgr && t->load_comps != 4)) {
            return false;
        }
    }
    return true;
}

static const VMStateDescription vmstate_sgi_mgras_tex = {
    .name = TYPE_SGI_MGRAS "/texture-state",
    .version_id = 8,
    .minimum_version_id = 8,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(texmode1, SGIMgrasTexture),
        VMSTATE_UINT32(texmode2, SGIMgrasTexture),
        VMSTATE_UINT32(txsize, SGIMgrasTexture),
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
        VMSTATE_UINT32(load, SGIMgrasTexture),
        VMSTATE_UINT32(load_base, SGIMgrasTexture),
        VMSTATE_UINT32(load_block, SGIMgrasTexture),
        VMSTATE_UINT32(load_spec, SGIMgrasTexture),
        VMSTATE_UINT32(load_mode, SGIMgrasTexture),
        VMSTATE_UINT32(load_x, SGIMgrasTexture),
        VMSTATE_UINT32(load_y, SGIMgrasTexture),
        VMSTATE_UINT32(load_size, SGIMgrasTexture),
        VMSTATE_UINT32(load_comps, SGIMgrasTexture),
        VMSTATE_UINT32(load_csize, SGIMgrasTexture),
        VMSTATE_UINT32(load_abgr, SGIMgrasTexture),
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
                   t->txclampsize | t->txenv[0] | t->txenv[1] |
                   t->txbcolor[0] | t->txbcolor[1] |
                   t->txaddr | t->tl_mode | t->tl_spec | t->tl_wbuffer |
                   t->texrbuffer | t->tl_addr | t->base | t->t_offset | t->on |
                   t->bracket | t->load | t->load_base | t->load_block |
                   t->load_spec | t->load_mode | t->load_x | t->load_y |
                   t->load_size | t->load_comps | t->load_csize |
                   t->load_abgr;

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
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_tram_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(tram_written, SGIMgrasState,
                             SGI_MGRAS_TRAM_BLOCKS / 32),
        VMSTATE_BUFFER_POINTER_UNSAFE(tram, SGIMgrasState, 0,
                                      SGI_MGRAS_TRAM_BYTES),
        VMSTATE_END_OF_LIST()
    }
};
