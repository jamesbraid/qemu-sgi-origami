/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SI MGRAS OpenGL command execution.
 *
 * On the board the HQ4 and GE11 run SGI programs that turn the OpenGL
 * client's command words into raster-engine work. Those programs are
 * opaque here, so the model reads the words at the boundary the IRIX
 * 6.5.30 client library defines and draws what the OpenGL 1.2.1 specification
 * says those commands produce. This is an approximation of the geometry
 * engine's function, not of its program.
 *
 * What it preserves: the pixels that colour and depth clears, untextured
 * points, lines, triangles, strips, fans, quads and polygons, at size and
 * width 1, flat or smooth shaded, transformed by the modelview and
 * projection matrices and their stacks and depth tested, and bitmaps,
 * leave in the window's front or back buffer, in RGBA or colour-index
 * mode, and the main-port framing,
 * so the words after a packet still parse as headers. What it does not: the
 * raster-engine register writes the GE would make, the timing of the
 * work, any GE state visible to a later readback, and every command
 * outside the subset below, which the parser logs through LOG_UNIMP and
 * consumes by length as before.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "hw/misc/sgi-mgras-gl.h"
#include "hw/display/sgi/rasterizer.h"

/* Class-0 opcodes, bits 15:8 of the header, with their byte lengths. */
#define MGRAS_GL_CONTEXT_OPENER  0x00000600
/*
 * glBegin and glEnd: 0x1600 to 0x1f00 begin the ten primitive modes, and
 * each mode's end token is 0x0a00 above its begin token
 * (mgras-gl-commands.md, "Primitives"). The tokens do not follow the
 * modes' enumerant order: 0x1800 begins GL_LINE_STRIP (3) and 0x1900
 * GL_LINE_LOOP (2). The defines below and mgras_gl_begin_tokens[] are the
 * mapping; no arithmetic on the mode stands in for them.
 */
#define MGRAS_GL_BEGIN_POINTS    0x00001600
#define MGRAS_GL_BEGIN_LINES     0x00001700
#define MGRAS_GL_BEGIN_LINE_STRIP 0x00001800
#define MGRAS_GL_BEGIN_LINE_LOOP 0x00001900
#define MGRAS_GL_BEGIN_POLYGON   0x00001f00
#define MGRAS_GL_END_OFFSET      0x00000a00
#define MGRAS_GL_BEGIN_TRIANGLES 0x00001a00
#define MGRAS_GL_BEGIN_TRI_STRIP 0x00001b00
#define MGRAS_GL_BEGIN_TRI_FAN   0x00001c00
#define MGRAS_GL_BEGIN_QUADS     0x00001d00
#define MGRAS_GL_BEGIN_QUAD_STRIP 0x00001e00
#define MGRAS_GL_CLEAR_COLOR_BUF 0x00001500
#define MGRAS_GL_CLEAR_DEPTH_BUF 0x00004a00
#define MGRAS_GL_VIEWPORT        0x00003310
#define MGRAS_GL_FLUSH           0x00003600
#define MGRAS_GL_SHADE_MODEL     0x00004704
#define MGRAS_GL_CLEAR_COLOR     0x0000ba10
#define MGRAS_GL_CLEAR_DEPTH     0x0000bc04
#define MGRAS_GL_MATRIX_MODE     0x00002a04
#define MGRAS_GL_LOAD_MATRIX     0x00002b40
#define MGRAS_GL_LOAD_IDENTITY   0x00002c00
#define MGRAS_GL_MULT_MATRIX     0x00002d40
#define MGRAS_GL_FRUSTUM         0x00003418
#define MGRAS_GL_ORTHO           0x00003518
/*
 * The matrix stack and transforms, named by the client's display-list
 * decoder (notes-lists-arrays-lighting.md in the Origami repository):
 * glRotatef's words are the angle in degrees and the axis.
 */
#define MGRAS_GL_POP_MATRIX      0x00002e00
#define MGRAS_GL_PUSH_MATRIX     0x00002f00
#define MGRAS_GL_ROTATE          0x00003010
#define MGRAS_GL_SCALE           0x0000310c
#define MGRAS_GL_TRANSLATE       0x0000320c
#define MGRAS_GL_DEPTH_MASK      0x00003c04
#define MGRAS_GL_DEPTH_FUNC      0x00004304
#define MGRAS_GL_DRAW_BUFFER     0x0000490c
#define MGRAS_GL_DEPTH_TEST      0x00006604
#define MGRAS_GL_BANKS           0x00009808

/* Vertex-attribute headers: the class names the source format. */
#define MGRAS_GL_VERTEX2F        0x40000008
#define MGRAS_GL_VERTEX3F        0x2000000c
#define MGRAS_GL_VERTEX4F        0x00000010
#define MGRAS_GL_COLOR3F         0x2000020c
#define MGRAS_GL_COLOR4F         0x00000210
#define MGRAS_GL_VERTEX2I        0x58000008
#define MGRAS_GL_COLOR4UB        0x14800204
/*
 * glColor3ub is sent as 0x34800203 and one word. The model matches every
 * header with its byte length rounded up to the whole words the payload
 * takes (mgras-gl-commands.md, "Word framing").
 */
#define MGRAS_GL_COLOR3UB        0x34800204
/*
 * glNormal3f, and glEdgeFlag true and false (mgras-gl-commands.md and
 * notes-lists-arrays-lighting.md in the Origami repository).
 */
#define MGRAS_GL_NORMAL3F        0x0000070c
/* glRasterPos2f and 3f (mgras-gl-commands.md, attribute table). */
#define MGRAS_GL_RASTER_POS2F    0x40003808
#define MGRAS_GL_RASTER_POS3F    0x2000380c
#define MGRAS_GL_EDGE_FLAG_TRUE  0x00000f00
#define MGRAS_GL_EDGE_FLAG_FALSE 0x00001000

/*
 * Colour-index mode, from the IRIX 6.5.30 IP27GR532 libGLcore.so's own
 * disassembly (SHA-256 626d6d94...). The context setup calls
 * __glMgrInitCI (0xd94e830) for each colour buffer when the visual is a
 * colour-index one and another initializer otherwise; __glMgrInitCI
 * writes 0x0a04 and a word of the visual's configuration, from which it
 * also forms the index mask (1 << n) - 1, and __glMgrInitRGB (0xd973620)
 * writes 0x0900 and nothing more. So 0x0a04's word is the number of
 * bits a colour index holds, 8 in gr_osview's context setup and in
 * glxinfo's (the application survey's run irix-20260928T004550-1040f3,
 * and mgras-gl-commands.md), and 0x0900 marks an RGBA context. That the
 * board switches its colour handling on them is inferred.
 */
#define MGRAS_GL_INIT_CI         0x00000a04
#define MGRAS_GL_INIT_RGB        0x00000900
/*
 * glIndexs, glIndexi and glIndexf: headers 0x19000202, 0x18000204 and
 * 0x00000204, the colour register 0x02 under the attribute classes for a
 * short, an integer and a float (__glMgrim_Indexs 0xdac7f90,
 * __glMgrim_Indexi 0xdac7f5c, __glMgrim_Indexf 0xdac7f30). glIndexs
 * sends the short's 16 bits in bits 31:16 of its one word and zeros in
 * 15:0 (andi 0xffff, dsll 16); gr_osview's words 0x20000 and 0x30000 are
 * indices 2 and 3. The short is read signed, as the class's short type
 * reads for a vertex (inferred from the attribute table); glIndexs's
 * header is matched with its length rounded up to the word it takes.
 */
#define MGRAS_GL_INDEXS          0x19000204
#define MGRAS_GL_INDEXI          0x18000204
#define MGRAS_GL_INDEXF          0x00000204
/*
 * glIndexMask (__glMgrim_IndexMask 0xd95e9d0): 0x3d04 and the mask ANDed
 * with the context's index mask. glClearIndex (__glMgrim_ClearIndex
 * 0xd980ba0): 0xbd04 and a float, trunc(c * 16.0) ANDed with (index mask
 * << 4) | 0xf and scaled by 0.0625, the constants the lit4 words at file
 * offsets 0x1d940c and 0x1d9410 hold: the index with four fraction bits.
 */
#define MGRAS_GL_INDEX_MASK      0x00003d04
#define MGRAS_GL_CLEAR_INDEX     0x0000bd04

/*
 * glBitmap (__glMgrim_Bitmap 0xd9c1b70, and __glMgrRenderBitmap 0xd9c1e70
 * for a display list). In render mode, with glPixelStore's unpack state at
 * its defaults but an alignment of one, and an image of at most 512
 * words, the client writes 0x9420 and eight words: a flags word, the width
 * and height as integers, xorig, yorig, xmove and ymove as floats, and the
 * image's length in words, ceil(ceil(width / 8) * height / 4). An inline
 * data block follows, 0x80000000 | length * 4, whose words
 * __glMgr_PushPixels (0xdac61a0) copies from the application's image in
 * memory order; a 64-bit store is two stream words, high first. When
 * the image is not word aligned the client sets bits 23:22 of the block
 * header to the image's byte offset in its first word and sends one word
 * more, from the word holding the first byte. A zero word follows when
 * the length is odd. gr_osview's 94 bitmaps in the survey run
 * (irix-20260928T004550-1040f3) are all this form: flags 0x1800f, width
 * 16, height 1 or 2, one data word and the zero word. The other paths,
 * for a larger image or other unpack state (0x9114, 0x9008 with raster
 * register records, then 0x9208 with xmove and ymove), are not modelled.
 *
 * Established from the listing: the packet's words and their order,
 * the block's length and offset, and the bytes' order in the stream.
 * Inferred: that the image is rows of ceil(width / 8) bytes from the
 * bottom row up with the most significant bit leftmost (OpenGL 1.2.1,
 * 3.6.4 and 3.7, glPixelStore's defaults; the client requires them before
 * taking this path, and its other path sends the least-significant-first
 * flag as bit 17 of the flags word, which this path leaves clear), that
 * the set bits take the raster position's colour or index and depth, and
 * that the board moves the raster position by xmove and ymove, which the
 * client does not do itself on this path but sends 0x9208 for on the
 * other. The flags word's bits 16:14, (8 - ceil(width / 8) % 8) % 8, and
 * bits 3:0, 0xf, are unknown; the model does not use them.
 */
#define MGRAS_GL_BITMAP          0x00009420
#define MGRAS_GL_BITMAP_OP       0x94
#define MGRAS_GL_BITMAP_WORDS    8
#define MGRAS_GL_BITMAP_LSB_FIRST 0x00020000
/*
 * __glMgrUpdateRasterPos (0xd9c9040) writes 0xd608 and two floats; the
 * client's glBitmap calls it with xmove and ymove for an empty image, so
 * it moves a valid raster position by them (inferred, 3.7).
 */
#define MGRAS_GL_MOVE_RASTER     0x0000d608

/*
 * glShadeModel's and glMatrixMode's words are the OpenGL enumerants, and
 * glDepthFunc's is the function minus GL_NEVER (OpenGL 4.1.5), the form
 * SGIGLDrawState keeps.
 *
 * The draw-buffer triple's first word: 1 in the context setup of the
 * probe's single-buffered visual, 2 in that of its double-buffered one
 * (run irix-20260927T172059-fdbada), where OpenGL's default draw buffer is
 * the front and the back buffer respectively. Reading it as a bit mask of
 * the front and back buffers, SGI_GL_DRAW_FRONT and SGI_GL_DRAW_BACK, is
 * inferred from those two values; the other bits are not modelled.
 */

/*
 * The window the current context draws into, from the 63-word image the
 * IRIX kernel's MgrasCtxsw writes at each context switch. In the image of
 * the probe's GL context, whose window's top-left pixel is at screen (256,
 * 256) and which is 128 pixels square (run irix-20260927T135825-92207b,
 * seq 3959112), word 2 is 0x02800100, the window's lower-left corner as
 * (y << 16) | x with y counted up from the bottom of the screen (640 =
 * 1024 - 256 - 128), the packing OpenBSD's impact(4) gives XYWIN. Words 11
 * and 12 are 0x0100017f and 0x028002ff, the window's columns 256-383 and
 * rows 640-767 from the bottom as first << 16 | last, the layout the X
 * server writes to the clip registers 0x147 and 0x148, and word 3 is 0x11,
 * the value it writes to 0x14f with a clip box. The X server's own
 * image carries 0x03ff0000 in word 2, XYWIN's reset value. That the board
 * restores these words into those registers is inferred from the matching
 * values; the model reads them from the image and keeps them out of the
 * 2D register file. Only one window position has been captured.
 */
#define MGRAS_GL_IMAGE_ORIGIN    2
#define MGRAS_GL_IMAGE_CLIP_MODE 3
#define MGRAS_GL_IMAGE_CLIP_X    11
#define MGRAS_GL_IMAGE_CLIP_Y    12
#define MGRAS_GL_CLIP_ON         0x11

/*
 * Words 16 and 17 of every captured GL context image are 1, those of the
 * X server's images 0 (run irix-20260927T172059-fdbada). The kernel's
 * bank token 0x9808 carries two words of the same form, 0 at a window's
 * first swap and 1 at its second, and the swaps then show first the frame
 * drawn before the first swap and then the one drawn between them. Both
 * fit these words naming the pixel-memory bank that holds the back
 * buffer, with the front buffer in the other; that the image restores
 * them is inferred from the matching values. The model uses the first
 * word; what distinguishes the second is unknown.
 */
#define MGRAS_GL_IMAGE_BANKS     16

/* The begin token of the mode a glBegin or glEnd token names, or 0. */
static uint32_t mgras_gl_begin_token(uint32_t header, bool end)
{
    uint32_t begin = end ? header - MGRAS_GL_END_OFFSET : header;

    if (begin < MGRAS_GL_BEGIN_POINTS || begin > MGRAS_GL_BEGIN_POLYGON ||
        (begin & 0xff)) {
        return 0;
    }
    return begin;
}

/* The GL primitive a begin token opens, and the token that opens one. */
static const uint32_t mgras_gl_begin_tokens[] = {
    [SGI_GL_POINTS] = MGRAS_GL_BEGIN_POINTS,
    [SGI_GL_LINES] = MGRAS_GL_BEGIN_LINES,
    [SGI_GL_LINE_LOOP] = MGRAS_GL_BEGIN_LINE_LOOP,
    [SGI_GL_LINE_STRIP] = MGRAS_GL_BEGIN_LINE_STRIP,
    [SGI_GL_TRIANGLES] = MGRAS_GL_BEGIN_TRIANGLES,
    [SGI_GL_TRIANGLE_STRIP] = MGRAS_GL_BEGIN_TRI_STRIP,
    [SGI_GL_TRIANGLE_FAN] = MGRAS_GL_BEGIN_TRI_FAN,
    [SGI_GL_QUADS] = MGRAS_GL_BEGIN_QUADS,
    [SGI_GL_QUAD_STRIP] = MGRAS_GL_BEGIN_QUAD_STRIP,
    [SGI_GL_POLYGON] = MGRAS_GL_BEGIN_POLYGON,
};

static uint32_t mgras_gl_mode_of_begin(uint32_t begin)
{
    for (uint32_t mode = 0; mode < ARRAY_SIZE(mgras_gl_begin_tokens);
         mode++) {
        if (mgras_gl_begin_tokens[mode] == begin) {
            return mode;
        }
    }
    g_assert_not_reached();
}

static uint32_t mgras_gl_begin_of_mode(uint32_t mode)
{
    assert(mode < ARRAY_SIZE(mgras_gl_begin_tokens));
    return mgras_gl_begin_tokens[mode];
}

/*
 * A vertex holding OpenGL's initial attributes (2.7): position and
 * texture coordinate (0, 0, 0, 1), normal (0, 0, 1), edge flag true,
 * colour index 1, material 0.
 */
static void mgras_gl_vertex_init(float v[SGI_GL_VERTEX_FLOATS])
{
    static const uint32_t edge = 1;

    memset(v, 0, SGI_GL_VERTEX_FLOATS * sizeof(float));
    v[SGI_GL_V_POSITION + 3] = 1.0f;
    v[SGI_GL_V_NORMAL + 2] = 1.0f;
    v[SGI_GL_V_TEXCOORD + 3] = 1.0f;
    memcpy(&v[SGI_GL_V_EDGE], &edge, sizeof(edge));
    v[SGI_GL_V_INDEX] = 1.0f;
}

bool sgi_mgras_gl_header(uint32_t header)
{
    header = (header & ~0xffu) |
             (DIV_ROUND_UP(header & 0xff, sizeof(uint32_t)) *
              sizeof(uint32_t));
    if (mgras_gl_begin_token(header, false) ||
        mgras_gl_begin_token(header, true)) {
        return true;
    }
    switch (header) {
    case MGRAS_GL_CONTEXT_OPENER:
    case MGRAS_GL_CLEAR_COLOR_BUF:
    case MGRAS_GL_CLEAR_DEPTH_BUF:
    case MGRAS_GL_VIEWPORT:
    case MGRAS_GL_FLUSH:
    case MGRAS_GL_SHADE_MODEL:
    case MGRAS_GL_CLEAR_COLOR:
    case MGRAS_GL_CLEAR_DEPTH:
    case MGRAS_GL_MATRIX_MODE:
    case MGRAS_GL_LOAD_MATRIX:
    case MGRAS_GL_LOAD_IDENTITY:
    case MGRAS_GL_MULT_MATRIX:
    case MGRAS_GL_FRUSTUM:
    case MGRAS_GL_ORTHO:
    case MGRAS_GL_POP_MATRIX:
    case MGRAS_GL_PUSH_MATRIX:
    case MGRAS_GL_ROTATE:
    case MGRAS_GL_SCALE:
    case MGRAS_GL_TRANSLATE:
    case MGRAS_GL_DEPTH_MASK:
    case MGRAS_GL_DEPTH_FUNC:
    case MGRAS_GL_DRAW_BUFFER:
    case MGRAS_GL_DEPTH_TEST:
    case MGRAS_GL_BANKS:
    case MGRAS_GL_VERTEX2F:
    case MGRAS_GL_VERTEX3F:
    case MGRAS_GL_VERTEX4F:
    case MGRAS_GL_COLOR3F:
    case MGRAS_GL_COLOR4F:
    case MGRAS_GL_VERTEX2I:
    case MGRAS_GL_COLOR3UB:
    case MGRAS_GL_COLOR4UB:
    case MGRAS_GL_NORMAL3F:
    case MGRAS_GL_EDGE_FLAG_TRUE:
    case MGRAS_GL_EDGE_FLAG_FALSE:
    case MGRAS_GL_RASTER_POS2F:
    case MGRAS_GL_RASTER_POS3F:
    case MGRAS_GL_INIT_CI:
    case MGRAS_GL_INIT_RGB:
    case MGRAS_GL_INDEXS:
    case MGRAS_GL_INDEXI:
    case MGRAS_GL_INDEXF:
    case MGRAS_GL_INDEX_MASK:
    case MGRAS_GL_CLEAR_INDEX:
    case MGRAS_GL_BITMAP:
    case MGRAS_GL_MOVE_RASTER:
        return true;
    default:
        return false;
    }
}

/*
 * glColor3ub's and glColor4ub's bytes in one word, the first component in
 * bits 31:24: colorbars' bars (run irix-20260928T004550-1040f3, from seq
 * 26371573) are 0x87878700, 0x87870000, 0x00878700, 0x00870000,
 * 0x87008700 and 0x87000000, white, yellow, cyan, green, magenta and red.
 * Alpha in bits 7:0 follows from that component order; noof's constant
 * glColor4ub word 0x00000060 is consistent with it but cannot fix it
 * alone.
 */
static void mgras_gl_color_bytes(uint32_t bytes[4], uint32_t word)
{
    for (int i = 0; i < 4; i++) {
        bytes[i] = (word >> (24 - 8 * i)) & 0xff;
    }
}

/*
 * Context image word 0 names the context's GL state. In run
 * irix-20260927T191513-b4997c it is 0x3bcd and 0x5420 for two GL clients,
 * 0x237a for the X server and 0xb27 for the kernel's default context.
 * These are allocations observed in that run, not published values, and
 * MGRAS_GL_DEFAULT_SLOT is the one observed there. The IRIX 6.5.30 kernel
 * allocates the word as the context's slot in GE external RAM and writes
 * -1 for a context with no GL state. The image
 * carries no other GL state, the clients never re-send theirs after a
 * switch, and nothing touches the GE ports at those switches, so the board
 * keeps each context's state under its slot (inferred;
 * mgras-gl-commands.md, "Per-context state").
 *
 * Approximated: the model keys its state by the slot address the kernel
 * chose, not by GE external RAM contents, and never interprets the GE
 * ports. When the kernel runs out of slots it copies one out through the
 * GE ports and later back in, possibly under another address; the model
 * cannot follow that. It keeps the old state under a reused address,
 * relying on the opener to start a new context, and cannot restore a
 * copied-back one. No capture has had enough GL contexts to show it. The
 * model holds 32 slots and replaces the least recently loaded when a 33rd
 * arrives. The discriminating tests are gl-state-follows-the-context-slot,
 * gl-context-opener-resets-only-its-context and
 * gl-context-table-replaces-the-least-recent.
 */
#define MGRAS_GL_IMAGE_SLOT      0
#define MGRAS_GL_NO_SLOT         0x80000000u
#define MGRAS_GL_DEFAULT_SLOT    0xb27

static SGIGLContext *mgras_gl(SGIMgrasState *s)
{
    return s->gl_context[s->gl_current].gl;
}

/*
 * The loaded context image's window and the banks the image or the bank
 * token last named, as the drawable the loaded GL context draws into.
 * MGRAS's colour targets are masks of pixel-memory banks: bank 0, the
 * model's front[] plane and the one the X server draws in, and bank 1,
 * back[]. Only mgras_gl_store_colour() reads them. A context image
 * without the clip words' 0x11 in word 3, or with an empty rectangle,
 * leaves the context without a window.
 *
 * The drawable is bound when a context image completes, and at the other
 * points that change it, not read from the image at each draw. The two
 * differ only after a guest protocol error: mgras_error() can end a
 * context transaction partway through the image, with the port still
 * ready, and GL words that follow draw into the window bound at the last
 * completed switch, where a per-draw read would take the partly written
 * image's. A migration inside such a transaction rebinds from the partly
 * written image, since that is what the stream carries, so the two sides
 * can then draw into different windows. The board's behaviour after such
 * an error is unknown either way.
 */
static void mgras_gl_bind(SGIMgrasState *s)
{
    uint32_t origin = s->context[MGRAS_GL_IMAGE_ORIGIN];
    uint32_t clip_x = s->context[MGRAS_GL_IMAGE_CLIP_X];
    uint32_t clip_y = s->context[MGRAS_GL_IMAGE_CLIP_Y];
    unsigned int back = s->gl_banks[0] & 1;
    SGIGLDrawable d = {
        .origin = { origin & 0xffff, origin >> 16 },
        .clip = {
            .n = 1,
            .r[0] = {
                .x0 = clip_x >> 16,
                .y0 = clip_y >> 16,
                .x1 = MIN(clip_x & 0xffff, s->width - 1),
                .y1 = MIN(clip_y & 0xffff, s->height - 1),
            },
        },
        .front = 1u << (back ^ 1),
        .back = 1u << back,
    };

    d.bound = s->context[MGRAS_GL_IMAGE_CLIP_MODE] == MGRAS_GL_CLIP_ON &&
              d.clip.r[0].x0 <= d.clip.r[0].x1 &&
              d.clip.r[0].y0 <= d.clip.r[0].y1;
    sgi_gl_set_drawable(mgras_gl(s), &d);
}

void sgi_mgras_gl_reset(SGIMgrasState *s)
{
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        s->gl_context[i].gl = &s->gl[i];
        memset(&s->gl_context[i].parser, 0, sizeof(s->gl_context[i].parser));
        sgi_gl_init(&s->gl[i], 0);
    }
    memset(s->gl_slot, 0, sizeof(s->gl_slot));
    memset(s->gl_selected, 0, sizeof(s->gl_selected));
    s->gl_selections = 0;
    s->gl_current = 0;
    s->gl_ge_port_logged = false;
    /* The value every captured GL context image restores. */
    s->gl_banks[0] = s->gl_banks[1] = 1;
    mgras_gl_bind(s);
}

/* The entry holding slot's state, taking a new one if it has none. */
static unsigned int mgras_gl_lookup(SGIMgrasState *s, uint32_t slot)
{
    unsigned int free = 0, oldest = 0;

    for (unsigned int i = 1; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (!s->gl_selected[i]) {
            free = free ? free : i;
        } else if (s->gl_slot[i] == slot) {
            return i;
        } else if (!oldest || s->gl_selected[i] < s->gl_selected[oldest]) {
            oldest = i;
        }
    }
    if (!free) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL context slot 0x%x replaces "
                      "slot 0x%x, loaded least recently; that context's GL "
                      "state is lost\n", slot, s->gl_slot[oldest]);
        free = oldest;
    }
    sgi_gl_init(&s->gl[free], slot);
    memset(&s->gl_context[free].parser, 0,
           sizeof(s->gl_context[free].parser));
    s->gl_slot[free] = slot;
    return free;
}

/*
 * A context switch can land inside a packet, and each context's words
 * continue its own packet when it is loaded again. In run
 * irix-20260928T000530-78d0c5 (seq 12929748 to 12930582) perfly's context
 * 0x84c6 was switched out 33 words into the 156 data words of a GE table
 * load, context 0x5420 was loaded and sent one glFlush header, and 0x84c6
 * came back and sent its remaining 123 data words. With one parser for
 * the board the glFlush was consumed as table data and 0x84c6's words then
 * parsed as headers, which later left the X server polling HQ flag bit 16
 * forever. The client cannot know where a switch falls, so the board
 * keeps the position per context (inferred). Approximated: the model
 * keeps it with the context's GL state, keyed the same way, not in
 * whatever HQ state the board saves. The discriminating tests are
 * gl-parser-follows-the-context and gl-packet-split-by-a-context-switch.
 */
void sgi_mgras_cmd_parser_save(SGIMgrasState *s, SGIMgrasCmdParser *p)
{
    p->cls = s->cmd_token_class;
    p->reg = s->cmd_token_reg;
    p->words_remaining = s->cmd_token_words_remaining;
    memcpy(p->payload, s->cmd_token_payload, sizeof(p->payload));
    p->payload_count = s->cmd_token_payload_count;
}

void sgi_mgras_cmd_parser_load(SGIMgrasState *s, const SGIMgrasCmdParser *p)
{
    s->cmd_token_class = p->cls;
    s->cmd_token_reg = p->reg;
    s->cmd_token_words_remaining = p->words_remaining;
    memcpy(s->cmd_token_payload, p->payload, sizeof(p->payload));
    s->cmd_token_payload_count = p->payload_count;
}

void sgi_mgras_gl_context_loaded(SGIMgrasState *s)
{
    uint32_t slot = s->context[MGRAS_GL_IMAGE_SLOT];

    sgi_mgras_cmd_parser_save(s, &s->gl_context[s->gl_current].parser);
    s->gl_banks[0] = s->context[MGRAS_GL_IMAGE_BANKS];
    s->gl_banks[1] = s->context[MGRAS_GL_IMAGE_BANKS + 1];
    s->gl_ge_port_logged = false;
    if ((slot & MGRAS_GL_NO_SLOT) || slot == MGRAS_GL_DEFAULT_SLOT) {
        s->gl_current = 0;
    } else {
        s->gl_current = mgras_gl_lookup(s, slot);
        s->gl_selected[s->gl_current] = ++s->gl_selections;
    }
    sgi_mgras_cmd_parser_load(s, &s->gl_context[s->gl_current].parser);
    mgras_gl_bind(s);
}

/*
 * The answer to the client's 0xa104 query for one current attribute.
 * __Mgr_GetCurrentAttrState reads indices 4 to 7 as GL_CURRENT_COLOR's
 * four floats, 8 to 10 as GL_CURRENT_NORMAL's, 11 to 14 as
 * GL_CURRENT_TEXTURE_COORDS' and 15 as GL_EDGE_FLAG, true when the answer
 * has any of bits 0x01c00000 (notes-return-mode-queries.md in the Origami
 * repository). The model answers with the loaded context's colour, normal
 * and edge flag; that the board answers with its copy of that state is
 * inferred. Which of the edge flag's three bits the board sets is
 * unknown, so the model sets all three. Texture coordinates are not
 * modelled: the model answers OpenGL's initial (0, 0, 0, 1) (2.7), wrong
 * once a client has sent glTexCoord. Other indices answer zero, logged.
 * The discriminating tests are hq-attribute-queries-set-flag-after-arguments
 * and hq-attribute-query-answers-normal-and-edge-flag.
 *
 * The same routine reads GL_CURRENT_INDEX at index 4, and the raster
 * index shares 0x351 with the raster colour's red in the 0xa20c table,
 * so the board keeps an index where it keeps red (inferred). In
 * colour-index mode the model answers both with the index, as a float
 * the client truncates.
 */
#define MGRAS_GL_ATTR_INDEX      4
#define MGRAS_GL_QUERY_RASTER_INDEX 0x351

uint32_t sgi_mgras_gl_attribute_answer(SGIMgrasState *s, uint32_t index)
{
    static const struct {
        uint32_t pname;
        uint32_t first;
    } attrs[] = {
        { SGI_GL_CURRENT_COLOR, 4 },
        { SGI_GL_CURRENT_NORMAL, 8 },
        { SGI_GL_CURRENT_TEXTURE_COORDS, 11 },
        { SGI_GL_EDGE_FLAG, 15 },
    };
    uint32_t out[SGI_GL_GET_WORDS];
    unsigned n;

    if (index == MGRAS_GL_ATTR_INDEX && mgras_gl(s)->cur.index_bits &&
        sgi_gl_get(mgras_gl(s), SGI_GL_CURRENT_INDEX, out, &n)) {
        return out[0];
    }
    for (int i = 0; i < ARRAY_SIZE(attrs); i++) {
        if (index < attrs[i].first ||
            !sgi_gl_get(mgras_gl(s), attrs[i].pname, out, &n) ||
            index - attrs[i].first >= n) {
            continue;
        }
        if (attrs[i].pname == SGI_GL_EDGE_FLAG) {
            return out[0] ? 0x01c00000 : 0;
        }
        return out[index - attrs[i].first];
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL current-attribute query index "
                  "%u answered with zero\n", index);
    return 0;
}

/* How a query's answer word holds its piece of glGet's answer. */
typedef enum MgrasGLQueryPacking {
    /* The word glGet gives, float or integer. */
    MGRAS_GL_QUERY_WORD,
    /* The matrix mode's enumerant less GL_PROJECTION. */
    MGRAS_GL_QUERY_MATRIX_MODE,
    /* A raster window coordinate plus MGRAS_GL_RASTER_BIAS. */
    MGRAS_GL_QUERY_RASTER_XY,
    /* All ones for true, zero for false. */
    MGRAS_GL_QUERY_BOOLEAN_MASK,
} MgrasGLQueryPacking;

/*
 * The client adds this double, read at $gp - 0x39e8 in
 * __glMgrDoGet, to the x and y words of GL_CURRENT_RASTER_POSITION
 * before returning them (notes-return-mode-queries.md).
 */
#define MGRAS_GL_RASTER_BIAS     49151.5

/*
 * The 0xa20c state queries the model answers. __Mgr_ReturnMode sends an
 * index, a1 and a2 and reads one word back at 0x028004
 * (notes-return-mode-queries.md in the Origami repository). No index is a
 * literal: __glMgrDoGet takes each from a table the library loads from
 * the HQ microcode file's symbol table, so the index is an address the
 * board's program gives for that piece of GL state. The notes list each
 * glGet caller's index and what it does with the word, established from
 * the IRIX 6.5.30 IP27GR532 libGLcore.so; the values below are the
 * hq3_revB_HQ4.u column. That the board answers with its copy of the
 * state OpenGL 1.2.1 has glGet return is inferred from how the client
 * uses each word, and the model answers the loaded context's.
 *
 * - GL_MODELVIEW_MATRIX and GL_PROJECTION_MATRIX: 16 queries with a1 0,
 *   index 0x3a + i and 0x4a + i, each word stored as a float into
 *   element i of the result (the loop at 0xd992d28 advances its output
 *   one float per query), which glGet returns column-major.
 * - GL_MATRIX_MODE: index 0x29, a1 0; the client returns the word plus
 *   0x1701, so the word is the mode's enumerant less GL_PROJECTION.
 * - GL_VIEWPORT: 0xc1d to 0xc20, stored as integers x, y, width, height.
 * - GL_SHADE_MODEL: 0xc19, returned as the integer it is.
 * - GL_COLOR_CLEAR_VALUE: 0xc0a to 0xc0d, four floats.
 * - GL_CURRENT_RASTER_POSITION: x, y, z and w at 0x359, 0x35a, 0x35b and
 *   0x350, as floats. The client adds -49151.5 in double to x and y and
 *   returns z and w as they are, so the words are the window x and y plus
 *   49151.5, and z and clip w. In single precision the biased x and y
 *   hold the 1/256 pixel the model snaps vertices to; how the board rounds
 *   them is not published, and the model rounds to nearest.
 * - GL_CURRENT_RASTER_POSITION_VALID: 0x35c. The client ANDs the word
 *   with the microcode symbol table's word at offset 0x240 and returns
 *   whether the result is nonzero. That mask's value is not recorded, so
 *   the model answers all ones for a valid position and zero otherwise,
 *   which reads correctly for any mask but zero; which bits the board
 *   sets is unknown.
 * - GL_CURRENT_RASTER_COLOR 0x351 to 0x354, GL_CURRENT_RASTER_TEXTURE_COORDS
 *   0x355 to 0x358 and GL_CURRENT_RASTER_DISTANCE 0x34b, as floats.
 *
 * What a1 and a2 select is unknown; the model answers by index alone.
 * Every other index answers zero, logged, which is right for the
 * glFinish and MgrasFinish fences and glGetError, which discard or fall
 * back from a zero, and wrong for the other state a client reads back.
 * The discriminating tests are hq-query-answers-the-matrices,
 * hq-query-answers-viewport-shade-and-clear-colour and
 * hq-query-answers-the-raster-position.
 */
static const struct {
    uint16_t first;
    uint8_t count;
    uint8_t packing;            /* MgrasGLQueryPacking */
    uint16_t pname;
    uint8_t element;            /* the first query's element of glGet's */
} mgras_gl_queries[] = {
    { 0x03a, 16, MGRAS_GL_QUERY_WORD, SGI_GL_MODELVIEW_MATRIX, 0 },
    { 0x04a, 16, MGRAS_GL_QUERY_WORD, SGI_GL_PROJECTION_MATRIX, 0 },
    { 0x029, 1, MGRAS_GL_QUERY_MATRIX_MODE, SGI_GL_MATRIX_MODE, 0 },
    { 0xc1d, 4, MGRAS_GL_QUERY_WORD, SGI_GL_VIEWPORT, 0 },
    { 0xc19, 1, MGRAS_GL_QUERY_WORD, SGI_GL_SHADE_MODEL, 0 },
    { 0xc0a, 4, MGRAS_GL_QUERY_WORD, SGI_GL_COLOR_CLEAR_VALUE, 0 },
    { 0x359, 2, MGRAS_GL_QUERY_RASTER_XY, SGI_GL_CURRENT_RASTER_POSITION, 0 },
    { 0x35b, 1, MGRAS_GL_QUERY_WORD, SGI_GL_CURRENT_RASTER_POSITION, 2 },
    { 0x350, 1, MGRAS_GL_QUERY_WORD, SGI_GL_CURRENT_RASTER_POSITION, 3 },
    { 0x35c, 1, MGRAS_GL_QUERY_BOOLEAN_MASK,
      SGI_GL_CURRENT_RASTER_POSITION_VALID, 0 },
    { 0x351, 4, MGRAS_GL_QUERY_WORD, SGI_GL_CURRENT_RASTER_COLOR, 0 },
    { 0x355, 4, MGRAS_GL_QUERY_WORD, SGI_GL_CURRENT_RASTER_TEXTURE_COORDS, 0 },
    { 0x34b, 1, MGRAS_GL_QUERY_WORD, SGI_GL_CURRENT_RASTER_DISTANCE, 0 },
};

/*
 * The library loads hq3.u's table instead of hq3_revB_HQ4.u's when the
 * GE revision the kernel recorded is below 2, or the board has two GEs
 * and one RE (notes-return-mode-queries.md, "Where the index comes
 * from"). The kernel records the low byte of 0x028004 after the GE
 * program download, which the model holds at the board's GE revision
 * until the first query. Every board the model presents reports 2 or 3
 * and none has two GEs with one RE, so only the hq3_revB_HQ4.u indices
 * are decoded; a board that would select hq3.u answers nothing.
 */
static bool mgras_gl_query_table_known(const SGIMgrasBoard *b)
{
    return b->ge_revision >= 2 && !(b->ge_count == 2 && b->re_count == 1);
}

uint32_t sgi_mgras_gl_query_answer(SGIMgrasState *s, const uint32_t *args,
                                   uint32_t nargs)
{
    uint32_t index = nargs ? args[0] : 0, out[SGI_GL_GET_WORDS];
    unsigned n;
    float f;

    if (nargs != 3 || !mgras_gl_query_table_known(s->board)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL state query with %u "
                      "arguments on this board answered with zero\n", nargs);
        return 0;
    }
    if (index == MGRAS_GL_QUERY_RASTER_INDEX && mgras_gl(s)->cur.index_bits &&
        sgi_gl_get(mgras_gl(s), SGI_GL_CURRENT_RASTER_INDEX, out, &n)) {
        return out[0];
    }
    for (int i = 0; i < ARRAY_SIZE(mgras_gl_queries); i++) {
        uint32_t element = index - mgras_gl_queries[i].first;

        if (index < mgras_gl_queries[i].first ||
            element >= mgras_gl_queries[i].count ||
            !sgi_gl_get(mgras_gl(s), mgras_gl_queries[i].pname, out, &n)) {
            continue;
        }
        element += mgras_gl_queries[i].element;
        assert(element < n);
        switch (mgras_gl_queries[i].packing) {
        case MGRAS_GL_QUERY_MATRIX_MODE:
            return out[element] - SGI_GL_PROJECTION;
        case MGRAS_GL_QUERY_RASTER_XY:
            memcpy(&f, &out[element], sizeof(f));
            f = f + MGRAS_GL_RASTER_BIAS;
            memcpy(&out[element], &f, sizeof(f));
            return out[element];
        case MGRAS_GL_QUERY_BOOLEAN_MASK:
            return out[element] ? UINT32_MAX : 0;
        default:
            return out[element];
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL state query index 0x%x "
                  "answered with zero\n", index);
    return 0;
}

void sgi_mgras_gl_ge_port_access(SGIMgrasState *s, hwaddr addr, bool write)
{
    bool loaded = false;

    for (unsigned int i = 1; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        loaded |= s->gl_selected[i] != 0;
    }
    if (!loaded || s->gl_ge_port_logged) {
        return;
    }
    s->gl_ge_port_logged = true;
    qemu_log_mask(LOG_UNIMP, "sgi-mgras: GE port 0x%06" HWADDR_PRIx " %s "
                  "after GL contexts were loaded; the model does not save "
                  "or restore GL state through the GE ports\n", addr,
                  write ? "write" : "read");
}

/*
 * The sizes MGRAS's stream carries: three held vertices since version 15
 * and 31 saved matrices a stack since version 21. They are the stream's,
 * not the shared context's, so a change to the shared sizes stops the
 * build here instead of changing the stream's length.
 */
#define MGRAS_GL_WIRE_HELD       3
#define MGRAS_GL_WIRE_STACK      31
QEMU_BUILD_BUG_ON(SGI_GL_HELD_VERTICES != MGRAS_GL_WIRE_HELD);
QEMU_BUILD_BUG_ON(SGI_GL_STACK_DEPTH - 1 != MGRAS_GL_WIRE_STACK);

/*
 * The per-context GL state the stream has carried since version 15, in
 * that version's layout, which the shared context's fields no longer
 * follow for the open primitive: there the stream has the begin token,
 * the vertex count and each held vertex's position and colour, as two
 * arrays.
 *
 * The stream carries no other attribute of a held vertex, so one loaded
 * from it takes OpenGL's initial normal, texture coordinate and edge
 * flag, not the ones current when it was sent. Nothing reads those yet;
 * lighting and edge flags will need them carried.
 */
typedef struct MgrasGLPrimitiveWire {
    SGIGLContext *parent;
    uint32_t begin;
    uint32_t vertex_count;
    uint32_t vertex[MGRAS_GL_WIRE_HELD][4];
    uint32_t vertex_color[MGRAS_GL_WIRE_HELD][4];
} MgrasGLPrimitiveWire;

static int mgras_gl_primitive_pre_save(void *opaque)
{
    MgrasGLPrimitiveWire *w = opaque;
    SGIGLContext *gl = w->parent;

    w->begin = gl->in_begin ? mgras_gl_begin_of_mode(gl->begin_mode) : 0;
    w->vertex_count = gl->nheld;
    for (int i = 0; i < MGRAS_GL_WIRE_HELD; i++) {
        memcpy(w->vertex[i], &gl->held[i][SGI_GL_V_POSITION],
               sizeof(w->vertex[i]));
        memcpy(w->vertex_color[i], &gl->held[i][SGI_GL_V_COLOR],
               sizeof(w->vertex_color[i]));
    }
    return 0;
}

static int mgras_gl_primitive_post_load(void *opaque, int version_id)
{
    MgrasGLPrimitiveWire *w = opaque;
    SGIGLContext *gl = w->parent;

    if (w->vertex_count > MGRAS_GL_WIRE_HELD ||
        (w->begin && !mgras_gl_begin_token(w->begin, false))) {
        return -EINVAL;
    }
    gl->in_begin = w->begin != 0;
    gl->begin_mode = w->begin ? mgras_gl_mode_of_begin(w->begin) : 0;
    gl->nheld = w->vertex_count;
    for (int i = 0; i < MGRAS_GL_WIRE_HELD; i++) {
        mgras_gl_vertex_init(gl->held[i]);
        memcpy(&gl->held[i][SGI_GL_V_POSITION], w->vertex[i],
               sizeof(w->vertex[i]));
        memcpy(&gl->held[i][SGI_GL_V_COLOR], w->vertex_color[i],
               sizeof(w->vertex_color[i]));
    }
    return 0;
}

static const VMStateDescription vmstate_mgras_gl_primitive_wire = {
    .name = "tmp",
    .pre_save = mgras_gl_primitive_pre_save,
    .post_load = mgras_gl_primitive_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(begin, MgrasGLPrimitiveWire),
        VMSTATE_UINT32(vertex_count, MgrasGLPrimitiveWire),
        VMSTATE_UINT32_2DARRAY(vertex, MgrasGLPrimitiveWire,
                               MGRAS_GL_WIRE_HELD, 4),
        VMSTATE_UINT32_2DARRAY(vertex_color, MgrasGLPrimitiveWire,
                               MGRAS_GL_WIRE_HELD, 4),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_sgi_mgras_gl_primitive = {
    .name = TYPE_SGI_MGRAS "/gl-primitive",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_WITH_TMP(SGIGLContext, MgrasGLPrimitiveWire,
                         vmstate_mgras_gl_primitive_wire),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_sgi_mgras_gl_context = {
    .name = TYPE_SGI_MGRAS "/gl-context",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32_ARRAY(cur.viewport, SGIGLContext, 4),
        VMSTATE_SGI_GL_FLOATS(clear_color, SGIGLContext, 4, 0),
        VMSTATE_SGI_GL_FLOATS(cur_color, SGIGLContext, 4, 0),
        VMSTATE_UINT32(cur.shade_model, SGIGLContext),
        VMSTATE_WITH_TMP(SGIGLContext, MgrasGLPrimitiveWire,
                         vmstate_mgras_gl_primitive_wire),
        VMSTATE_UINT32(matrix_mode, SGIGLContext),
        VMSTATE_SGI_GL_FLOATS(cur.modelview, SGIGLContext, 16, 0),
        VMSTATE_SGI_GL_FLOATS(cur.projection, SGIGLContext, 16, 0),
        VMSTATE_UINT32(cur.depth_test, SGIGLContext),
        VMSTATE_UINT32(cur.depth_func, SGIGLContext),
        VMSTATE_UINT32(cur.depth_mask, SGIGLContext),
        VMSTATE_UINT32(clear_depth, SGIGLContext),
        VMSTATE_UINT32(cur.draw_buffers, SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_cmd_parser = {
    .name = TYPE_SGI_MGRAS "/cmd-parser",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cls, SGIMgrasCmdParser),
        VMSTATE_UINT32(reg, SGIMgrasCmdParser),
        VMSTATE_UINT32(words_remaining, SGIMgrasCmdParser),
        VMSTATE_UINT32_ARRAY(payload, SGIMgrasCmdParser,
                             SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS),
        VMSTATE_UINT32(payload_count, SGIMgrasCmdParser),
        VMSTATE_END_OF_LIST()
    }
};

/* The state each context gained in version 20, sent for every entry. */
const VMStateDescription vmstate_sgi_mgras_gl_context_v20 = {
    .name = TYPE_SGI_MGRAS "/gl-context-v20",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parser, SGIMgrasGLContext, 1,
                       vmstate_sgi_mgras_cmd_parser, SGIMgrasCmdParser),
        VMSTATE_END_OF_LIST()
    }
};

/* The matrix stacks each context gained in version 21. */
const VMStateDescription vmstate_sgi_mgras_gl_context_v21 = {
    .name = TYPE_SGI_MGRAS "/gl-context-v21",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_2DARRAY(mv_stack, SGIGLContext, MGRAS_GL_WIRE_STACK, 16,
                        0, vmstate_info_uint32, float),
        VMSTATE_2DARRAY(pr_stack, SGIGLContext, MGRAS_GL_WIRE_STACK, 16,
                        0, vmstate_info_uint32, float),
        VMSTATE_UINT32(mv_depth, SGIGLContext),
        VMSTATE_UINT32(pr_depth, SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/* The current normal and edge flag each context gained in version 22. */
const VMStateDescription vmstate_sgi_mgras_gl_context_v22 = {
    .name = TYPE_SGI_MGRAS "/gl-context-v22",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_GL_FLOATS(cur_normal, SGIGLContext, 3, 0),
        VMSTATE_UINT32(cur_edge, SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_raster = {
    .name = TYPE_SGI_MGRAS "/gl-context-raster",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_GL_FLOATS(raster_pos, SGIGLContext, 4, 0),
        VMSTATE_UINT32(raster_valid, SGIGLContext),
        VMSTATE_SINGLE(raster_distance, SGIGLContext, 0, vmstate_info_uint32,
                       float),
        VMSTATE_SGI_GL_FLOATS(raster_color, SGIGLContext, 4, 0),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Every context's raster position, sent only once one has left OpenGL's
 * initial one, so a stream from a guest that never sent glRasterPos stays
 * loadable by a binary that predates it. A destination that lacks this
 * section refuses a stream that carries it rather than lose the position.
 * A stream without it leaves the destination's initial positions, which
 * are what a source that predates it had: it never decoded glRasterPos.
 */
static bool mgras_gl_raster_needed(void *opaque)
{
    SGIMgrasState *s = opaque;
    g_autofree SGIGLContext *init = g_new(SGIGLContext, 1);

    sgi_gl_init(init, 0);
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIGLContext *gl = &s->gl[i];

        if (memcmp(gl->raster_pos, init->raster_pos,
                   sizeof(gl->raster_pos)) ||
            gl->raster_valid != init->raster_valid ||
            memcmp(&gl->raster_distance, &init->raster_distance,
                   sizeof(gl->raster_distance)) ||
            memcmp(gl->raster_color, init->raster_color,
                   sizeof(gl->raster_color))) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_raster = {
    .name = TYPE_SGI_MGRAS "/gl-raster",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_raster_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_raster,
                             SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_index = {
    .name = TYPE_SGI_MGRAS "/gl-context-index",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cur.index_bits, SGIGLContext),
        VMSTATE_UINT32(cur.index_mask, SGIGLContext),
        VMSTATE_SINGLE(cur_index, SGIGLContext, 0, vmstate_info_uint32,
                       float),
        VMSTATE_SINGLE(raster_index, SGIGLContext, 0, vmstate_info_uint32,
                       float),
        VMSTATE_SINGLE(clear_index, SGIGLContext, 0, vmstate_info_uint32,
                       float),
        VMSTATE_SINGLE(held[0][SGI_GL_V_INDEX], SGIGLContext, 0,
                       vmstate_info_uint32, float),
        VMSTATE_SINGLE(held[1][SGI_GL_V_INDEX], SGIGLContext, 0,
                       vmstate_info_uint32, float),
        VMSTATE_SINGLE(held[2][SGI_GL_V_INDEX], SGIGLContext, 0,
                       vmstate_info_uint32, float),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Every context's colour-index state, sent only once some context has
 * left what a model without colour-index mode keeps, OpenGL's initial
 * state and a held vertex's index of 1, so a guest that never used it
 * migrates to and from such a binary, and one that did is refused by it.
 * A stream without the section comes from a model that drew every
 * context in RGBA mode, which is what the destination's initial state
 * does.
 */
static bool mgras_gl_index_needed(void *opaque)
{
    SGIMgrasState *s = opaque;
    static const float one = 1.0f;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIGLContext *gl = &s->gl[i];

        if (gl->cur.index_bits || gl->cur.index_mask != UINT32_MAX ||
            memcmp(&gl->cur_index, &one, sizeof(one)) ||
            memcmp(&gl->raster_index, &one, sizeof(one)) ||
            gl->clear_index != 0 || signbit(gl->clear_index)) {
            return true;
        }
        for (uint32_t v = 0; v < MIN(gl->nheld, SGI_GL_HELD_VERTICES); v++) {
            if (memcmp(&gl->held[v][SGI_GL_V_INDEX], &one, sizeof(one))) {
                return true;
            }
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_index = {
    .name = TYPE_SGI_MGRAS "/gl-index",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_index_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_index,
                             SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * The parser inside a glBitmap image, live or set aside with a context,
 * keeps its place in the payload fields the stream already carries. A
 * binary without the image's class would read its words as another
 * packet's, so this empty section is sent while one is under way, for
 * such a binary to refuse.
 */
static bool mgras_gl_block_needed(void *opaque)
{
    SGIMgrasState *s = opaque;
    bool block = s->cmd_token_class == SGI_MGRAS_CMD_TOKEN_GL_BLOCK;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        block |= s->gl_context[i].parser.cls == SGI_MGRAS_CMD_TOKEN_GL_BLOCK;
    }
    return block;
}

const VMStateDescription vmstate_sgi_mgras_gl_block = {
    .name = TYPE_SGI_MGRAS "/gl-block",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_block_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    }
};

bool sgi_mgras_cmd_parser_valid(uint32_t cls, uint32_t reg,
                                uint32_t words_remaining,
                                uint32_t payload_count)
{
    if (reg >= sizeof_field(SGIMgrasState, rss) / sizeof(uint32_t) ||
        payload_count > SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS) {
        return false;
    }
    /* A block with an offset takes one word more than its length. */
    if (cls == SGI_MGRAS_CMD_TOKEN_DISCARD) {
        return words_remaining <= SGI_MGRAS_CMD_TOKEN_MAX_BLOCK_WORDS + 1;
    }
    if (cls == SGI_MGRAS_CMD_TOKEN_GL_BLOCK) {
        return reg == MGRAS_GL_BITMAP_OP &&
               payload_count == MGRAS_GL_BITMAP_WORDS &&
               words_remaining <= SGI_MGRAS_CMD_TOKEN_MAX_BLOCK_WORDS + 1;
    }
    return words_remaining <= SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS;
}

bool sgi_mgras_gl_post_load(SGIMgrasState *s)
{
    if (s->gl_current >= SGI_MGRAS_GL_CONTEXTS ||
        (s->gl_current && !s->gl_selected[s->gl_current])) {
        return false;
    }
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIMgrasCmdParser *p = &s->gl_context[i].parser;
        const SGIGLContext *gl = &s->gl[i];

        if (gl->nheld > SGI_GL_HELD_VERTICES ||
            gl->begin_mode > SGI_GL_POLYGON ||
            gl->mv_depth >= SGI_GL_STACK_DEPTH ||
            gl->pr_depth >= SGI_GL_STACK_DEPTH ||
            gl->raster_valid > 1 || gl->cur.index_bits > 32 ||
            !sgi_mgras_cmd_parser_valid(p->cls, p->reg, p->words_remaining,
                                        p->payload_count)) {
            return false;
        }
    }
    /* An entry's identity is its slot, which the stream carries. */
    for (int i = 1; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        s->gl[i].hw_id = s->gl_selected[i] ? s->gl_slot[i] : 0;
    }
    /*
     * Nor does it carry drawables: the loaded context's comes from the
     * image and banks it does carry, and every other is set again when
     * that context is next loaded.
     */
    mgras_gl_bind(s);
    return true;
}

/*
 * Store a direct-colour pixel at screen column x and row y counted from
 * the bottom, in each bank of @banks. The GL window's WID selects an XMAP
 * mode, 0x1408 for the probe's visual, whose colour map is never loaded;
 * an RGB pixel bypasses that map (sgi-mgras-display.c), which is what
 * makes the drawing visible. Bank 1 holds only direct colour in this
 * model, so it needs no format tag.
 */
static void mgras_gl_put(SGIMgrasState *s, unsigned int banks, int x, int y,
                         uint32_t rgb)
{
    unsigned int idx = (s->height - 1 - y) * s->width + x;

    if (banks & 1) {
        s->front[idx] = rgb;
        s->format[idx] = SGI_MGRAS_PIXEL_FORMAT_RGB;
    }
    if (banks & 2) {
        s->back[idx] = rgb;
    }
}

static uint32_t *mgras_gl_depth(SGIMgrasState *s, int x, int y)
{
    return &s->depth[(s->height - 1 - y) * s->width + x];
}

/* target is the bank mask mgras_gl_bind() gave the drawable. */
static void mgras_gl_store_colour(void *opaque, SGIColourTarget target,
                                  int x, int y, uint32_t colour)
{
    mgras_gl_put(opaque, target, x, y, colour);
}

/*
 * A colour index into bank 0 as a CI8 pixel, which scans out through the
 * colour map the window's display ID selects, the X server's own CI8 form
 * (mgras-raster-display.md, "Scanout"). The write mask keeps the stored
 * index's bits where it is clear. Bank 1 holds only direct colour in this
 * model, so an index for it is dropped; mgras_gl_check_index() logs that.
 * A context deeper than eight bits keeps its low eight, logged at its
 * setup.
 *
 * A mask with none of the eight bits set leaves the pixel as it was
 * (OpenGL 1.2.1, 4.2.2), whatever its format. Approximated: the model
 * tags each pixel with one format rather than keeping planes, so a
 * partial mask over a direct-colour pixel keeps that pixel's low byte
 * as the index's other bits and makes it CI8. The discriminating test is
 * gl-colour-index-zero-mask-keeps-the-pixel.
 */
static void mgras_gl_store_index(void *opaque, SGIColourTarget target,
                                 int x, int y, uint32_t index, uint32_t mask)
{
    SGIMgrasState *s = opaque;
    unsigned int idx = (s->height - 1 - y) * s->width + x;

    if ((target & 1) && (mask & 0xff)) {
        s->front[idx] = ((s->front[idx] & ~mask) | (index & mask)) & 0xff;
        s->format[idx] = SGI_MGRAS_PIXEL_FORMAT_CI8;
    }
}

/* Log colour-index work bound for bank 1, which the model cannot hold. */
static void mgras_gl_check_index(uint32_t index_bits,
                                 const SGIColourTarget *targets, uint32_t n)
{
    for (uint32_t i = 0; index_bits && i < n; i++) {
        if (targets[i] & 2) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: colour-index drawing into "
                          "pixel-memory bank 1 is not modelled; dropped\n");
            return;
        }
    }
}

static uint32_t mgras_gl_load_depth(void *opaque, int x, int y)
{
    return *mgras_gl_depth(opaque, x, y);
}

static void mgras_gl_store_depth(void *opaque, int x, int y, uint32_t depth)
{
    *mgras_gl_depth(opaque, x, y) = depth;
}

/*
 * The rules MGRAS's GL drawing follows, each implemented and tested:
 *
 * - Window coordinates snap to 1/256 pixel. The Octane Technical Report
 *   (section 4.2.3) gives the RE4 eight bits of subpixel precision; how
 *   the GE rounds to them is not published, so rounding to nearest, half
 *   away from zero, is the model's choice. The discriminating test is
 *   gl-triangle-snaps-to-the-nearest-subpixel.
 * - The depth plane holds 24 bits, the Z width the same report gives the
 *   RE4. A fragment's window depth rounds the same way, the model's
 *   choice; the discriminating test is gl-depth-rounds-to-24-bits.
 * - A colour channel is stored at eight bits. The 12-bit-per-channel
 *   visual the probe uses (visual 0x30, 12 red bits) is stored at the
 *   eight bits a direct-colour pixel holds in this model; 0.0 and 1.0 are
 *   exact either way, and how the board reduces an interpolated 12-bit
 *   value is not published. The discriminating test is
 *   gl-smooth-rounds-to-the-nearest-step.
 * - A shared edge's centres belong to the left and top edges, rows
 *   counted up from the bottom, the model's choice; the discriminating
 *   test is gl-triangle-shared-edge-centres-drawn-once.
 * - Attributes interpolate in double arithmetic, not the GE's, which is
 *   not published; a result that differs only in rounding is within the
 *   probe's margins.
 * - Lines cover the pixels the diamond-exit rule gives on the subpixel
 *   grid, with no endpoint perturbation (gl-reference/lines and
 *   gl-line-leaves-out-its-last-pixel).
 * - A primitive with a vertex at w <= 0 is dropped, not clipped
 *   (gl-triangle-with-negative-w-is-dropped).
 */
static const SGIRasterRules mgras_gl_raster_rules = {
    .subpixel_bits = 8,
    .color_bits = 8,
    .depth_bits = 24,
    .rounding = SGI_ROUND_HALF_AWAY,
    .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
    .arith = SGI_ARITH_DOUBLE,
    .line_rule = SGI_LINE_COVERAGE_DOUBLE,
    .evidence = "MGRAS: 8 subpixel and 24 depth bits from the Octane "
                "Technical Report 4.2.3; the rest the model's choices, "
                "tested",
};

static const SGIRasterTarget mgras_gl_target = {
    .store_colour = mgras_gl_store_colour,
    .store_index = mgras_gl_store_index,
    .load_depth = mgras_gl_load_depth,
    .store_depth = mgras_gl_store_depth,
};

/* A GL context's draw, rasterized into the banks and the depth plane. */
static void mgras_gl_draw(void *opaque, const SGIGLDraw *d)
{
    SGIMgrasState *s = opaque;
    SGIRasterTarget t = mgras_gl_target;
    SGIColourTarget targets[SGI_GL_MAX_TARGETS];

    t.opaque = s;
    mgras_gl_check_index(d->state->index_bits, targets,
                         sgi_gl_draw_targets(d->state, targets));
    sgi_soft_draw(&mgras_gl_raster_rules, &t, d);
    s->dirty = true;
}

/* A GL context's clear or bitmap. */
static void mgras_gl_raster(void *opaque, const SGIRasterOp *op)
{
    SGIMgrasState *s = opaque;
    SGIRasterTarget t = mgras_gl_target;

    t.opaque = s;
    mgras_gl_check_index(op->kind == SGI_RASTER_BITMAP ?
                         op->bitmap.state->index_bits : op->index_bits,
                         op->targets, op->ntargets);
    sgi_soft_raster(&mgras_gl_raster_rules, &t, op);
    s->dirty = true;
}

/*
 * The client writes the opener first when it sets up a new context
 * (mgras-gl-commands.md, "Context setup opener"), and the setup that
 * follows sends no matrix, so the context must already hold the OpenGL
 * initial state then. The model restores it in the loaded context's entry
 * alone; the other contexts and the banks the image gave are the board's.
 * The discriminating tests are gl-context-setup-restores-initial-state and
 * gl-context-opener-resets-only-its-context.
 */
static void mgras_gl_context_start(SGIMgrasState *s)
{
    SGIGLContext *gl = mgras_gl(s);

    sgi_gl_init(gl, gl->hw_id);
    mgras_gl_bind(s);
}

/*
 * No MGRAS command is a glFinish: the client's glFinish is a query and a
 * status poll (mgras-gl-commands.md), which the HQ answers itself.
 */
static SGIGLSink mgras_gl_sink(SGIMgrasState *s)
{
    return (SGIGLSink) {
        .draw = mgras_gl_draw,
        .raster = mgras_gl_raster,
        .opaque = s,
    };
}

static void mgras_gl_command(SGIMgrasState *s, SGIGLOp op,
                             const uint32_t *args, unsigned nargs)
{
    SGIGLSink sink = mgras_gl_sink(s);
    SGIGLCmd cmd = { .op = op, .nargs = nargs, .args = args };

    sgi_gl_command(mgras_gl(s), &sink, &cmd);
}

/* A colour index, as the float the shared context keeps. */
static void mgras_gl_index(SGIMgrasState *s, float index)
{
    uint32_t word;

    memcpy(&word, &index, sizeof(word));
    mgras_gl_command(s, SGI_GL_OP_INDEXF, &word, 1);
}

/*
 * Where glBitmap's image is while its words arrive: the parser keeps the
 * 0x9420 packet's eight words in its payload, and these slots after them,
 * so a context switch inside the image carries them with the context's
 * parser position.
 */
#define MGRAS_GL_BITMAP_PLACED   8
#define MGRAS_GL_BITMAP_X        9
#define MGRAS_GL_BITMAP_Y        10
#define MGRAS_GL_BITMAP_SKIP     11
#define MGRAS_GL_BITMAP_NEXT     12
QEMU_BUILD_BUG_ON(MGRAS_GL_BITMAP_NEXT >= SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS);

static void mgras_gl_bitmap(SGIMgrasState *s, const uint32_t *payload)
{
    uint32_t *slot = s->cmd_token_payload;
    int32_t width = payload[1], height = payload[2];
    float f[4];
    SGIGLBitmap b;
    bool placed;

    if ((payload[0] & MGRAS_GL_BITMAP_LSB_FIRST) ||
        (payload[0] & 0xf) != 0xf) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: glBitmap flags 0x%x are not "
                      "the form the client sends; drawn as that form\n",
                      payload[0]);
    }
    if (width < 0 || height < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: glBitmap of %d by %d "
                      "ignored\n", width, height);
        width = height = 0;
    }
    memcpy(f, &payload[3], sizeof(f));
    placed = sgi_gl_bitmap_place(mgras_gl(s), width, height, f[0], f[1],
                                 f[2], f[3], &b);
    slot[MGRAS_GL_BITMAP_PLACED] = placed && width && height;
    slot[MGRAS_GL_BITMAP_X] = placed ? b.x : 0;
    slot[MGRAS_GL_BITMAP_Y] = placed ? b.y : 0;
    slot[MGRAS_GL_BITMAP_SKIP] = 0;
    slot[MGRAS_GL_BITMAP_NEXT] = 0;
}

bool sgi_mgras_gl_block(SGIMgrasState *s, uint32_t header)
{
    const uint32_t *slot = s->cmd_token_payload;
    uint32_t offset = (header & SGI_MGRAS_CMD_BLOCK_OFFSET) >> 22;
    uint32_t bytes = header & SGI_MGRAS_CMD_BLOCK_BYTES;

    if (s->cmd_token_class != 0 || s->cmd_token_reg != MGRAS_GL_BITMAP_OP ||
        s->cmd_token_payload_count != MGRAS_GL_BITMAP_WORDS) {
        return false;
    }
    if (bytes != slot[7] * sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: glBitmap image of %u "
                      "bytes after a packet giving %u words\n", bytes,
                      slot[7]);
    }
    s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_GL_BLOCK;
    s->cmd_token_words_remaining = DIV_ROUND_UP(offset + bytes,
                                                sizeof(uint32_t));
    s->cmd_token_payload[MGRAS_GL_BITMAP_SKIP] = offset;
    if (!s->cmd_token_words_remaining) {
        s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_DISCARD;
    }
    return true;
}

void sgi_mgras_gl_block_word(SGIMgrasState *s, uint32_t word)
{
    uint32_t *slot = s->cmd_token_payload;
    uint32_t first = slot[MGRAS_GL_BITMAP_NEXT], n = 0;
    uint8_t bytes[4];

    for (int i = 0; i < 4; i++) {
        if (slot[MGRAS_GL_BITMAP_SKIP]) {
            slot[MGRAS_GL_BITMAP_SKIP]--;
            continue;
        }
        bytes[n++] = word >> (24 - 8 * i);
    }
    slot[MGRAS_GL_BITMAP_NEXT] += n;
    if (slot[MGRAS_GL_BITMAP_PLACED] && n) {
        SGIGLSink sink = mgras_gl_sink(s);
        SGIGLBitmap b = {
            .x = slot[MGRAS_GL_BITMAP_X],
            .y = slot[MGRAS_GL_BITMAP_Y],
            .width = slot[1],
            .height = slot[2],
        };

        sgi_gl_bitmap_bytes(mgras_gl(s), &sink, &b, first, bytes, n);
    }
    if (!s->cmd_token_words_remaining) {
        s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_DISCARD;
    }
}

void sgi_mgras_gl_execute(SGIMgrasState *s, uint32_t header,
                          const uint32_t *payload, uint32_t words)
{
    uint32_t word[4];

    if (words != (header & 0xff) / sizeof(uint32_t)) {
        return;
    }
    if (mgras_gl_begin_token(header, false)) {
        word[0] = mgras_gl_mode_of_begin(header);
        mgras_gl_command(s, SGI_GL_OP_BEGIN, word, 1);
        return;
    }
    if (mgras_gl_begin_token(header, true)) {
        /* Any end token ends whichever primitive is open. */
        mgras_gl_command(s, SGI_GL_OP_END, payload, 0);
        return;
    }
    switch (header) {
    case MGRAS_GL_CONTEXT_OPENER:
        mgras_gl_context_start(s);
        return;
    case MGRAS_GL_VIEWPORT:
        /* The client has already clamped the size (__glMgrim_Viewport). */
        mgras_gl_command(s, SGI_GL_OP_VIEWPORT, payload, 4);
        return;
    case MGRAS_GL_CLEAR_COLOR:
        mgras_gl_command(s, SGI_GL_OP_CLEAR_COLOR, payload, 4);
        return;
    case MGRAS_GL_CLEAR_COLOR_BUF:
        word[0] = SGI_GL_COLOR_BUFFER_BIT;
        mgras_gl_command(s, SGI_GL_OP_CLEAR, word, 1);
        return;
    case MGRAS_GL_CLEAR_DEPTH:
        /*
         * The word is already scaled to the depth plane's 24 bits
         * (mgras-gl-commands.md), the form the shared context keeps.
         */
        mgras_gl_command(s, SGI_GL_OP_CLEAR_DEPTH, payload, 1);
        return;
    case MGRAS_GL_CLEAR_DEPTH_BUF:
        word[0] = SGI_GL_DEPTH_BUFFER_BIT;
        mgras_gl_command(s, SGI_GL_OP_CLEAR, word, 1);
        return;
    case MGRAS_GL_SHADE_MODEL:
        mgras_gl_command(s, SGI_GL_OP_SHADE_MODEL, payload, 1);
        return;
    case MGRAS_GL_MATRIX_MODE:
        mgras_gl_command(s, SGI_GL_OP_MATRIX_MODE, payload, 1);
        return;
    case MGRAS_GL_LOAD_MATRIX:
        mgras_gl_command(s, SGI_GL_OP_LOAD_MATRIX, payload, 16);
        return;
    case MGRAS_GL_LOAD_IDENTITY:
        mgras_gl_command(s, SGI_GL_OP_LOAD_IDENTITY, payload, 0);
        return;
    case MGRAS_GL_MULT_MATRIX:
        mgras_gl_command(s, SGI_GL_OP_MULT_MATRIX, payload, 16);
        return;
    case MGRAS_GL_FRUSTUM:
        mgras_gl_command(s, SGI_GL_OP_FRUSTUM, payload, 6);
        return;
    case MGRAS_GL_ORTHO:
        mgras_gl_command(s, SGI_GL_OP_ORTHO, payload, 6);
        return;
    case MGRAS_GL_PUSH_MATRIX:
        mgras_gl_command(s, SGI_GL_OP_PUSH_MATRIX, payload, 0);
        return;
    case MGRAS_GL_POP_MATRIX:
        mgras_gl_command(s, SGI_GL_OP_POP_MATRIX, payload, 0);
        return;
    case MGRAS_GL_TRANSLATE:
        mgras_gl_command(s, SGI_GL_OP_TRANSLATE, payload, 3);
        return;
    case MGRAS_GL_SCALE:
        mgras_gl_command(s, SGI_GL_OP_SCALE, payload, 3);
        return;
    case MGRAS_GL_ROTATE:
        mgras_gl_command(s, SGI_GL_OP_ROTATE, payload, 4);
        return;
    case MGRAS_GL_DEPTH_TEST:
        /*
         * glEnable(GL_DEPTH_TEST) sent 0x6604 with 1 and no other word in
         * the probe's depth case, and every context setup sends it with 0
         * (run irix-20260927T172059-fdbada).
         */
        word[0] = SGI_GL_DEPTH_TEST;
        mgras_gl_command(s, payload[0] ? SGI_GL_OP_ENABLE :
                         SGI_GL_OP_DISABLE, word, 1);
        return;
    case MGRAS_GL_DEPTH_FUNC:
        word[0] = SGI_GL_NEVER_ENUM + (payload[0] & 7);
        mgras_gl_command(s, SGI_GL_OP_DEPTH_FUNC, word, 1);
        return;
    case MGRAS_GL_DEPTH_MASK:
        mgras_gl_command(s, SGI_GL_OP_DEPTH_MASK, payload, 1);
        return;
    case MGRAS_GL_DRAW_BUFFER:
        mgras_gl_command(s, SGI_GL_OP_DRAW_BUFFERS, payload, 1);
        return;
    case MGRAS_GL_BANKS:
        if (payload[0] != payload[1]) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: bank token words %u and "
                          "%u differ; the model uses the first\n",
                          payload[0], payload[1]);
        }
        s->gl_banks[0] = payload[0];
        s->gl_banks[1] = payload[1];
        mgras_gl_bind(s);
        return;
    case MGRAS_GL_COLOR3F:
        mgras_gl_command(s, SGI_GL_OP_COLOR3F, payload, 3);
        return;
    case MGRAS_GL_COLOR4F:
        mgras_gl_command(s, SGI_GL_OP_COLOR4F, payload, 4);
        return;
    case MGRAS_GL_COLOR3UB:
        mgras_gl_color_bytes(word, payload[0]);
        mgras_gl_command(s, SGI_GL_OP_COLOR3UB, word, 3);
        return;
    case MGRAS_GL_COLOR4UB:
        mgras_gl_color_bytes(word, payload[0]);
        mgras_gl_command(s, SGI_GL_OP_COLOR4UB, word, 4);
        return;
    case MGRAS_GL_NORMAL3F:
        /* Kept for the 0xa104 query only; lighting is not modelled. */
        mgras_gl_command(s, SGI_GL_OP_NORMAL3F, payload, 3);
        return;
    case MGRAS_GL_EDGE_FLAG_TRUE:
    case MGRAS_GL_EDGE_FLAG_FALSE:
        /* Kept for the 0xa104 query; polygon edge modes are not modelled. */
        word[0] = header == MGRAS_GL_EDGE_FLAG_TRUE;
        mgras_gl_command(s, SGI_GL_OP_EDGE_FLAG, word, 1);
        return;
    case MGRAS_GL_VERTEX2F:
        mgras_gl_command(s, SGI_GL_OP_VERTEX2F, payload, 2);
        return;
    case MGRAS_GL_VERTEX3F:
        mgras_gl_command(s, SGI_GL_OP_VERTEX3F, payload, 3);
        return;
    case MGRAS_GL_VERTEX4F:
        mgras_gl_command(s, SGI_GL_OP_VERTEX4F, payload, 4);
        return;
    case MGRAS_GL_VERTEX2I:
        mgras_gl_command(s, SGI_GL_OP_VERTEX2I, payload, 2);
        return;
    case MGRAS_GL_RASTER_POS2F:
        mgras_gl_command(s, SGI_GL_OP_RASTER_POS2F, payload, 2);
        return;
    case MGRAS_GL_RASTER_POS3F:
        mgras_gl_command(s, SGI_GL_OP_RASTER_POS3F, payload, 3);
        return;
    case MGRAS_GL_FLUSH:
        /* Approximated: every command has already run to completion. */
        mgras_gl_command(s, SGI_GL_OP_FLUSH, payload, 0);
        return;
    case MGRAS_GL_INIT_CI:
        if (!payload[0]) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: colour-index context "
                          "of 0 bits ignored\n");
            return;
        }
        if (payload[0] > 8) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: colour-index context of "
                          "%u bits is stored as its low 8 bits\n",
                          payload[0]);
        }
        sgi_gl_set_index_mode(mgras_gl(s), payload[0]);
        return;
    case MGRAS_GL_INIT_RGB:
        sgi_gl_set_index_mode(mgras_gl(s), 0);
        return;
    case MGRAS_GL_INDEXS:
        mgras_gl_index(s, (int16_t)(payload[0] >> 16));
        return;
    case MGRAS_GL_INDEXI:
        mgras_gl_index(s, (int32_t)payload[0]);
        return;
    case MGRAS_GL_INDEXF:
        mgras_gl_command(s, SGI_GL_OP_INDEXF, payload, 1);
        return;
    case MGRAS_GL_INDEX_MASK:
        mgras_gl_command(s, SGI_GL_OP_INDEX_MASK, payload, 1);
        return;
    case MGRAS_GL_CLEAR_INDEX:
        mgras_gl_command(s, SGI_GL_OP_CLEAR_INDEX, payload, 1);
        return;
    case MGRAS_GL_BITMAP:
        mgras_gl_bitmap(s, payload);
        return;
    case MGRAS_GL_MOVE_RASTER: {
        SGIGLBitmap b;
        float f[2];

        memcpy(f, payload, sizeof(f));
        sgi_gl_bitmap_place(mgras_gl(s), 0, 0, 0, 0, f[0], f[1], &b);
        return;
    }
    default:
        g_assert_not_reached();
    }
}
