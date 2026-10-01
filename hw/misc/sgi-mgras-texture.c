/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SI+T MGRAS texture: one TE1 engine and its TRAM, as the IRIX 6.5.30
 * OpenGL client drives them (docs/datasheets/mgras-texture.md in the
 * Origami repository, "Upload, bind and draw as traced on SI+T", and the
 * review record docs/reviews/mgras-texture-ingress-evidence-20260930.md).
 * Every word below is established as what that client and kernel sent
 * to the model, in run irix-20260930T224203-2e96c4 (ivview drawing one
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
 *   TL_SPEC and checks it against the size word, h << 16 | w, at GE
 *   parameter 0xe38; it does not read the destination corners, which are
 *   0, 0, w, h here where the image path's are inclusive, and it does not
 *   read the bytes a pixel from XFRMODE, 0xc00080 on these four-byte
 *   levels. The pixel program must be the one the client loads for
 *   GL_RGBA unsigned bytes; the host bytes are then R, G, B, A a texel,
 *   the bottom row (t = 0) first, established for that format and type
 *   only. Routine 0x511a is taken as the identity at the default pixel
 *   transfer state, as the image path takes 0x49d0: an approximation, as
 *   the GE program is not read. TL_MODE is state that persists across
 *   loads, since the client skips it when reloading the image it loaded
 *   last. An unpaired 0x9c00, which the client's untraced wide-level
 *   branch sends, and a 0x511a transfer outside a bracket are logged and
 *   not rejected. An inline image, another pixel program, another kept
 *   layout than RGBA, or a 12-bit depth is logged and nothing is stored.
 *
 * - Storage. Approximated: a level is stored per GL context keyed by the
 *   base block the kernel's 0xeb04 names and TL_SPEC's level number, not
 *   at a TRAM block. TXMIPMAP gives the three small levels of a traced
 *   mipmapped texture all block 0, so where a level sits in TRAM is
 *   unknown, and the model keeps no TRAM byte image: TRAM save and
 *   restore through the kernel's DMA paths are not modelled. Two textures
 *   of one context under one base replace each other's levels. A context
 *   keeps at most one TRAM's 4 MiB of texels and the board four TRAMs'.
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
 *   An 8-bit depth keeps the host byte.
 *
 * - Bind and draw. 0x0b00 with TEXMODE1 bit 0 set turns texturing on,
 *   0x0c00 or bit 0 clear off. A draw is textured from level 0 under the
 *   context's base when TXSIZE's base size is that level's, the level's
 *   layout and depth are TEXMODE1 bits 4:3 and TEXMODE2 bits 3:2, the
 *   environment (TEXMODE1 bits 2:1) is modulate, and nothing in TEXMODE2
 *   asks for linear or mipmapped filtering, detail, clamping, a volume or
 *   a one-dimensional texture: nearest sampling with repeat wrap, through
 *   the shared rasterizer's texture sampling. Anything else draws
 *   untextured, logged.
 *
 * The discriminating tests are gl-texture-load-draws-the-level,
 * gl-texture-size-from-tl-spec, gl-texture-tl-mode-persists,
 * gl-texture-rgba4-truncates, gl-texture-unsupported-draws-untextured,
 * gl-texture-textureless-board-ignores, gl-texture-migration and the
 * replay fixture gl-texture-rgba4-quad.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/sgi-mgras-texture.h"
#include "hw/misc/sgi-mgras-gl.h"
#include "trace.h"

/* Class-0 texture tokens (mgras-texture.md, "Class-0 texture tokens"). */
#define MGRAS_TEX_ON             0x00000b00
#define MGRAS_TEX_OFF            0x00000c00
#define MGRAS_TEX_T_OFFSET       0x00005f04
#define MGRAS_TEX_LOAD_OPEN      0x00009b00
#define MGRAS_TEX_LOAD_CLOSE     0x00009c00
#define MGRAS_TEX_BASE           0x0000eb04
#define MGRAS_TEX_RECORDS        0x00007c08
#define MGRAS_TEX_TABLE          0x0000ec10

/* Texture registers (mgras-texture.md, "Register map"). */
#define MGRAS_TEX_TEXMODE1       0x111
#define MGRAS_TEX_TEXMODE2       0x180
#define MGRAS_TEX_TXSIZE         0x181
#define MGRAS_TEX_TXLOD          0x183
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

/* TEXMODE1 fields. */
#define MGRAS_TEXMODE1_ENABLE    0x00000001u
#define MGRAS_TEXMODE1_ENV       0x00000006u
#define MGRAS_TEXMODE1_SELECT    0x00000180u
#define MGRAS_TEXMODE1_TABLE     0x0000f000u
/*
 * TEXMODE2 bits the model does not draw: 5 to 10 filtering, mipmap and
 * detail control, 11 to 14 the clamps, 19 mipmap and detail control, 23 a
 * volume and 24 a one-dimensional texture.
 */
#define MGRAS_TEXMODE2_UNDRAWN   0x01887fe0u

/* The pixel transfer's routine word in 0x8d20's payload, and its value. */
#define MGRAS_TEX_ROUTINE_WORD   6
#define MGRAS_TEX_ROUTINE_LOAD   0x511a
#define MGRAS_TEX_FLAGS_WORD     7
#define MGRAS_TEX_FLAGS_BY_DMA   0x80

/*
 * The GE pixel program the client loads for four unsigned bytes a pixel
 * (mgras-texture.md, "Upload": 0x1f44/0, 0x3e65/0xd91, 0x156a/0x1b4,
 * 0x352d/0xdf3, with 0x4064 in the unpack routine's second word), the
 * one the image path also knows, at GE parameters 0x13b on; the size word
 * at 0xe34 + 4; and the HQ pixel-format word's GL_ABGR_EXT bit.
 */
#define MGRAS_TEX_GE_PROGRAM     0x13b
#define MGRAS_TEX_GE_SIZE        (0xe34 + 4)
#define MGRAS_TEX_UNPACK_4UB     0x4064
#define MGRAS_TEX_HQ_ABGR        0x80
static const uint32_t mgras_tex_program_4ub[] = {
    0x1f44, 0x3e65, 0x156a, 0x352d,
};

/* TL_MODE and TL_SPEC fields, and the kept formats the model stores. */
#define MGRAS_TEX_LAYOUT(mode)   (((mode) >> 1) & 3)
#define MGRAS_TEX_DEPTH(mode)    (((mode) >> 5) & 3)
#define MGRAS_TEX_LAYOUT_RGBA    3
#define MGRAS_TEX_DEPTH_4        0
#define MGRAS_TEX_DEPTH_8        1
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

static uint32_t mgras_tex_texel_bytes(uint32_t mode)
{
    return MGRAS_TEX_DEPTH(mode) == MGRAS_TEX_DEPTH_4 ? 2 : 4;
}

static uint64_t mgras_tex_level_bytes(uint32_t spec, uint32_t mode)
{
    return (1ull << MGRAS_TEX_SPEC_LOG2W(spec)) <<
           MGRAS_TEX_SPEC_LOG2H(spec) << (mgras_tex_texel_bytes(mode) / 2);
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

/* Whether the GE program loaded is the client's four-unsigned-byte one. */
static bool mgras_tex_program_known(SGIMgrasState *s)
{
    const uint32_t *ge = s->ge_param;
    uint32_t n = ARRAY_SIZE(mgras_tex_program_4ub);
    uint32_t params = ge[MGRAS_TEX_GE_PROGRAM + 3];

    for (uint32_t i = 0; i < n; i++) {
        if (ge[MGRAS_TEX_GE_PROGRAM + 2 * i] != mgras_tex_program_4ub[i]) {
            return false;
        }
    }
    return params < SGI_MGRAS_GE_PARAM_WORDS - 1 &&
           ge[params + 1] == MGRAS_TEX_UNPACK_4UB;
}

bool sgi_mgras_texture_load_start(SGIMgrasState *s, const uint32_t *payload)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t spec = t->tl_spec, mode = t->tl_mode;
    uint32_t w = 1u << MGRAS_TEX_SPEC_LOG2W(spec);
    uint32_t h = 1u << MGRAS_TEX_SPEC_LOG2H(spec);
    uint32_t size = s->ge_param[MGRAS_TEX_GE_SIZE];

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
    if (!mgras_tex_program_known(s) ||
        (s->dma_desc_30000[0] & MGRAS_TEX_HQ_ABGR)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture load through GE pixel "
                      "program 0x%x, 0x%x (HQ pixel format 0x%x) is not "
                      "modelled; nothing stored\n",
                      s->ge_param[MGRAS_TEX_GE_PROGRAM],
                      s->ge_param[MGRAS_TEX_GE_PROGRAM + 2],
                      s->dma_desc_30000[0]);
        return true;
    }
    if (size != (h << 16 | w)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture load size word 0x%x "
                      "is not TL_SPEC 0x%x's %u by %u; nothing stored\n",
                      size, spec, w, h);
        return true;
    }
    if (MGRAS_TEX_LAYOUT(mode) != MGRAS_TEX_LAYOUT_RGBA ||
        MGRAS_TEX_DEPTH(mode) > MGRAS_TEX_DEPTH_8) {
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
    t->load_spec = spec;
    t->load_mode = mode;
    return true;
}

bool sgi_mgras_texture_dma(SGIMgrasState *s, uint32_t rows, uint32_t bpr,
                           bool *take)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint64_t host = (1ull << MGRAS_TEX_SPEC_LOG2W(t->load_spec)) <<
                    MGRAS_TEX_SPEC_LOG2H(t->load_spec) << 2;

    *take = false;
    if (!t->load) {
        return false;
    }
    /* Each traced level came as one row; rows are taken end to end. */
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

/* Keep the gathered host image as the load's level. */
static void mgras_tex_commit(SGIMgrasState *s)
{
    SGIMgrasTexture *t = mgras_tex(s);
    uint32_t level = MGRAS_TEX_SPEC_LEVEL(t->load_spec);
    uint32_t bytes = mgras_tex_level_bytes(t->load_spec, t->load_mode);
    uint32_t texels = t->staging_bytes / 4;
    uint64_t board = 0;
    SGIMgrasTexLevel *l = NULL, *free = NULL;
    const uint8_t *in = t->staging;
    uint8_t *out;

    for (int i = 0; i < SGI_MGRAS_TEX_LEVELS; i++) {
        SGIMgrasTexLevel *e = &t->levels[i];

        if (e->used && e->base == t->load_base && e->level == level) {
            l = e;
        } else if (!e->used && !free) {
            free = e;
        }
    }
    l = l ? l : free;
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        board += mgras_tex_bytes_kept(&s->gl_context[i].tex, l);
    }
    if (!l || mgras_tex_bytes_kept(t, l) + bytes > MGRAS_TEX_CONTEXT_BYTES ||
        board + bytes > MGRAS_TEX_BOARD_BYTES) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: no room for texture level %u "
                      "of base %u; nothing stored\n", level, t->load_base);
        goto done;
    }
    mgras_tex_level_free(l);
    out = g_malloc(bytes);
    for (uint32_t i = 0; i < texels; i++, in += 4) {
        if (MGRAS_TEX_DEPTH(t->load_mode) == MGRAS_TEX_DEPTH_4) {
            /* Truncation to 4 bits, the model's choice. */
            out[2 * i] = (in[0] & 0xf0) | in[1] >> 4;
            out[2 * i + 1] = (in[2] & 0xf0) | in[3] >> 4;
        } else {
            memcpy(&out[4 * i], in, 4);
        }
    }
    *l = (SGIMgrasTexLevel) {
        .used = 1,
        .base = t->load_base,
        .level = level,
        .log2w = MGRAS_TEX_SPEC_LOG2W(t->load_spec),
        .log2h = MGRAS_TEX_SPEC_LOG2H(t->load_spec),
        .mode = t->load_mode,
        .bytes = bytes,
        .texels = out,
    };
    trace_sgi_mgras_texture_level(t->load_base, level, 1u << l->log2w,
                                  1u << l->log2h, t->load_mode);
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

/* A stored texel as canonical RRGGBBAA, a 4-bit component replicated. */
static bool mgras_tex_read_texel(void *opaque, uint32_t x, uint32_t y,
                                 uint32_t *rgba)
{
    const SGIMgrasTexLevel *l = opaque;
    uint32_t i = (y << l->log2w) + x;

    if (MGRAS_TEX_DEPTH(l->mode) == MGRAS_TEX_DEPTH_4) {
        uint32_t v = lduw_be_p(&l->texels[2 * i]);

        *rgba = 0;
        for (int k = 0; k < 4; k++) {
            *rgba = *rgba << 8 | ((v >> (12 - 4 * k)) & 0xf) * 0x11;
        }
    } else {
        *rgba = ldl_be_p(&l->texels[4 * i]);
    }
    return true;
}

const SGIGLTexImage *sgi_mgras_texture_image(SGIMgrasState *s,
                                             SGIGLTexImage *image)
{
    SGIMgrasTexture *t = mgras_tex(s);
    SGIMgrasTexLevel *l = NULL;
    uint32_t size;

    if (!mgras_tex_present(s) ||
        !s->gl_context[s->gl_current].gl->cur.texture_2d) {
        return NULL;
    }
    for (int i = 0; i < SGI_MGRAS_TEX_LEVELS; i++) {
        if (t->levels[i].used && t->levels[i].base == t->base &&
            t->levels[i].level == 0) {
            l = &t->levels[i];
        }
    }
    if (!l) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: textured draw with no level 0 "
                      "loaded under base %u\n", t->base);
        return NULL;
    }
    size = l->log2h << 4 | l->log2w;
    if ((t->txsize & 0xffff) != (size << 8 | size) ||
        (t->texmode1 & (MGRAS_TEXMODE1_ENV | MGRAS_TEXMODE1_SELECT |
                        MGRAS_TEXMODE1_TABLE)) ||
        ((t->texmode1 >> 3) & 3) != MGRAS_TEX_LAYOUT(l->mode) ||
        (t->texmode2 & MGRAS_TEXMODE2_UNDRAWN) ||
        ((t->texmode2 >> 2) & 3) != MGRAS_TEX_DEPTH(l->mode) ||
        t->t_offset) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture state TEXMODE1 0x%x "
                      "TEXMODE2 0x%x TXSIZE 0x%x offset 0x%x with a %u by %u "
                      "level kept as 0x%x is not modelled; drawn "
                      "untextured\n", t->texmode1, t->texmode2, t->txsize,
                      t->t_offset, 1u << l->log2w, 1u << l->log2h, l->mode);
        return NULL;
    }
    *image = (SGIGLTexImage) {
        .width = 1u << l->log2w,
        .height = 1u << l->log2h,
        .base_format = SGI_GL_RGBA,
        .min_filter = SGI_GL_NEAREST,
        .mag_filter = SGI_GL_NEAREST,
        .wrap_s = SGI_GL_REPEAT,
        .wrap_t = SGI_GL_REPEAT,
        .read_texel = mgras_tex_read_texel,
        .opaque = l,
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
    if (t->load &&
        (MGRAS_TEX_LAYOUT(t->load_mode) != MGRAS_TEX_LAYOUT_RGBA ||
         MGRAS_TEX_DEPTH(t->load_mode) > MGRAS_TEX_DEPTH_8 ||
         mgras_tex_level_bytes(t->load_spec, t->load_mode) >
         MGRAS_TEX_CONTEXT_BYTES)) {
        return false;
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
                o->level == l->level) {
                return false;
            }
        }
        if (l->used != 1 || l->level > 0xf || l->log2w > 0xf ||
            l->log2h > 0xf || !l->texels ||
            MGRAS_TEX_LAYOUT(l->mode) != MGRAS_TEX_LAYOUT_RGBA ||
            MGRAS_TEX_DEPTH(l->mode) > MGRAS_TEX_DEPTH_8 ||
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
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(used, SGIMgrasTexLevel),
        VMSTATE_UINT32(base, SGIMgrasTexLevel),
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
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(texmode1, SGIMgrasTexture),
        VMSTATE_UINT32(texmode2, SGIMgrasTexture),
        VMSTATE_UINT32(txsize, SGIMgrasTexture),
        VMSTATE_UINT32(txlod, SGIMgrasTexture),
        VMSTATE_UINT32(txclampsize, SGIMgrasTexture),
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
        VMSTATE_UINT32(load_spec, SGIMgrasTexture),
        VMSTATE_UINT32(load_mode, SGIMgrasTexture),
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
                   t->txclampsize | t->txbcolor[0] | t->txbcolor[1] |
                   t->txaddr | t->tl_mode | t->tl_spec | t->tl_wbuffer |
                   t->texrbuffer | t->base | t->t_offset | t->on |
                   t->bracket | t->load | t->load_base | t->load_spec |
                   t->load_mode;

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
