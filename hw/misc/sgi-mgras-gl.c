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
 * What it preserves: the pixels that colour and depth clears and
 * untextured points, lines, triangles, strips, fans, quads and polygons,
 * at size and width 1, flat or smooth shaded, transformed by the
 * modelview and projection matrices and their stacks and depth tested,
 * leave in the window's front or back buffer, and the main-port framing,
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

/* Class-0 opcodes, bits 15:8 of the header, with their byte lengths. */
#define MGRAS_GL_CONTEXT_OPENER  0x00000600
/*
 * glBegin and glEnd: 0x1600 to 0x1f00 begin GL_POINTS (0) to GL_POLYGON
 * (9) in the order of their enumerants, and each mode's end token is
 * 0x0a00 above its begin token (mgras-gl-commands.md, "Primitives").
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
#define MGRAS_GL_EDGE_FLAG_TRUE  0x00000f00
#define MGRAS_GL_EDGE_FLAG_FALSE 0x00001000

#define MGRAS_GL_FLAT            0x1d00
#define MGRAS_GL_SMOOTH          0x1d01
#define MGRAS_GL_MODELVIEW       0x1700
#define MGRAS_GL_PROJECTION      0x1701

/* glDepthFunc's word is the function minus GL_NEVER (OpenGL 4.1.5). */
enum {
    MGRAS_GL_NEVER, MGRAS_GL_LESS, MGRAS_GL_EQUAL, MGRAS_GL_LEQUAL,
    MGRAS_GL_GREATER, MGRAS_GL_NOTEQUAL, MGRAS_GL_GEQUAL, MGRAS_GL_ALWAYS,
};

/*
 * The draw-buffer triple's first word: 1 in the context setup of the
 * probe's single-buffered visual, 2 in that of its double-buffered one
 * (run irix-20260927T172059-fdbada), where OpenGL's default draw buffer is
 * the front and the back buffer respectively. Reading it as a bit mask of
 * the front and back buffers is inferred from those two values; the other
 * bits are not modelled.
 */
#define MGRAS_GL_DRAW_FRONT      0x1
#define MGRAS_GL_DRAW_BACK       0x2

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

/*
 * Window coordinates are snapped to 1/256 pixel before rasterization. The
 * Octane Technical Report (section 4.2.3) gives the RE4 eight bits of
 * subpixel precision; how the GE rounds to them is not published, so
 * rounding to nearest is the model's choice. The discriminating test is
 * gl-triangle-snaps-to-the-nearest-subpixel.
 */
#define MGRAS_GL_SUBPIXEL_BITS   8

/*
 * The depth plane holds 24 bits, the Z width the Octane Technical Report
 * (section 4.2.3) gives the RE4, and glClearDepth's word is already
 * scaled to it (mgras-gl-commands.md). A fragment's window depth is
 * rounded to the nearest step, the model's choice; the discriminating
 * test is gl-depth-rounds-to-24-bits.
 */
#define MGRAS_GL_DEPTH_MAX       0xffffff

/* Screen columns, and rows counted up from the bottom of the screen. */
typedef struct MgrasGLWindow {
    int ox, oy;           /* the window's lower-left pixel */
    int x0, y0, x1, y1;   /* the pixels it owns, inclusive */
} MgrasGLWindow;

/* A vertex after transformation, ready to rasterize. */
typedef struct MgrasGLVertex {
    int64_t x, y;         /* window position in 1/256 pixel */
    double z;             /* window depth; [0, 1] inside the view volume */
    double inv_w;         /* 1 / clip w, for perspective-correct colour */
    double rgb[3];
} MgrasGLVertex;

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
        return true;
    default:
        return false;
    }
}

static float mgras_gl_float(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t mgras_gl_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/*
 * glColor3ub's and glColor4ub's bytes in one word, the first component in
 * bits 31:24: colorbars' bars (run irix-20260928T004550-1040f3, from seq
 * 26371573) are 0x87878700, 0x87870000, 0x00878700, 0x00870000,
 * 0x87008700 and 0x87000000, white, yellow, cyan, green, magenta and red.
 * Alpha in bits 7:0 follows from that component order; noof's constant
 * glColor4ub word 0x00000060 is consistent with it but cannot fix it
 * alone. A byte c is c / 255 (OpenGL 1.2.1, table 2.6). glColor3ub's
 * alpha is 1.
 */
static void mgras_gl_color_bytes(uint32_t color[4], uint32_t word,
                                 bool alpha)
{
    for (int i = 0; i < 4; i++) {
        uint32_t c = (word >> (24 - 8 * i)) & 0xff;

        color[i] = mgras_gl_bits(i == 3 && !alpha ? 1.0f : c / 255.0f);
    }
}

static void mgras_gl_identity(uint32_t m[16])
{
    for (int i = 0; i < 16; i++) {
        m[i] = mgras_gl_bits(i % 5 == 0);
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

static SGIMgrasGLContext *mgras_gl(SGIMgrasState *s)
{
    return &s->gl_context[s->gl_current];
}

/* OpenGL 1.2.1 initial state (2.7, 2.10, 2.13.7, 4.1.5, 4.2.1-4.2.3). */
static void mgras_gl_context_init(SGIMgrasGLContext *gl)
{
    static const float one = 1.0f;
    uint32_t one_bits;

    memcpy(&one_bits, &one, sizeof(one_bits));
    memset(gl, 0, sizeof(*gl));
    for (int i = 0; i < 4; i++) {
        gl->color[i] = one_bits;
    }
    gl->shade_model = MGRAS_GL_SMOOTH;
    gl->matrix_mode = MGRAS_GL_MODELVIEW;
    mgras_gl_identity(gl->modelview);
    mgras_gl_identity(gl->projection);
    gl->depth_func = MGRAS_GL_LESS;
    gl->depth_mask = 1;
    gl->clear_depth = MGRAS_GL_DEPTH_MAX;
    gl->draw_buffers = MGRAS_GL_DRAW_FRONT;
    gl->normal[2] = one_bits;
    gl->edge_flag = 1;
}

void sgi_mgras_gl_reset(SGIMgrasState *s)
{
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        mgras_gl_context_init(&s->gl_context[i]);
    }
    memset(s->gl_slot, 0, sizeof(s->gl_slot));
    memset(s->gl_selected, 0, sizeof(s->gl_selected));
    s->gl_selections = 0;
    s->gl_current = 0;
    s->gl_ge_port_logged = false;
    /* The value every captured GL context image restores. */
    s->gl_banks[0] = s->gl_banks[1] = 1;
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
    mgras_gl_context_init(&s->gl_context[free]);
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

    sgi_mgras_cmd_parser_save(s, &mgras_gl(s)->parser);
    s->gl_banks[0] = s->context[MGRAS_GL_IMAGE_BANKS];
    s->gl_banks[1] = s->context[MGRAS_GL_IMAGE_BANKS + 1];
    s->gl_ge_port_logged = false;
    if ((slot & MGRAS_GL_NO_SLOT) || slot == MGRAS_GL_DEFAULT_SLOT) {
        s->gl_current = 0;
    } else {
        s->gl_current = mgras_gl_lookup(s, slot);
        s->gl_selected[s->gl_current] = ++s->gl_selections;
    }
    sgi_mgras_cmd_parser_load(s, &mgras_gl(s)->parser);
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
 */
uint32_t sgi_mgras_gl_attribute_answer(SGIMgrasState *s, uint32_t index)
{
    SGIMgrasGLContext *gl = mgras_gl(s);

    if (index >= 4 && index <= 7) {
        return gl->color[index - 4];
    }
    if (index >= 8 && index <= 10) {
        return gl->normal[index - 8];
    }
    if (index >= 11 && index <= 14) {
        return index == 14 ? mgras_gl_bits(1.0f) : 0;
    }
    if (index == 15) {
        return gl->edge_flag ? 0x01c00000 : 0;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL current-attribute query index "
                  "%u answered with zero\n", index);
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

const VMStateDescription vmstate_sgi_mgras_gl_context = {
    .name = TYPE_SGI_MGRAS "/gl-context",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32_ARRAY(viewport, SGIMgrasGLContext, 4),
        VMSTATE_UINT32_ARRAY(clear_color, SGIMgrasGLContext, 4),
        VMSTATE_UINT32_ARRAY(color, SGIMgrasGLContext, 4),
        VMSTATE_UINT32(shade_model, SGIMgrasGLContext),
        VMSTATE_UINT32(begin, SGIMgrasGLContext),
        VMSTATE_UINT32(vertex_count, SGIMgrasGLContext),
        VMSTATE_UINT32_2DARRAY(vertex, SGIMgrasGLContext, 3, 4),
        VMSTATE_UINT32_2DARRAY(vertex_color, SGIMgrasGLContext, 3, 4),
        VMSTATE_UINT32(matrix_mode, SGIMgrasGLContext),
        VMSTATE_UINT32_ARRAY(modelview, SGIMgrasGLContext, 16),
        VMSTATE_UINT32_ARRAY(projection, SGIMgrasGLContext, 16),
        VMSTATE_UINT32(depth_test, SGIMgrasGLContext),
        VMSTATE_UINT32(depth_func, SGIMgrasGLContext),
        VMSTATE_UINT32(depth_mask, SGIMgrasGLContext),
        VMSTATE_UINT32(clear_depth, SGIMgrasGLContext),
        VMSTATE_UINT32(draw_buffers, SGIMgrasGLContext),
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
        VMSTATE_UINT32_2DARRAY(modelview_stack, SGIMgrasGLContext,
                               SGI_MGRAS_GL_STACK_DEPTH - 1, 16),
        VMSTATE_UINT32_2DARRAY(projection_stack, SGIMgrasGLContext,
                               SGI_MGRAS_GL_STACK_DEPTH - 1, 16),
        VMSTATE_UINT32(modelview_depth, SGIMgrasGLContext),
        VMSTATE_UINT32(projection_depth, SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/* The current normal and edge flag each context gained in version 22. */
const VMStateDescription vmstate_sgi_mgras_gl_context_v22 = {
    .name = TYPE_SGI_MGRAS "/gl-context-v22",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(normal, SGIMgrasGLContext, 3),
        VMSTATE_UINT32(edge_flag, SGIMgrasGLContext),
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
    if (cls == SGI_MGRAS_CMD_TOKEN_DISCARD) {
        return words_remaining <= SGI_MGRAS_CMD_TOKEN_MAX_BLOCK_WORDS;
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

        if (s->gl_context[i].vertex_count >
            ARRAY_SIZE(s->gl_context[i].vertex) ||
            (s->gl_context[i].begin &&
             !mgras_gl_begin_token(s->gl_context[i].begin, false)) ||
            s->gl_context[i].modelview_depth >= SGI_MGRAS_GL_STACK_DEPTH ||
            s->gl_context[i].projection_depth >= SGI_MGRAS_GL_STACK_DEPTH ||
            !sgi_mgras_cmd_parser_valid(p->cls, p->reg, p->words_remaining,
                                        p->payload_count)) {
            return false;
        }
    }
    return true;
}

/*
 * A component clamped to [0, 1] and rounded to the nearest of eight bits
 * (OpenGL 1.2.1, 2.13.9). The 12-bit-per-channel visual the probe uses
 * (visual 0x30, 12 red bits) is stored at the eight bits a direct-colour
 * pixel holds in this model; 0.0 and 1.0 are exact either way, and how
 * the board reduces an interpolated 12-bit value is not published. The
 * discriminating test is gl-smooth-rounds-to-the-nearest-step.
 */
static uint32_t mgras_gl_channel(double c)
{
    if (!(c > 0.0)) {
        return 0;
    }
    if (c >= 1.0) {
        return 0xff;
    }
    return (uint32_t)(c * 255.0 + 0.5);
}

static bool mgras_gl_window(SGIMgrasState *s, MgrasGLWindow *win)
{
    uint32_t origin = s->context[MGRAS_GL_IMAGE_ORIGIN];
    uint32_t clip_x = s->context[MGRAS_GL_IMAGE_CLIP_X];
    uint32_t clip_y = s->context[MGRAS_GL_IMAGE_CLIP_Y];

    if (s->context[MGRAS_GL_IMAGE_CLIP_MODE] != MGRAS_GL_CLIP_ON) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL drawing with context image "
                      "word 3 0x%x; the window rectangle is not modelled "
                      "without 0x11\n",
                      s->context[MGRAS_GL_IMAGE_CLIP_MODE]);
        return false;
    }
    win->ox = origin & 0xffff;
    win->oy = origin >> 16;
    win->x0 = clip_x >> 16;
    win->y0 = clip_y >> 16;
    win->x1 = MIN(clip_x & 0xffff, s->width - 1);
    win->y1 = MIN(clip_y & 0xffff, s->height - 1);
    return win->x0 <= win->x1 && win->y0 <= win->y1;
}

/*
 * The pixel-memory banks the draw buffers select, as a mask of bank 0 and
 * bank 1. Bank 0 is the model's front[] plane, the one the X server draws
 * in; bank 1 is back[].
 */
static unsigned int mgras_gl_draw_banks(SGIMgrasState *s)
{
    SGIMgrasGLContext *gl = mgras_gl(s);
    unsigned int back = s->gl_banks[0] & 1;
    unsigned int banks = 0;

    if (gl->draw_buffers & ~(MGRAS_GL_DRAW_FRONT | MGRAS_GL_DRAW_BACK)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL draw buffers 0x%x beyond "
                      "the front and back buffers ignored\n",
                      gl->draw_buffers);
    }
    if (gl->draw_buffers & MGRAS_GL_DRAW_FRONT) {
        banks |= 1u << (back ^ 1);
    }
    if (gl->draw_buffers & MGRAS_GL_DRAW_BACK) {
        banks |= 1u << back;
    }
    return banks;
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

/*
 * OpenGL 1.2.1, 4.2.3: a clear passes only the pixel ownership, scissor
 * and dither steps, and the viewport does not bound it. The model takes
 * ownership as the image's window rectangle, which is the whole window
 * when nothing covers it; the scissor test and dithering are not
 * modelled. A depth clear writes only when the depth mask allows.
 */
static void mgras_gl_clear(SGIMgrasState *s, bool depth)
{
    SGIMgrasGLContext *gl = mgras_gl(s);
    MgrasGLWindow win;
    unsigned int banks = mgras_gl_draw_banks(s);
    uint32_t rgb = 0;

    if (!mgras_gl_window(s, &win)) {
        return;
    }
    if (depth && !gl->depth_mask) {
        return;
    }
    if (!depth) {
        rgb = (mgras_gl_channel(mgras_gl_float(gl->clear_color[0])) << 16) |
              (mgras_gl_channel(mgras_gl_float(gl->clear_color[1])) << 8) |
              mgras_gl_channel(mgras_gl_float(gl->clear_color[2]));
    }
    for (int y = win.y0; y <= win.y1; y++) {
        for (int x = win.x0; x <= win.x1; x++) {
            if (depth) {
                *mgras_gl_depth(s, x, y) =
                    gl->clear_depth & MGRAS_GL_DEPTH_MAX;
            } else {
                mgras_gl_put(s, banks, x, y, rgb);
            }
        }
    }
    s->dirty = true;
}

/*
 * m = m * b, both column-major as glLoadMatrixf lays them out (2.10.2).
 * The product is formed in double precision and stored at single, the
 * precision the client sends; the GE's own arithmetic is not published.
 */
static void mgras_gl_multiply(uint32_t m[16], const double b[16])
{
    uint32_t r[16];

    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            double sum = 0;

            for (int k = 0; k < 4; k++) {
                sum += (double)mgras_gl_float(m[k * 4 + row]) *
                       b[col * 4 + k];
            }
            r[col * 4 + row] = mgras_gl_bits(sum);
        }
    }
    memcpy(m, r, sizeof(r));
}

static uint32_t *mgras_gl_current_matrix(SGIMgrasState *s)
{
    SGIMgrasGLContext *gl = mgras_gl(s);
    switch (gl->matrix_mode) {
    case MGRAS_GL_MODELVIEW:
        return gl->modelview;
    case MGRAS_GL_PROJECTION:
        return gl->projection;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL matrix mode 0x%x is not "
                      "modelled; matrix command ignored\n",
                      gl->matrix_mode);
        return NULL;
    }
}

/*
 * glOrtho's and glFrustum's matrices (OpenGL 1.2.1, 2.10.2), from the six
 * arguments the client sends as floats. Arguments the client itself would
 * have rejected with GL_INVALID_VALUE leave the matrix unchanged.
 */
static bool mgras_gl_projection_matrix(uint32_t header, const uint32_t *p,
                                       double m[16])
{
    double l = mgras_gl_float(p[0]), r = mgras_gl_float(p[1]);
    double b = mgras_gl_float(p[2]), t = mgras_gl_float(p[3]);
    double n = mgras_gl_float(p[4]), f = mgras_gl_float(p[5]);

    if (l == r || b == t || n == f ||
        (header == MGRAS_GL_FRUSTUM && (n <= 0 || f <= 0))) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: GL projection with "
                      "invalid bounds ignored\n");
        return false;
    }
    memset(m, 0, 16 * sizeof(double));
    if (header == MGRAS_GL_ORTHO) {
        m[0] = 2 / (r - l);
        m[5] = 2 / (t - b);
        m[10] = -2 / (f - n);
        m[12] = -(r + l) / (r - l);
        m[13] = -(t + b) / (t - b);
        m[14] = -(f + n) / (f - n);
        m[15] = 1;
    } else {
        m[0] = 2 * n / (r - l);
        m[5] = 2 * n / (t - b);
        m[8] = (r + l) / (r - l);
        m[9] = (t + b) / (t - b);
        m[10] = -(f + n) / (f - n);
        m[11] = -1;
        m[14] = -2 * f * n / (f - n);
    }
    return true;
}

/*
 * glTranslatef's, glScalef's and glRotatef's matrices (OpenGL 1.2.1,
 * 2.10.2). The rotation is by the angle in degrees, counter-clockwise
 * looking from the axis toward the origin, about the axis scaled to unit
 * length; the model forms it in double precision. A zero axis leaves the
 * matrix unchanged.
 */
static bool mgras_gl_transform_matrix(uint32_t header, const uint32_t *p,
                                      double m[16])
{
    double x, y, z, len, a, c, sn;

    memset(m, 0, 16 * sizeof(double));
    m[0] = m[5] = m[10] = m[15] = 1;
    switch (header) {
    case MGRAS_GL_TRANSLATE:
        m[12] = mgras_gl_float(p[0]);
        m[13] = mgras_gl_float(p[1]);
        m[14] = mgras_gl_float(p[2]);
        return true;
    case MGRAS_GL_SCALE:
        m[0] = mgras_gl_float(p[0]);
        m[5] = mgras_gl_float(p[1]);
        m[10] = mgras_gl_float(p[2]);
        return true;
    default:
        a = mgras_gl_float(p[0]) * M_PI / 180.0;
        x = mgras_gl_float(p[1]);
        y = mgras_gl_float(p[2]);
        z = mgras_gl_float(p[3]);
        len = sqrt(x * x + y * y + z * z);
        if (!(len > 0)) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: glRotatef about a "
                          "zero axis ignored\n");
            return false;
        }
        x /= len;
        y /= len;
        z /= len;
        c = cos(a);
        sn = sin(a);
        /* Column-major: m[col * 4 + row]. */
        m[0] = x * x * (1 - c) + c;
        m[1] = y * x * (1 - c) + z * sn;
        m[2] = x * z * (1 - c) - y * sn;
        m[4] = x * y * (1 - c) - z * sn;
        m[5] = y * y * (1 - c) + c;
        m[6] = y * z * (1 - c) + x * sn;
        m[8] = x * z * (1 - c) + y * sn;
        m[9] = y * z * (1 - c) - x * sn;
        m[10] = z * z * (1 - c) + c;
        return true;
    }
}

/*
 * glPushMatrix and glPopMatrix on the current mode's stack (2.10.2).
 * Approximated: the board's stack depths are not known. Each stack holds
 * SGI_MGRAS_GL_STACK_DEPTH matrices, the current one included, the least
 * OpenGL 1.2.1 allows for the modelview stack (table 6.25) and more than
 * it requires for the projection stack. A push onto a full stack and a
 * pop of the last matrix change nothing, which is what the specification
 * has the stack-overflow and stack-underflow errors do; the client
 * reports the errors itself, if at all. The discriminating test is
 * gl-matrix-stack-holds-32.
 */
static void mgras_gl_stack_command(SGIMgrasState *s, uint32_t header,
                                   uint32_t *m)
{
    SGIMgrasGLContext *gl = mgras_gl(s);
    bool modelview = m == gl->modelview;
    uint32_t (*stack)[16] = modelview ? gl->modelview_stack :
                                        gl->projection_stack;
    uint32_t *depth = modelview ? &gl->modelview_depth :
                                  &gl->projection_depth;

    if (header == MGRAS_GL_PUSH_MATRIX) {
        if (*depth >= SGI_MGRAS_GL_STACK_DEPTH - 1) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: glPushMatrix on a "
                          "full stack ignored\n");
            return;
        }
        memcpy(stack[(*depth)++], m, 16 * sizeof(uint32_t));
        return;
    }
    if (*depth == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: glPopMatrix of the last "
                      "matrix ignored\n");
        return;
    }
    memcpy(m, stack[--(*depth)], 16 * sizeof(uint32_t));
}

static void mgras_gl_matrix_command(SGIMgrasState *s, uint32_t header,
                                    const uint32_t *payload)
{
    uint32_t *m = mgras_gl_current_matrix(s);
    double arg[16];

    if (!m) {
        return;
    }
    switch (header) {
    case MGRAS_GL_PUSH_MATRIX:
    case MGRAS_GL_POP_MATRIX:
        mgras_gl_stack_command(s, header, m);
        return;
    case MGRAS_GL_TRANSLATE:
    case MGRAS_GL_SCALE:
    case MGRAS_GL_ROTATE:
        if (mgras_gl_transform_matrix(header, payload, arg)) {
            mgras_gl_multiply(m, arg);
        }
        return;
    case MGRAS_GL_LOAD_IDENTITY:
        mgras_gl_identity(m);
        return;
    case MGRAS_GL_LOAD_MATRIX:
        memcpy(m, payload, 16 * sizeof(uint32_t));
        return;
    case MGRAS_GL_MULT_MATRIX:
        for (int i = 0; i < 16; i++) {
            arg[i] = mgras_gl_float(payload[i]);
        }
        mgras_gl_multiply(m, arg);
        return;
    default:
        if (mgras_gl_projection_matrix(header, payload, arg)) {
            mgras_gl_multiply(m, arg);
        }
        return;
    }
}

static int64_t mgras_gl_edge(const MgrasGLVertex *a, const MgrasGLVertex *b,
                             int64_t px, int64_t py)
{
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/*
 * A counter-clockwise edge owns the pixel centres on it when it is a left
 * edge (running down) or a top edge (horizontal, running left), so two
 * triangles sharing an edge never both draw its centres, as OpenGL 1.2.1,
 * 3.5.1 requires. Which edges own them is the model's choice. The
 * discriminating test is gl-triangle-shared-edge-centres-drawn-once.
 */
static bool mgras_gl_owns(const MgrasGLVertex *a, const MgrasGLVertex *b,
                          int64_t e)
{
    if (e != 0) {
        return e > 0;
    }
    return b->y < a->y || (b->y == a->y && b->x < a->x);
}

static bool mgras_gl_depth_passes(uint32_t func, uint32_t z, uint32_t stored)
{
    switch (func) {
    case MGRAS_GL_NEVER:
        return false;
    case MGRAS_GL_LESS:
        return z < stored;
    case MGRAS_GL_EQUAL:
        return z == stored;
    case MGRAS_GL_LEQUAL:
        return z <= stored;
    case MGRAS_GL_GREATER:
        return z > stored;
    case MGRAS_GL_NOTEQUAL:
        return z != stored;
    case MGRAS_GL_GEQUAL:
        return z >= stored;
    default:
        return true;
    }
}

/*
 * One fragment at window pixel (px, py) with window depth z: dropped
 * outside [0, 1] (the view volume's near and far planes), depth tested
 * when the test is enabled, then written to the draw buffers' banks.
 */
static void mgras_gl_fragment(SGIMgrasState *s, const MgrasGLWindow *win,
                              unsigned int banks, int px, int py, double z,
                              uint32_t rgb)
{
    SGIMgrasGLContext *gl = mgras_gl(s);
    uint32_t zq, *stored;

    if (!(z >= 0.0 && z <= 1.0)) {
        return;
    }
    zq = (uint32_t)llround(z * MGRAS_GL_DEPTH_MAX);
    stored = mgras_gl_depth(s, win->ox + px, win->oy + py);
    if (gl->depth_test) {
        if (!mgras_gl_depth_passes(gl->depth_func, zq, *stored)) {
            return;
        }
        if (gl->depth_mask) {
            *stored = zq;
        }
    }
    mgras_gl_put(s, banks, win->ox + px, win->oy + py, rgb);
}

/* A colour, each component clamped and rounded to eight bits. */
static uint32_t mgras_gl_rgb(const double c[3])
{
    return (mgras_gl_channel(c[0]) << 16) | (mgras_gl_channel(c[1]) << 8) |
           mgras_gl_channel(c[2]);
}

/*
 * Transform one vertex (OpenGL 1.2.1, 2.10): object to eye by the
 * modelview, eye to clip by the projection, the divide by w and the
 * viewport map with the default depth range [0, 1]. Arithmetic is double
 * precision, not the GE's; a result that differs only in rounding is
 * within the probe's margins. A vertex with w <= 0 needs clipping the
 * model does not do, and returns false.
 */
static bool mgras_gl_transform(SGIMgrasState *s, const uint32_t *pos,
                               const uint32_t *rgba, MgrasGLVertex *out)
{
    SGIMgrasGLContext *gl = mgras_gl(s);
    double obj[4], eye[4], clip[4];
    int vx = gl->viewport[0], vy = gl->viewport[1];
    int vw = gl->viewport[2], vh = gl->viewport[3];
    double xw, yw;

    for (int k = 0; k < 4; k++) {
        obj[k] = mgras_gl_float(pos[k]);
    }
    for (int row = 0; row < 4; row++) {
        eye[row] = 0;
        for (int k = 0; k < 4; k++) {
            eye[row] += mgras_gl_float(gl->modelview[k * 4 + row]) *
                        obj[k];
        }
    }
    for (int row = 0; row < 4; row++) {
        clip[row] = 0;
        for (int k = 0; k < 4; k++) {
            clip[row] += mgras_gl_float(gl->projection[k * 4 + row]) *
                         eye[k];
        }
    }
    if (!(clip[3] > 0)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL vertex with w %g "
                      "needs clipping, triangle dropped\n", clip[3]);
        return false;
    }
    /* 2.10.1: xw = (px / 2) xd + ox, ox = x + w / 2. */
    xw = (clip[0] / clip[3] + 1.0) * vw / 2.0 + vx;
    yw = (clip[1] / clip[3] + 1.0) * vh / 2.0 + vy;
    if (!(fabs(xw) < 65536.0 && fabs(yw) < 65536.0)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL vertex outside the "
                      "rasterizer's range, triangle dropped\n");
        return false;
    }
    out->x = llround(xw * (1 << MGRAS_GL_SUBPIXEL_BITS));
    out->y = llround(yw * (1 << MGRAS_GL_SUBPIXEL_BITS));
    out->z = (clip[2] / clip[3] + 1.0) / 2.0;
    out->inv_w = 1.0 / clip[3];
    for (int k = 0; k < 3; k++) {
        out->rgb[k] = MIN(MAX(mgras_gl_float(rgba[k]), 0.0f), 1.0f);
    }
    return true;
}

/*
 * Transform, map and rasterize one triangle, given each vertex's object
 * position and colour and, for flat shading, the provoking vertex's
 * colour (OpenGL 1.2.1, 2.13.7 and table 2.9).
 *
 * Clipping to the view volume is approximated. In x and y the fragments
 * are bounded by the viewport rectangle, which matches clipping for a
 * positive w; the discriminating test is
 * gl-triangle-bounded-by-the-viewport. In z a fragment whose interpolated
 * window depth lies outside [0, 1] is dropped: window depth is affine in
 * window x and y over the triangle, so that removes exactly the part
 * beyond the near or far plane (gl-triangle-clipped-at-the-far-plane). A
 * vertex with w <= 0 drops the whole triangle
 * (gl-triangle-with-negative-w-is-dropped).
 *
 * Depth is interpolated linearly in window coordinates and colour with
 * the perspective correction of equation 3.4 (OpenGL 1.2.1, 3.5.1).
 */
static void mgras_gl_triangle(SGIMgrasState *s, const uint32_t *pos[3],
                              const uint32_t *rgba[3],
                              const uint32_t *provoking)
{
    SGIMgrasGLContext *gl = mgras_gl(s);
    unsigned int banks = mgras_gl_draw_banks(s);
    MgrasGLWindow win;
    MgrasGLVertex v[3];
    int64_t min_x, max_x, min_y, max_y, area;
    int vx = gl->viewport[0], vy = gl->viewport[1];
    int vw = gl->viewport[2], vh = gl->viewport[3];
    int px0, px1, py0, py1;
    bool smooth = gl->shade_model != MGRAS_GL_FLAT;
    uint32_t flat_rgb;

    flat_rgb = (mgras_gl_channel(mgras_gl_float(provoking[0])) << 16) |
               (mgras_gl_channel(mgras_gl_float(provoking[1])) << 8) |
               mgras_gl_channel(mgras_gl_float(provoking[2]));

    if (!mgras_gl_window(s, &win) || vw <= 0 || vh <= 0) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        if (!mgras_gl_transform(s, pos[i], rgba[i], &v[i])) {
            return;
        }
    }

    area = mgras_gl_edge(&v[0], &v[1], v[2].x, v[2].y);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        /* Culling is off (3.5.1): draw either winding the same way. */
        MgrasGLVertex t = v[1];

        v[1] = v[2];
        v[2] = t;
        area = -area;
    }

    min_x = MIN(v[0].x, MIN(v[1].x, v[2].x)) >> MGRAS_GL_SUBPIXEL_BITS;
    max_x = MAX(v[0].x, MAX(v[1].x, v[2].x)) >> MGRAS_GL_SUBPIXEL_BITS;
    min_y = MIN(v[0].y, MIN(v[1].y, v[2].y)) >> MGRAS_GL_SUBPIXEL_BITS;
    max_y = MAX(v[0].y, MAX(v[1].y, v[2].y)) >> MGRAS_GL_SUBPIXEL_BITS;

    /* Window pixels, bounded by the viewport and the owned rectangle. */
    px0 = MAX(MAX((int)min_x, vx), win.x0 - win.ox);
    px1 = MIN(MIN((int)max_x, vx + vw - 1), win.x1 - win.ox);
    py0 = MAX(MAX((int)min_y, vy), win.y0 - win.oy);
    py1 = MIN(MIN((int)max_y, vy + vh - 1), win.y1 - win.oy);

    for (int py = py0; py <= py1; py++) {
        int64_t cy = ((int64_t)py << MGRAS_GL_SUBPIXEL_BITS) +
                     (1 << (MGRAS_GL_SUBPIXEL_BITS - 1));

        for (int px = px0; px <= px1; px++) {
            int64_t cx = ((int64_t)px << MGRAS_GL_SUBPIXEL_BITS) +
                         (1 << (MGRAS_GL_SUBPIXEL_BITS - 1));
            int64_t e0 = mgras_gl_edge(&v[1], &v[2], cx, cy);
            int64_t e1 = mgras_gl_edge(&v[2], &v[0], cx, cy);
            int64_t e2 = mgras_gl_edge(&v[0], &v[1], cx, cy);
            double b1, b2, z;
            uint32_t rgb = flat_rgb;

            if (!mgras_gl_owns(&v[1], &v[2], e0) ||
                !mgras_gl_owns(&v[2], &v[0], e1) ||
                !mgras_gl_owns(&v[0], &v[1], e2)) {
                continue;
            }
            b1 = (double)e1 / area;
            b2 = (double)e2 / area;
            z = v[0].z + b1 * (v[1].z - v[0].z) + b2 * (v[2].z - v[0].z);
            if (smooth) {
                double w0 = (1.0 - b1 - b2) * v[0].inv_w;
                double w1 = b1 * v[1].inv_w, w2 = b2 * v[2].inv_w;
                double sum = w0 + w1 + w2;
                double c[3];

                for (int k = 0; k < 3; k++) {
                    c[k] = (w0 * v[0].rgb[k] + w1 * v[1].rgb[k] +
                            w2 * v[2].rgb[k]) / sum;
                }
                rgb = mgras_gl_rgb(c);
            }
            mgras_gl_fragment(s, &win, banks, px, py, z, rgb);
        }
    }
    s->dirty = true;
}

/*
 * Whether a width-1 segment from a to b produces the fragment at pixel
 * (px, py): the diamond-exit rule of OpenGL 1.2.1, 3.4.1. The diamond is
 * the open square |x - xc| + |y - yc| < 1/2 about the pixel's centre, and
 * the segment produces the fragment when it meets the diamond, unless b
 * lies in it. The distance from the centre along the segment is convex
 * and piecewise linear, so its least value lies at an end or where the
 * segment crosses the centre's row or column; the test is exact in the
 * 1/256-pixel grid the vertices were snapped to.
 *
 * Approximated: the specification lets an implementation depart from
 * this rule within stated bounds, and it perturbs endpoints on a
 * diamond's edge in an unstated direction; the board's rule is not
 * published. The model applies the rule as written with no perturbation.
 * The discriminating tests are gl-reference/lines and
 * gl-line-leaves-out-its-last-pixel.
 */
static bool mgras_gl_line_covers(const MgrasGLVertex *a,
                                 const MgrasGLVertex *b, int px, int py)
{
    const int64_t half = 1 << (MGRAS_GL_SUBPIXEL_BITS - 1);
    int64_t cx = ((int64_t)px << MGRAS_GL_SUBPIXEL_BITS) + half;
    int64_t cy = ((int64_t)py << MGRAS_GL_SUBPIXEL_BITS) + half;
    int64_t dx = b->x - a->x, dy = b->y - a->y;
    int64_t ex = cx - a->x, ey = cy - a->y;

    if (llabs(b->x - cx) + llabs(b->y - cy) < half) {
        return false;
    }
    if (llabs(ex) + llabs(ey) < half) {
        return true;
    }
    /* Crossing the centre's column at t = ex / dx, scaled by |dx|. */
    if (dx && (dx > 0 ? ex >= 0 && ex <= dx : ex <= 0 && ex >= dx) &&
        llabs(dy * ex - ey * dx) < half * llabs(dx)) {
        return true;
    }
    /* Crossing the centre's row at t = ey / dy, scaled by |dy|. */
    return dy && (dy > 0 ? ey >= 0 && ey <= dy : ey <= 0 && ey >= dy) &&
           llabs(dx * ey - ex * dy) < half * llabs(dy);
}

/*
 * Transform, map and rasterize one width-1 segment (OpenGL 1.2.1, 3.4.1)
 * with the provoking vertex's colour when flat shaded. A fragment's
 * depth and smooth colour come from t, the centre's projection onto the
 * segment (equations 3.5 to 3.7), the colour corrected for perspective.
 *
 * Approximated like a triangle: the fragments are bounded by the viewport
 * rather than the segment clipped to the view volume, which differs only
 * in the pixels next to where a segment leaves the viewport, and a vertex
 * with w <= 0 drops the segment. The discriminating test is
 * gl-line-bounded-by-the-viewport. glLineWidth, glLineStipple and line
 * antialiasing are not modelled.
 */
static void mgras_gl_line(SGIMgrasState *s, const uint32_t *pa,
                          const uint32_t *ca, const uint32_t *pb,
                          const uint32_t *cb, const uint32_t *provoking)
{
    SGIMgrasGLContext *gl = mgras_gl(s);
    unsigned int banks = mgras_gl_draw_banks(s);
    int vx = gl->viewport[0], vy = gl->viewport[1];
    int vw = gl->viewport[2], vh = gl->viewport[3];
    bool smooth = gl->shade_model != MGRAS_GL_FLAT;
    bool x_major;
    MgrasGLWindow win;
    MgrasGLVertex a, b;
    double flat[3], len2;
    int major0, major1;

    if (!mgras_gl_window(s, &win) || vw <= 0 || vh <= 0 ||
        !mgras_gl_transform(s, pa, ca, &a) ||
        !mgras_gl_transform(s, pb, cb, &b) || (a.x == b.x && a.y == b.y)) {
        return;
    }
    for (int k = 0; k < 3; k++) {
        flat[k] = mgras_gl_float(provoking[k]);
    }
    len2 = (double)(b.x - a.x) * (b.x - a.x) +
           (double)(b.y - a.y) * (b.y - a.y);
    x_major = llabs(b.x - a.x) >= llabs(b.y - a.y);

    /*
     * Walk the major axis a pixel at a time, and in each step try the
     * minor-axis pixels within one of those the segment spans there.
     */
    major0 = (x_major ? MIN(a.x, b.x) : MIN(a.y, b.y)) >>
             MGRAS_GL_SUBPIXEL_BITS;
    major1 = (x_major ? MAX(a.x, b.x) : MAX(a.y, b.y)) >>
             MGRAS_GL_SUBPIXEL_BITS;
    for (int m = major0 - 1; m <= major1 + 1; m++) {
        const MgrasGLVertex *lo = x_major ? (a.x <= b.x ? &a : &b) :
                                            (a.y <= b.y ? &a : &b);
        const MgrasGLVertex *hi = lo == &a ? &b : &a;
        double l0 = x_major ? lo->x : lo->y, l1 = x_major ? hi->x : hi->y;
        double n0 = x_major ? lo->y : lo->x, n1 = x_major ? hi->y : hi->x;
        double e0 = MAX((double)m * (1 << MGRAS_GL_SUBPIXEL_BITS), l0);
        double e1 = MIN((double)(m + 1) * (1 << MGRAS_GL_SUBPIXEL_BITS), l1);
        double f0, f1;
        int minor0, minor1;

        if (l1 > l0) {
            e0 = MIN(e0, l1);
            e1 = MAX(e1, l0);
            f0 = n0 + (n1 - n0) * (e0 - l0) / (l1 - l0);
            f1 = n0 + (n1 - n0) * (e1 - l0) / (l1 - l0);
        } else {
            f0 = n0;
            f1 = n1;
        }
        minor0 = (int)floor(MIN(f0, f1) / (1 << MGRAS_GL_SUBPIXEL_BITS)) - 1;
        minor1 = (int)floor(MAX(f0, f1) / (1 << MGRAS_GL_SUBPIXEL_BITS)) + 1;
        for (int n = minor0; n <= minor1; n++) {
            int px = x_major ? m : n, py = x_major ? n : m;
            double cx = (px + 0.5) * (1 << MGRAS_GL_SUBPIXEL_BITS);
            double cy = (py + 0.5) * (1 << MGRAS_GL_SUBPIXEL_BITS);
            double t, c[3];

            if (px < MAX(vx, win.x0 - win.ox) ||
                px > MIN(vx + vw - 1, win.x1 - win.ox) ||
                py < MAX(vy, win.y0 - win.oy) ||
                py > MIN(vy + vh - 1, win.y1 - win.oy) ||
                !mgras_gl_line_covers(&a, &b, px, py)) {
                continue;
            }
            t = ((cx - a.x) * (b.x - a.x) + (cy - a.y) * (b.y - a.y)) / len2;
            t = MIN(MAX(t, 0.0), 1.0);
            for (int k = 0; k < 3; k++) {
                double wa = (1 - t) * a.inv_w, wb = t * b.inv_w;

                c[k] = smooth ? (wa * a.rgb[k] + wb * b.rgb[k]) / (wa + wb) :
                                flat[k];
            }
            mgras_gl_fragment(s, &win, banks, px, py,
                              a.z + t * (b.z - a.z), mgras_gl_rgb(c));
        }
    }
    s->dirty = true;
}

/*
 * A size-1 point covers the pixel holding its window position (OpenGL
 * 1.2.1, 3.3), at the 1/256-pixel grid the vertex was snapped to. A point
 * outside the viewport or beyond the near or far plane is dropped, which
 * is what clipping a point does while w is positive. glPointSize and
 * point antialiasing are not modelled.
 */
static void mgras_gl_point(SGIMgrasState *s, const uint32_t *pos,
                           const uint32_t *rgba)
{
    SGIMgrasGLContext *gl = mgras_gl(s);
    int vx = gl->viewport[0], vy = gl->viewport[1];
    int vw = gl->viewport[2], vh = gl->viewport[3];
    MgrasGLWindow win;
    MgrasGLVertex v;
    int px, py;

    if (!mgras_gl_window(s, &win) || !mgras_gl_transform(s, pos, rgba, &v)) {
        return;
    }
    px = v.x >> MGRAS_GL_SUBPIXEL_BITS;
    py = v.y >> MGRAS_GL_SUBPIXEL_BITS;
    if (px < MAX(vx, win.x0 - win.ox) ||
        px > MIN(vx + vw - 1, win.x1 - win.ox) ||
        py < MAX(vy, win.y0 - win.oy) ||
        py > MIN(vy + vh - 1, win.y1 - win.oy)) {
        return;
    }
    mgras_gl_fragment(s, &win, mgras_gl_draw_banks(s), px, py, v.z,
                      mgras_gl_rgb(v.rgb));
    s->dirty = true;
}

/* A segment between held or arriving vertices, flat colour from p. */
static void mgras_gl_draw_line(SGIMgrasState *s, uint32_t *const pos[4],
                               uint32_t *const rgba[4], int a, int b, int p)
{
    mgras_gl_line(s, pos[a], rgba[a], pos[b], rgba[b], rgba[p]);
}

/* A triangle of three held or arriving vertices, flat colour from p. */
static void mgras_gl_draw_triangle(SGIMgrasState *s, uint32_t *const pos[4],
                                   uint32_t *const rgba[4], int a, int b,
                                   int c, int p)
{
    const uint32_t *tp[3] = { pos[a], pos[b], pos[c] };
    const uint32_t *tc[3] = { rgba[a], rgba[b], rgba[c] };

    mgras_gl_triangle(s, tp, tc, rgba[p]);
}

/*
 * A quadrilateral a, b, c, d in order around its edge, drawn as the
 * triangles a b c and a c d. OpenGL 1.2.1 (3.5.1) allows any split of a
 * polygon with more than three vertices that gives a convex combination
 * of the vertices' values, and says nothing of non-convex or non-planar
 * ones; which diagonal the board takes is not known. Approximated: the
 * model splits every polygon into a fan from its first vertex. The
 * discriminating test is gl-quad-splits-from-its-first-vertex.
 */
static void mgras_gl_draw_quad(SGIMgrasState *s, uint32_t *const pos[4],
                               uint32_t *const rgba[4], int a, int b, int c,
                               int d, int p)
{
    mgras_gl_draw_triangle(s, pos, rgba, a, b, c, p);
    mgras_gl_draw_triangle(s, pos, rgba, a, c, d, p);
}

/* Hold the arriving vertex in slot n of the three a context keeps. */
static void mgras_gl_hold(SGIMgrasGLContext *gl, unsigned int n,
                          const uint32_t pos[4], const uint32_t rgba[4])
{
    memcpy(gl->vertex[n], pos, sizeof(gl->vertex[n]));
    memcpy(gl->vertex_color[n], rgba, sizeof(gl->vertex_color[n]));
}

/*
 * One vertex of the primitive open since glBegin (OpenGL 1.2.1, 2.6.1).
 * The context holds at most three earlier vertices, in vertex[] with
 * vertex_count of them in use, and every piece is drawn as soon as its
 * last vertex arrives, so a context switch between two vertices loses
 * nothing. The flat colour follows table 2.9 and 2.13.7: a point's own,
 * the second vertex of a segment, the last vertex of a triangle, strip
 * or fan triangle, quad or quad-strip quad, and the first vertex of a
 * polygon. A line loop's closing segment is drawn at glEnd. Vertices
 * that complete no piece before glEnd draw nothing.
 */
static void mgras_gl_vertex(SGIMgrasState *s, const uint32_t *payload,
                            uint32_t components)
{
    SGIMgrasGLContext *gl = mgras_gl(s);
    uint32_t cur[4] = { 0, 0, 0, mgras_gl_bits(1.0f) };
    uint32_t cur_rgba[4];
    uint32_t *const pos[4] = { gl->vertex[0], gl->vertex[1], gl->vertex[2],
                               cur };
    uint32_t *const rgba[4] = { gl->vertex_color[0], gl->vertex_color[1],
                                gl->vertex_color[2], cur_rgba };
    unsigned int n = gl->vertex_count;

    if (!gl->begin) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL vertex outside glBegin "
                      "ignored\n");
        return;
    }
    memcpy(cur, payload, components * sizeof(uint32_t));
    memcpy(cur_rgba, gl->color, sizeof(cur_rgba));

    switch (gl->begin) {
    case MGRAS_GL_BEGIN_POINTS:
        mgras_gl_point(s, cur, cur_rgba);
        return;
    case MGRAS_GL_BEGIN_LINES:
        if (n < 1) {
            break;
        }
        mgras_gl_draw_line(s, pos, rgba, 0, 3, 3);
        gl->vertex_count = 0;
        return;
    case MGRAS_GL_BEGIN_LINE_STRIP:
        if (n < 1) {
            break;
        }
        mgras_gl_draw_line(s, pos, rgba, 0, 3, 3);
        mgras_gl_hold(gl, 0, cur, cur_rgba);
        return;
    case MGRAS_GL_BEGIN_LINE_LOOP:
        /* Slot 0 keeps the first vertex for the closing segment. */
        if (n < 1) {
            break;
        }
        mgras_gl_draw_line(s, pos, rgba, n - 1, 3, 3);
        mgras_gl_hold(gl, 1, cur, cur_rgba);
        gl->vertex_count = 2;
        return;
    case MGRAS_GL_BEGIN_TRIANGLES:
        if (n < 2) {
            break;
        }
        mgras_gl_draw_triangle(s, pos, rgba, 0, 1, 3, 3);
        gl->vertex_count = 0;
        return;
    case MGRAS_GL_BEGIN_TRI_STRIP:
        if (n < 2) {
            break;
        }
        mgras_gl_draw_triangle(s, pos, rgba, 0, 1, 3, 3);
        mgras_gl_hold(gl, 0, gl->vertex[1], gl->vertex_color[1]);
        mgras_gl_hold(gl, 1, cur, cur_rgba);
        return;
    case MGRAS_GL_BEGIN_TRI_FAN:
    case MGRAS_GL_BEGIN_POLYGON:
        if (n < 2) {
            break;
        }
        mgras_gl_draw_triangle(s, pos, rgba, 0, 1, 3,
                               gl->begin == MGRAS_GL_BEGIN_POLYGON ? 0 : 3);
        mgras_gl_hold(gl, 1, cur, cur_rgba);
        return;
    case MGRAS_GL_BEGIN_QUADS:
        if (n < 3) {
            break;
        }
        mgras_gl_draw_quad(s, pos, rgba, 0, 1, 2, 3, 3);
        gl->vertex_count = 0;
        return;
    case MGRAS_GL_BEGIN_QUAD_STRIP:
        if (n < 3) {
            break;
        }
        /* Vertices 2i - 1, 2i, 2i + 2 and 2i + 1 in order (2.6.1). */
        mgras_gl_draw_quad(s, pos, rgba, 0, 1, 3, 2, 3);
        mgras_gl_hold(gl, 0, gl->vertex[2], gl->vertex_color[2]);
        mgras_gl_hold(gl, 1, cur, cur_rgba);
        gl->vertex_count = 2;
        return;
    default:
        g_assert_not_reached();
    }
    mgras_gl_hold(gl, n, cur, cur_rgba);
    gl->vertex_count = n + 1;
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
    mgras_gl_context_init(mgras_gl(s));
}

void sgi_mgras_gl_execute(SGIMgrasState *s, uint32_t header,
                          const uint32_t *payload, uint32_t words)
{
    static const float one = 1.0f;
    SGIMgrasGLContext *gl = mgras_gl(s);

    if (words != (header & 0xff) / sizeof(uint32_t)) {
        return;
    }
    if (mgras_gl_begin_token(header, false)) {
        gl->begin = header;
        gl->vertex_count = 0;
        return;
    }
    if (mgras_gl_begin_token(header, true)) {
        /*
         * A line loop closes from its last vertex to its first, in the
         * first vertex's colour when flat (2.13.7). Vertices that
         * complete no piece draw nothing (2.6.1).
         */
        if (gl->begin == MGRAS_GL_BEGIN_LINE_LOOP && gl->vertex_count == 2) {
            mgras_gl_line(s, gl->vertex[1], gl->vertex_color[1],
                          gl->vertex[0], gl->vertex_color[0],
                          gl->vertex_color[0]);
        }
        gl->begin = 0;
        gl->vertex_count = 0;
        return;
    }
    switch (header) {
    case MGRAS_GL_CONTEXT_OPENER:
        mgras_gl_context_start(s);
        return;
    case MGRAS_GL_VIEWPORT:
        /* The client has already clamped the size (__glMgrim_Viewport). */
        for (int i = 0; i < 4; i++) {
            gl->viewport[i] = (int32_t)payload[i];
        }
        return;
    case MGRAS_GL_CLEAR_COLOR:
        memcpy(gl->clear_color, payload, sizeof(gl->clear_color));
        return;
    case MGRAS_GL_CLEAR_COLOR_BUF:
        mgras_gl_clear(s, false);
        return;
    case MGRAS_GL_CLEAR_DEPTH:
        gl->clear_depth = payload[0];
        return;
    case MGRAS_GL_CLEAR_DEPTH_BUF:
        mgras_gl_clear(s, true);
        return;
    case MGRAS_GL_SHADE_MODEL:
        gl->shade_model = payload[0];
        return;
    case MGRAS_GL_MATRIX_MODE:
        gl->matrix_mode = payload[0];
        return;
    case MGRAS_GL_LOAD_MATRIX:
    case MGRAS_GL_LOAD_IDENTITY:
    case MGRAS_GL_MULT_MATRIX:
    case MGRAS_GL_FRUSTUM:
    case MGRAS_GL_ORTHO:
    case MGRAS_GL_PUSH_MATRIX:
    case MGRAS_GL_POP_MATRIX:
    case MGRAS_GL_TRANSLATE:
    case MGRAS_GL_SCALE:
    case MGRAS_GL_ROTATE:
        mgras_gl_matrix_command(s, header, payload);
        return;
    case MGRAS_GL_DEPTH_TEST:
        /*
         * glEnable(GL_DEPTH_TEST) sent 0x6604 with 1 and no other word in
         * the probe's depth case, and every context setup sends it with 0
         * (run irix-20260927T172059-fdbada).
         */
        gl->depth_test = payload[0] != 0;
        return;
    case MGRAS_GL_DEPTH_FUNC:
        gl->depth_func = payload[0] & 7;
        return;
    case MGRAS_GL_DEPTH_MASK:
        gl->depth_mask = payload[0] != 0;
        return;
    case MGRAS_GL_DRAW_BUFFER:
        gl->draw_buffers = payload[0];
        return;
    case MGRAS_GL_BANKS:
        if (payload[0] != payload[1]) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: bank token words %u and "
                          "%u differ; the model uses the first\n",
                          payload[0], payload[1]);
        }
        s->gl_banks[0] = payload[0];
        s->gl_banks[1] = payload[1];
        return;
    case MGRAS_GL_COLOR3F:
        memcpy(gl->color, payload, 3 * sizeof(uint32_t));
        memcpy(&gl->color[3], &one, sizeof(gl->color[3]));
        return;
    case MGRAS_GL_COLOR4F:
        memcpy(gl->color, payload, sizeof(gl->color));
        return;
    case MGRAS_GL_COLOR3UB:
        mgras_gl_color_bytes(gl->color, payload[0], false);
        return;
    case MGRAS_GL_NORMAL3F:
        /* Kept for the 0xa104 query only; lighting is not modelled. */
        memcpy(gl->normal, payload, sizeof(gl->normal));
        return;
    case MGRAS_GL_EDGE_FLAG_TRUE:
    case MGRAS_GL_EDGE_FLAG_FALSE:
        /* Kept for the 0xa104 query; polygon edge modes are not modelled. */
        gl->edge_flag = header == MGRAS_GL_EDGE_FLAG_TRUE;
        return;
    case MGRAS_GL_COLOR4UB:
        mgras_gl_color_bytes(gl->color, payload[0], true);
        return;
    case MGRAS_GL_VERTEX2F:
        mgras_gl_vertex(s, payload, 2);
        return;
    case MGRAS_GL_VERTEX3F:
        mgras_gl_vertex(s, payload, 3);
        return;
    case MGRAS_GL_VERTEX4F:
        mgras_gl_vertex(s, payload, 4);
        return;
    case MGRAS_GL_VERTEX2I: {
        /* Integer components convert to float unscaled (2.7). */
        uint32_t xy[2] = {
            mgras_gl_bits((int32_t)payload[0]),
            mgras_gl_bits((int32_t)payload[1]),
        };

        mgras_gl_vertex(s, xy, 2);
        return;
    }
    case MGRAS_GL_FLUSH:
        /* Approximated: every command has already run to completion. */
        return;
    default:
        g_assert_not_reached();
    }
}
