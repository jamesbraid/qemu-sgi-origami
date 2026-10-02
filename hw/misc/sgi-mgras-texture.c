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
 *   before 32 packets in mgrTM_Init). The shadow address is the GE's and
 *   is not used. Any other record is logged, as the whole packet was
 *   before. Registers are kept as written, without the diagnostic's
 *   width masks.
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
 *   refuses one that does not lie inside the level. Words 0xe36 and 0xe37,
 *   the rectangle's size again where the image path loads inclusive
 *   corners, are not read, and neither are the bytes a pixel in XFRMODE,
 *   0xc00080 on these four-byte levels. The kept format is TL_MODE's
 *   layout (one component, luminance alpha, RGB, RGBA) and depth (4, 8 or
 *   12 bits), the host bytes the components in order, a byte each at 4
 *   and 8 bits and a big-endian halfword at 12, the bottom row (t = 0)
 *   first; established for RGBA unsigned bytes, inferred for the rest.
 *   The transfer must be exactly the rectangle's width times height times
 *   the format's bytes a texel. Texels of a level no transfer has reached
 *   are 0 (the model's choice; the board's TRAM would hold what it held).
 *   The pixel program is read for one case: the three-unsigned-byte RGB
 *   program the image path draws GL_RGB with, which perfly's client
 *   loaded for the CH-53's RGB body texture kept as RGBA4 (its DMA rows
 *   192 bytes for 64 texels). Its image is three bytes a texel into a
 *   level kept as RGB or RGBA at 4 or 8 bits, alpha 1 where the level
 *   keeps one, as OpenGL 1.2.1 3.6.4 converts an RGB group; inferred, as
 *   the GE program is not read. Any other program is taken as the kept
 *   layout's own bytes, approximated: only the four-unsigned-byte one
 *   was traced, so the others are taken as the identity, as routine
 *   0x511a is at the default pixel transfer state, as the image path
 *   takes 0x49d0. TL_MODE is state that persists
 *   across loads, since the client skips it when reloading the image it
 *   loaded last. An unpaired 0x9c00, which the client's untraced wide-level
 *   branch sends, and a 0x511a transfer outside a bracket are logged and
 *   not rejected. An inline image, GL_ABGR_EXT or a transfer of another
 *   byte count is logged and nothing is stored.
 *
 * - Storage. Approximated: a level is stored per GL context keyed by the
 *   base block the kernel's 0xeb04 names, the TXMIPMAP word the load
 *   names and TL_SPEC's level number, not at a TRAM byte address. The
 *   load's GE words at 0xe3d carry the level's TXMIPMAP word, the first
 *   block in 7:0 and bit 8, which perfly set on alternate levels of a
 *   shared block (placement inside it, meaning unknown), and the draw
 *   sends the same word for the level through the TXMIPMAP table:
 *   ivview's three levels loaded 0, 0x100 and 0 and drew 0, 0x100 and 0,
 *   and perfly's three textures loaded and drew 0, 4 and 0xc for level 0 (established
 *   as words; that the board reads the level from the block it was loaded
 *   to is inferred). The draw finds level n by TXMIPMAP entry n, the
 *   client writing the table from the first level, 0 in every trace. A
 *   mipmapped texture puts several small levels in one block under one
 *   word, so the level number stays in the key. Where a level sits inside
 *   its block is unknown, and the model keeps no TRAM byte image: TRAM
 *   save and restore through the kernel's DMA paths are not modelled, and
 *   a load does not evict a level of another word that overlapped it in
 *   TRAM; the other stays until its own word is loaded again. A context
 *   keeps at most one TRAM's 4 MiB of texels and the board four TRAMs'.
 *   MXI and EMXI have a TE1 and TRAM on each of two REs. The model
 *   keeps one store per context for both, approximated: each path
 *   holding the full texture is inferred, so a load, a base write and a
 *   draw reach the rows of both REs, and the capacity stays one TRAM's
 *   4 MiB, not 8. The software cannot tell the difference; only the
 *   diagnostics' per-RE TRAM tests could. Save and restore are not
 *   modelled there either.
 *   A context opener discards that context's levels and texture state,
 *   approximated: whether the board's TRAM and TE1 survive a context
 *   switch or an opener is unknown, so a client that relied on texels
 *   outliving its opener would lose them here.
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
 *   being 8-bit.
 *
 * - Bind and draw. 0x0b00 with TEXMODE1 bit 0 set turns texturing on,
 *   0x0c00 or bit 0 clear off. A draw is textured from level 0 under the
 *   context's base when TXSIZE's base size is that level's, the level's
 *   layout and depth are TEXMODE1 bits 4:3 and TEXMODE2 bits 3:2, the
 *   environment (TEXMODE1 bits 2:1, see mgras_tex_environment) is
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
 * read (TEXMODE2 bit 4 and TL_MODE bit 8), bordered textures (a bordered
 * image is two texels wider and taller than its level, so its load does
 * not lie inside the level and is not stored, and its draw has no level
 * of its own: it is untextured unless an older level was loaded under the
 * same key, which a refused load leaves drawable, as every refusal here
 * does; inferred, as no bordered load was traced), detail,
 * the texture colour table, one-dimensional and volume textures,
 * texture-select groups and GL_ABGR_EXT loads.
 *
 * The discriminating tests are gl-texture-load-draws-the-level,
 * gl-texture-size-from-tl-spec, gl-texture-tl-mode-persists,
 * gl-texture-rgba4-truncates, gl-texture-unsupported-draws-untextured,
 * gl-texture-textureless-board-ignores, gl-texture-migration, the replay
 * fixture gl-texture-rgba4-quad, and for the formats, environments, wraps
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

/* One TRAM path's 4 MiB, the most one context keeps; four for the board. */
#define MGRAS_TEX_CONTEXT_BYTES  (4 * MiB)
#define MGRAS_TEX_BOARD_BYTES    (4 * MGRAS_TEX_CONTEXT_BYTES)

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

/*
 * Bytes a texel in the host image and in the model's storage, a byte a
 * component at 4 and 8 bits and a big-endian halfword at 12.
 */
static uint32_t mgras_tex_host_texel(uint32_t mode)
{
    return mgras_tex_components(mode) *
           (MGRAS_TEX_DEPTH(mode) == MGRAS_TEX_DEPTH_12 ? 2 : 1);
}

static uint64_t mgras_tex_level_bytes(uint32_t spec, uint32_t mode)
{
    return ((1ull << MGRAS_TEX_SPEC_LOG2W(spec)) <<
            MGRAS_TEX_SPEC_LOG2H(spec)) * mgras_tex_host_texel(mode);
}

/* Host bytes a texel of the image the pending load carries. */
static uint32_t mgras_tex_load_texel(const SGIMgrasTexture *t)
{
    return t->load_host ? t->load_host : mgras_tex_host_texel(t->load_mode);
}

static void mgras_tex_level_free(SGIMgrasTexLevel *l)
{
    g_free(l->texels);
    memset(l, 0, sizeof(*l));
}

static void mgras_tex_staging_free(SGIMgrasTexture *t)
{
    g_free(t->staging);
    t->staging = NULL;
    t->staged = t->staging_bytes = 0;
}

void sgi_mgras_texture_reset(SGIMgrasTexture *t)
{
    for (int i = 0; i < SGI_MGRAS_TEX_LEVELS; i++) {
        mgras_tex_level_free(&t->levels[i]);
    }
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

/*
 * The 0xec10 table writes (shadow index, shadow table, register, value)
 * into TXMIPMAP and TXBORDER; the GL path keeps the detail-scale table's
 * defaults, and the load-side tables are logged.
 */
static bool mgras_tex_table(SGIMgrasState *s, const uint32_t *payload)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t reg = payload[2];

    if (reg != MGRAS_TEX_TXMIPMAP && reg != MGRAS_TEX_TXBORDER) {
        return false;
    }
    if (reg == MGRAS_TEX_TXMIPMAP) {
        t->txmipmap[t->txaddr] = payload[3];
    } else {
        t->txborder[t->txaddr] = payload[3];
    }
    t->txaddr = (t->txaddr + 1) % SGI_MGRAS_TEX_TABLE;
    return true;
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
    uint32_t host;

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
     * Only two pixel programs were traced: four unsigned bytes and, for
     * GL_RGB, three. The program itself is not decoded; the three-byte
     * one is told apart below by sgi_mgras_gl_pixels_rgb8(). Approximated:
     * any other program is taken as the kept layout's own bytes, so one
     * that reorders or converts components would be taken as the
     * identity. The load is keyed on routine 0x511a and sized by the
     * rectangle the size word names. The one other thing checked is the HQ
     * pixel format's GL_ABGR_EXT bit, which swaps components and is not
     * modelled.
     */
    if (s->dma_desc_30000[0] & MGRAS_TEX_HQ_ABGR) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture load with HQ pixel "
                      "format 0x%x (GL_ABGR_EXT) is not modelled; nothing "
                      "stored\n", s->dma_desc_30000[0]);
        return true;
    }
    /*
     * The three-byte RGB program (GL_RGB, as perfly's CH-53 body texture
     * came) feeds a level kept as RGB or RGBA at 4 or 8 bits; any other
     * program is taken as the kept layout's own bytes, as above.
     */
    host = 0;
    if (sgi_mgras_gl_pixels_rgb8(s)) {
        if (MGRAS_TEX_LAYOUT(mode) < MGRAS_TEX_LAYOUT_RGB ||
            MGRAS_TEX_DEPTH(mode) >= MGRAS_TEX_DEPTH_12) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture load of an RGB "
                          "image kept as TL_MODE 0x%x is not modelled; "
                          "nothing stored\n", mode);
            return true;
        }
        host = 3;
    }
    if (!rw || !rh || rw > w || rh > h || x > w - rw || y > h - rh) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture load of %u by %u at "
                      "%u, %u (size word 0x%x) is not inside TL_SPEC 0x%x's "
                      "%u by %u; nothing stored\n", rw, rh, x, y, size, spec,
                      w, h);
        return true;
    }
    if (MGRAS_TEX_DEPTH(mode) > MGRAS_TEX_DEPTH_12) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture kept as TL_MODE 0x%x "
                      "(layout %u, depth code %u) is not modelled; nothing "
                      "stored\n", mode, MGRAS_TEX_LAYOUT(mode),
                      MGRAS_TEX_DEPTH(mode));
        return true;
    }
    if (mgras_tex_level_bytes(spec, mode) > MGRAS_TEX_CONTEXT_BYTES) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: texture level of %u by "
                      "%u is larger than TRAM; nothing stored\n", w, h);
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
    t->load_host = host;
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

static uint64_t mgras_tex_bytes_kept(const SGIMgrasTexture *t,
                                     const SGIMgrasTexLevel *except)
{
    uint64_t sum = 0;

    for (int i = 0; i < SGI_MGRAS_TEX_LEVELS; i++) {
        if (&t->levels[i] != except) {
            sum += t->levels[i].bytes;
        }
    }
    return sum;
}

/*
 * Keep the gathered host image as the load's rectangle of its level: into
 * the level kept under the same key when its size and format are the
 * load's, so the strips of one level add up, and otherwise into a new
 * level whose other texels are 0.
 */
static void mgras_tex_commit(SGIMgrasState *s)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t level = MGRAS_TEX_SPEC_LEVEL(t->load_spec);
    uint32_t log2w = MGRAS_TEX_SPEC_LOG2W(t->load_spec);
    uint32_t log2h = MGRAS_TEX_SPEC_LOG2H(t->load_spec);
    uint32_t bytes = mgras_tex_level_bytes(t->load_spec, t->load_mode);
    uint32_t texel = mgras_tex_host_texel(t->load_mode);
    uint32_t comps = mgras_tex_components(t->load_mode);
    /* The image's components a texel, and bytes a component. */
    uint32_t src = t->load_host ? t->load_host : comps;
    uint32_t size = texel / comps;
    uint32_t rw = t->load_size & 0xffff, rh = t->load_size >> 16;
    uint64_t board = 0;
    SGIMgrasTexLevel *l = NULL, *free = NULL;
    const uint8_t *in = t->staging;

    for (int i = 0; i < SGI_MGRAS_TEX_LEVELS; i++) {
        SGIMgrasTexLevel *e = &t->levels[i];

        if (e->used && e->base == t->load_base &&
            e->block == t->load_block && e->level == level) {
            l = e;
        } else if (!e->used && !free) {
            free = e;
        }
    }
    if (!l || l->log2w != log2w || l->log2h != log2h ||
        l->mode != t->load_mode) {
        l = l ? l : free;
        for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
            board += mgras_tex_bytes_kept(&s->gl_context[i].tex, l);
        }
        if (!l ||
            mgras_tex_bytes_kept(t, l) + bytes > MGRAS_TEX_CONTEXT_BYTES ||
            board + bytes > MGRAS_TEX_BOARD_BYTES) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: no room for texture level "
                          "%u of base %u; nothing stored\n", level,
                          t->load_base);
            goto done;
        }
        mgras_tex_level_free(l);
        *l = (SGIMgrasTexLevel) {
            .used = 1,
            .base = t->load_base,
            .block = t->load_block,
            .level = level,
            .log2w = log2w,
            .log2h = log2h,
            .mode = t->load_mode,
            .bytes = bytes,
            .texels = g_malloc0(bytes),
        };
    }
    for (uint32_t row = 0; row < rh; row++, in += rw * src * size) {
        uint8_t *out = l->texels +
                       (((t->load_y + row) << log2w) + t->load_x) * texel;

        for (uint32_t i = 0; i < rw * comps; i++) {
            uint32_t n = i / comps * src + i % comps;

            if (i % comps >= src) {
                /* An RGB image's alpha is 1 (OpenGL 1.2.1, 3.6.4). */
                out[i] = MGRAS_TEX_DEPTH(t->load_mode) == MGRAS_TEX_DEPTH_4 ?
                         0xf : 0xff;
                continue;
            }
            switch (MGRAS_TEX_DEPTH(t->load_mode)) {
            case MGRAS_TEX_DEPTH_4:
                /* Truncation to 4 bits, the model's choice, as above. */
                out[i] = in[n] >> 4;
                break;
            case MGRAS_TEX_DEPTH_8:
                out[i] = in[n];
                break;
            default:
                /* The top 12 bits of the halfword, kept right-aligned. */
                stw_be_p(&out[2 * i], lduw_be_p(&in[2 * n]) >> 4);
                break;
            }
        }
    }
    trace_sgi_mgras_texture_level(t->load_base, t->load_block, level,
                                  1u << l->log2w, 1u << l->log2h,
                                  t->load_mode);
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
    const SGIMgrasTexLevel *l = opaque;
    uint32_t comps = mgras_tex_components(l->mode);
    uint32_t i = ((y << l->log2w) + x) * comps;
    uint32_t c[4];

    for (uint32_t k = 0; k < comps; k++) {
        switch (MGRAS_TEX_DEPTH(l->mode)) {
        case MGRAS_TEX_DEPTH_4:
            c[k] = l->texels[i + k] * 0x11;
            break;
        case MGRAS_TEX_DEPTH_8:
            c[k] = l->texels[i + k];
            break;
        default:
            /*
             * The sampler's texels are 8-bit, so a 12-bit component is
             * rounded to the nearest of 255 (approximated): precision
             * beyond the 8-bit store is lost before filtering.
             */
            c[k] = (lduw_be_p(&l->texels[2 * (i + k)]) * 255 + 2047) / 4095;
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

static SGIMgrasTexLevel *mgras_tex_find(SGIMgrasTexture *t, uint32_t base,
                                        uint32_t block, uint32_t level)
{
    for (int i = 0; i < SGI_MGRAS_TEX_LEVELS; i++) {
        if (t->levels[i].used && t->levels[i].base == base &&
            t->levels[i].block == block && t->levels[i].level == level) {
            return &t->levels[i];
        }
    }
    return NULL;
}

/* Level n of the draw: TXMIPMAP entry n under the context's base. */
static SGIMgrasTexLevel *mgras_tex_draw_level(SGIMgrasTexture *t,
                                              uint32_t level)
{
    return mgras_tex_find(t, t->base, t->txmipmap[level], level);
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
static void mgras_tex_pyramid(SGIMgrasTexture *t, const SGIMgrasTexLevel *l0,
                              uint32_t format, bool mip)
{
    uint32_t n = 0, last = mip ? MGRAS_TXLOD_LAST(t->txlod) : 0;
    double min_lod = mip ? MIN(MGRAS_TXLOD_MIN(t->txlod) / 64.0, last) : 0;

    for (uint32_t level = 0; level < SGI_MGRAS_TEX_LEVELS; level++) {
        SGIMgrasTexLevel *e = mgras_tex_draw_level(t, level);

        if (!e || e->mode != l0->mode) {
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
    SGIMgrasTexLevel *l;
    uint32_t size, format, cls = MGRAS_TEXMODE1_CLASS(t->texmode1);
    uint32_t mip = t->texmode2 & MGRAS_TEXMODE2_MIP;
    uint32_t min_filter;

    if (!mgras_tex_present(s) ||
        !s->gl_context[s->gl_current].gl->cur.texture_2d) {
        return NULL;
    }
    l = mgras_tex_draw_level(t, 0);
    if (!l) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: textured draw with no level 0 "
                      "loaded under base %u at TXMIPMAP 0x%x\n", t->base,
                      t->txmipmap[0]);
        return NULL;
    }
    size = l->log2h << 4 | l->log2w;
    /*
     * The class is the draw's (TEXMODE1 11:9) and must suit the level's
     * layout. Class 1, alpha, comes with environment code 3 from the
     * client, whatever glTexEnv set; the model reads the alpha class as
     * its function for every code.
     */
    if ((t->txsize & 0xffff) != (size << 8 | size) ||
        (t->texmode1 & (MGRAS_TEXMODE1_SELECT | MGRAS_TEXMODE1_TABLE)) ||
        !mgras_tex_environment(s, cls) ||
        MGRAS_TEXMODE1_LAYOUT(t->texmode1) != MGRAS_TEX_LAYOUT(l->mode) ||
        !mgras_tex_format(MGRAS_TEX_LAYOUT(l->mode), cls, &format) ||
        (t->texmode2 & MGRAS_TEXMODE2_UNDRAWN) ||
        (mip && mip != MGRAS_TEXMODE2_MIP) ||
        ((t->texmode2 >> 2) & 3) != MGRAS_TEX_DEPTH(l->mode) ||
        t->t_offset) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture state TEXMODE1 0x%x "
                      "TEXMODE2 0x%x TXSIZE 0x%x offset 0x%x with a %u by %u "
                      "level kept as 0x%x is not modelled; drawn "
                      "untextured\n", t->texmode1, t->texmode2, t->txsize,
                      t->t_offset, 1u << l->log2w, 1u << l->log2h, l->mode);
        return NULL;
    }
    mgras_tex_pyramid(t, l, format, mip);
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

bool sgi_mgras_texture_board_valid(const SGIMgrasState *s)
{
    uint64_t board = 0;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        board += mgras_tex_bytes_kept(&s->gl_context[i].tex, NULL);
    }
    return board <= MGRAS_TEX_BOARD_BYTES;
}

bool sgi_mgras_texture_valid(const SGIMgrasTexture *t)
{
    if (t->on > 1 || t->bracket > 1 || t->load > 1 ||
        t->txaddr >= SGI_MGRAS_TEX_TABLE || t->staging) {
        return false;
    }
    if (t->load) {
        uint32_t w = 1u << MGRAS_TEX_SPEC_LOG2W(t->load_spec);
        uint32_t h = 1u << MGRAS_TEX_SPEC_LOG2H(t->load_spec);
        uint32_t rw = t->load_size & 0xffff, rh = t->load_size >> 16;

        if (MGRAS_TEX_DEPTH(t->load_mode) > MGRAS_TEX_DEPTH_12 ||
            mgras_tex_level_bytes(t->load_spec, t->load_mode) >
            MGRAS_TEX_CONTEXT_BYTES ||
            !rw || !rh || rw > w || rh > h ||
            t->load_x > w - rw || t->load_y > h - rh ||
            (t->load_host &&
             (t->load_host != 3 ||
              MGRAS_TEX_LAYOUT(t->load_mode) < MGRAS_TEX_LAYOUT_RGB ||
              MGRAS_TEX_DEPTH(t->load_mode) >= MGRAS_TEX_DEPTH_12))) {
            return false;
        }
    }
    for (int i = 0; i < SGI_MGRAS_TEX_LEVELS; i++) {
        const SGIMgrasTexLevel *l = &t->levels[i];
        uint32_t spec = l->log2h << 8 | l->log2w << 4;

        if (!l->used) {
            if (l->bytes || l->texels) {
                return false;
            }
            continue;
        }
        for (int j = 0; j < i; j++) {
            const SGIMgrasTexLevel *o = &t->levels[j];

            if (o->used && l->used && o->base == l->base &&
                o->block == l->block && o->level == l->level) {
                return false;
            }
        }
        if (l->used != 1 || l->level > 0xf || l->log2w > 0xf ||
            l->log2h > 0xf || !l->texels ||
            MGRAS_TEX_DEPTH(l->mode) > MGRAS_TEX_DEPTH_12 ||
            l->bytes != mgras_tex_level_bytes(spec, l->mode)) {
            return false;
        }
    }
    return mgras_tex_bytes_kept(t, NULL) <= MGRAS_TEX_CONTEXT_BYTES;
}

/* A level's texels are allocated only up to a context's TRAM. */
static bool mgras_tex_level_bytes_ok(void *opaque, int version_id)
{
    SGIMgrasTexLevel *l = opaque;

    return l->bytes <= MGRAS_TEX_CONTEXT_BYTES;
}

static const VMStateDescription vmstate_sgi_mgras_tex_level = {
    .name = TYPE_SGI_MGRAS "/texture-level",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(used, SGIMgrasTexLevel),
        VMSTATE_UINT32(base, SGIMgrasTexLevel),
        VMSTATE_UINT32(block, SGIMgrasTexLevel),
        VMSTATE_UINT32(level, SGIMgrasTexLevel),
        VMSTATE_UINT32(log2w, SGIMgrasTexLevel),
        VMSTATE_UINT32(log2h, SGIMgrasTexLevel),
        VMSTATE_UINT32(mode, SGIMgrasTexLevel),
        VMSTATE_UINT32(bytes, SGIMgrasTexLevel),
        VMSTATE_VBUFFER_ALLOC_UINT32(texels, SGIMgrasTexLevel, 1,
                                     mgras_tex_level_bytes_ok, bytes),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_tex = {
    .name = TYPE_SGI_MGRAS "/texture-state",
    .version_id = 5,
    .minimum_version_id = 5,
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
        VMSTATE_UINT32(load_host, SGIMgrasTexture),
        VMSTATE_STRUCT_ARRAY(levels, SGIMgrasTexture, SGI_MGRAS_TEX_LEVELS, 1,
                             vmstate_sgi_mgras_tex_level, SGIMgrasTexLevel),
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
                   t->texrbuffer | t->base | t->t_offset | t->on |
                   t->bracket | t->load | t->load_base | t->load_block |
                   t->load_spec | t->load_mode | t->load_x | t->load_y |
                   t->load_size | t->load_host;

    for (int i = 0; i < SGI_MGRAS_TEX_TABLE; i++) {
        any |= t->txmipmap[i] | t->txborder[i];
    }
    for (int i = 0; i < SGI_MGRAS_TEX_LEVELS; i++) {
        any |= t->levels[i].used;
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

/* The stream allocates every level it carries; free what is here first. */
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
