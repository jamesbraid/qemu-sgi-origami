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
 * untextured triangles, flat or smooth shaded, transformed by the
 * modelview and projection matrices and depth tested, leave in the
 * window's front or back buffer, and the main-port framing, so the words
 * after a packet still parse as headers. What it does not: the
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
#define MGRAS_GL_BEGIN_TRIANGLES 0x00001a00
#define MGRAS_GL_END_TRIANGLES   0x00002400
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

bool sgi_mgras_gl_header(uint32_t header)
{
    switch (header) {
    case MGRAS_GL_CONTEXT_OPENER:
    case MGRAS_GL_BEGIN_TRIANGLES:
    case MGRAS_GL_END_TRIANGLES:
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

static void mgras_gl_identity(uint32_t m[16])
{
    for (int i = 0; i < 16; i++) {
        m[i] = mgras_gl_bits(i % 5 == 0);
    }
}

/* OpenGL 1.2.1 initial state (2.7, 2.10, 2.13.7, 4.1.5, 4.2.1-4.2.3). */
void sgi_mgras_gl_reset(SGIMgrasState *s)
{
    static const float one = 1.0f;
    uint32_t one_bits;

    memcpy(&one_bits, &one, sizeof(one_bits));
    memset(s->gl_viewport, 0, sizeof(s->gl_viewport));
    memset(s->gl_clear_color, 0, sizeof(s->gl_clear_color));
    for (int i = 0; i < 4; i++) {
        s->gl_color[i] = one_bits;
    }
    s->gl_shade_model = MGRAS_GL_SMOOTH;
    s->gl_begin = 0;
    s->gl_vertex_count = 0;
    memset(s->gl_vertex, 0, sizeof(s->gl_vertex));
    memset(s->gl_vertex_color, 0, sizeof(s->gl_vertex_color));
    s->gl_matrix_mode = MGRAS_GL_MODELVIEW;
    mgras_gl_identity(s->gl_modelview);
    mgras_gl_identity(s->gl_projection);
    s->gl_depth_test = 0;
    s->gl_depth_func = MGRAS_GL_LESS;
    s->gl_depth_mask = 1;
    s->gl_clear_depth = MGRAS_GL_DEPTH_MAX;
    s->gl_draw_buffers = MGRAS_GL_DRAW_FRONT;
    /* The value every captured GL context image restores. */
    s->gl_banks[0] = s->gl_banks[1] = 1;
}

void sgi_mgras_gl_context_loaded(SGIMgrasState *s)
{
    s->gl_banks[0] = s->context[MGRAS_GL_IMAGE_BANKS];
    s->gl_banks[1] = s->context[MGRAS_GL_IMAGE_BANKS + 1];
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
    win->x1 = MIN(clip_x & 0xffff, SGI_MGRAS_WIDTH - 1);
    win->y1 = MIN(clip_y & 0xffff, SGI_MGRAS_HEIGHT - 1);
    return win->x0 <= win->x1 && win->y0 <= win->y1;
}

/*
 * The pixel-memory banks the draw buffers select, as a mask of bank 0 and
 * bank 1. Bank 0 is the model's front[] plane, the one the X server draws
 * in; bank 1 is back[].
 */
static unsigned int mgras_gl_draw_banks(SGIMgrasState *s)
{
    unsigned int back = s->gl_banks[0] & 1;
    unsigned int banks = 0;

    if (s->gl_draw_buffers & ~(MGRAS_GL_DRAW_FRONT | MGRAS_GL_DRAW_BACK)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL draw buffers 0x%x beyond "
                      "the front and back buffers ignored\n",
                      s->gl_draw_buffers);
    }
    if (s->gl_draw_buffers & MGRAS_GL_DRAW_FRONT) {
        banks |= 1u << (back ^ 1);
    }
    if (s->gl_draw_buffers & MGRAS_GL_DRAW_BACK) {
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
    unsigned int idx = (SGI_MGRAS_HEIGHT - 1 - y) * SGI_MGRAS_WIDTH + x;

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
    return &s->depth[(SGI_MGRAS_HEIGHT - 1 - y) * SGI_MGRAS_WIDTH + x];
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
    MgrasGLWindow win;
    unsigned int banks = mgras_gl_draw_banks(s);
    uint32_t rgb = 0;

    if (!mgras_gl_window(s, &win)) {
        return;
    }
    if (depth && !s->gl_depth_mask) {
        return;
    }
    if (!depth) {
        rgb = (mgras_gl_channel(mgras_gl_float(s->gl_clear_color[0])) << 16) |
              (mgras_gl_channel(mgras_gl_float(s->gl_clear_color[1])) << 8) |
              mgras_gl_channel(mgras_gl_float(s->gl_clear_color[2]));
    }
    for (int y = win.y0; y <= win.y1; y++) {
        for (int x = win.x0; x <= win.x1; x++) {
            if (depth) {
                *mgras_gl_depth(s, x, y) =
                    s->gl_clear_depth & MGRAS_GL_DEPTH_MAX;
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
    switch (s->gl_matrix_mode) {
    case MGRAS_GL_MODELVIEW:
        return s->gl_modelview;
    case MGRAS_GL_PROJECTION:
        return s->gl_projection;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL matrix mode 0x%x is not "
                      "modelled; matrix command ignored\n",
                      s->gl_matrix_mode);
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

static void mgras_gl_matrix_command(SGIMgrasState *s, uint32_t header,
                                    const uint32_t *payload)
{
    uint32_t *m = mgras_gl_current_matrix(s);
    double arg[16];

    if (!m) {
        return;
    }
    switch (header) {
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
 * Transform one vertex (OpenGL 1.2.1, 2.10): object to eye by the
 * modelview, eye to clip by the projection, the divide by w and the
 * viewport map with the default depth range [0, 1]. Arithmetic is double
 * precision, not the GE's; a result that differs only in rounding is
 * within the probe's margins. A vertex with w <= 0 needs clipping the
 * model does not do, and returns false.
 */
static bool mgras_gl_transform(SGIMgrasState *s, int i, MgrasGLVertex *out)
{
    double obj[4], eye[4], clip[4];
    int vx = s->gl_viewport[0], vy = s->gl_viewport[1];
    int vw = s->gl_viewport[2], vh = s->gl_viewport[3];
    double xw, yw;

    for (int k = 0; k < 4; k++) {
        obj[k] = mgras_gl_float(s->gl_vertex[i][k]);
    }
    for (int row = 0; row < 4; row++) {
        eye[row] = 0;
        for (int k = 0; k < 4; k++) {
            eye[row] += mgras_gl_float(s->gl_modelview[k * 4 + row]) *
                        obj[k];
        }
    }
    for (int row = 0; row < 4; row++) {
        clip[row] = 0;
        for (int k = 0; k < 4; k++) {
            clip[row] += mgras_gl_float(s->gl_projection[k * 4 + row]) *
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
        out->rgb[k] = MIN(MAX(mgras_gl_float(s->gl_vertex_color[i][k]),
                              0.0f), 1.0f);
    }
    return true;
}

/*
 * Transform, map and rasterize the collected triangle.
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
static void mgras_gl_triangle(SGIMgrasState *s)
{
    const uint32_t *provoking = s->gl_vertex_color[2];
    unsigned int banks = mgras_gl_draw_banks(s);
    MgrasGLWindow win;
    MgrasGLVertex v[3];
    int64_t min_x, max_x, min_y, max_y, area;
    int vx = s->gl_viewport[0], vy = s->gl_viewport[1];
    int vw = s->gl_viewport[2], vh = s->gl_viewport[3];
    int px0, px1, py0, py1;
    bool smooth = s->gl_shade_model != MGRAS_GL_FLAT;
    uint32_t flat_rgb;

    /* Table 2.9: an independent triangle's third vertex provokes. */
    flat_rgb = (mgras_gl_channel(mgras_gl_float(provoking[0])) << 16) |
               (mgras_gl_channel(mgras_gl_float(provoking[1])) << 8) |
               mgras_gl_channel(mgras_gl_float(provoking[2]));

    if (!mgras_gl_window(s, &win) || vw <= 0 || vh <= 0) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        if (!mgras_gl_transform(s, i, &v[i])) {
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
            uint32_t zq, *stored;

            if (!mgras_gl_owns(&v[1], &v[2], e0) ||
                !mgras_gl_owns(&v[2], &v[0], e1) ||
                !mgras_gl_owns(&v[0], &v[1], e2)) {
                continue;
            }
            b1 = (double)e1 / area;
            b2 = (double)e2 / area;
            z = v[0].z + b1 * (v[1].z - v[0].z) + b2 * (v[2].z - v[0].z);
            if (!(z >= 0.0 && z <= 1.0)) {
                continue;
            }
            zq = (uint32_t)llround(z * MGRAS_GL_DEPTH_MAX);
            stored = mgras_gl_depth(s, win.ox + px, win.oy + py);
            if (s->gl_depth_test) {
                if (!mgras_gl_depth_passes(s->gl_depth_func, zq, *stored)) {
                    continue;
                }
                if (s->gl_depth_mask) {
                    *stored = zq;
                }
            }
            if (smooth) {
                double w0 = (1.0 - b1 - b2) * v[0].inv_w;
                double w1 = b1 * v[1].inv_w, w2 = b2 * v[2].inv_w;
                double sum = w0 + w1 + w2;

                rgb = 0;
                for (int k = 0; k < 3; k++) {
                    double c = (w0 * v[0].rgb[k] + w1 * v[1].rgb[k] +
                                w2 * v[2].rgb[k]) / sum;

                    rgb = (rgb << 8) | mgras_gl_channel(c);
                }
            }
            mgras_gl_put(s, banks, win.ox + px, win.oy + py, rgb);
        }
    }
    s->dirty = true;
}

static void mgras_gl_vertex(SGIMgrasState *s, const uint32_t *payload,
                            uint32_t components)
{
    static const float one = 1.0f;
    uint32_t *vertex;

    if (s->gl_begin != MGRAS_GL_BEGIN_TRIANGLES) {
        /*
         * The other primitive modes are not tracked, so this may be a
         * legal vertex of a strip, fan or line the model cannot draw.
         */
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL vertex outside "
                      "glBegin(GL_TRIANGLES) ignored\n");
        return;
    }
    vertex = s->gl_vertex[s->gl_vertex_count];
    memset(vertex, 0, sizeof(s->gl_vertex[0]));
    memcpy(&vertex[3], &one, sizeof(vertex[3]));
    memcpy(vertex, payload, components * sizeof(uint32_t));
    memcpy(s->gl_vertex_color[s->gl_vertex_count], s->gl_color,
           sizeof(s->gl_color));
    if (++s->gl_vertex_count == 3) {
        mgras_gl_triangle(s);
        s->gl_vertex_count = 0;
    }
}

/*
 * The client writes the opener first when it sets up a new context
 * (mgras-gl-commands.md, "Context setup opener"), and the setup that
 * follows sends no matrix, so the board must already hold the OpenGL
 * initial state then; where it keeps each context's state is unknown. The
 * model has one GL state for the board and restores the initial values
 * here, keeping the banks the context image gave. The discriminating test
 * is gl-context-setup-restores-initial-state.
 */
static void mgras_gl_context_start(SGIMgrasState *s)
{
    uint32_t banks[2] = { s->gl_banks[0], s->gl_banks[1] };

    sgi_mgras_gl_reset(s);
    s->gl_banks[0] = banks[0];
    s->gl_banks[1] = banks[1];
}

void sgi_mgras_gl_execute(SGIMgrasState *s, uint32_t header,
                          const uint32_t *payload, uint32_t words)
{
    static const float one = 1.0f;

    if (words != (header & 0xff) / sizeof(uint32_t)) {
        return;
    }
    switch (header) {
    case MGRAS_GL_CONTEXT_OPENER:
        mgras_gl_context_start(s);
        return;
    case MGRAS_GL_VIEWPORT:
        /* The client has already clamped the size (__glMgrim_Viewport). */
        for (int i = 0; i < 4; i++) {
            s->gl_viewport[i] = (int32_t)payload[i];
        }
        return;
    case MGRAS_GL_CLEAR_COLOR:
        memcpy(s->gl_clear_color, payload, sizeof(s->gl_clear_color));
        return;
    case MGRAS_GL_CLEAR_COLOR_BUF:
        mgras_gl_clear(s, false);
        return;
    case MGRAS_GL_CLEAR_DEPTH:
        s->gl_clear_depth = payload[0];
        return;
    case MGRAS_GL_CLEAR_DEPTH_BUF:
        mgras_gl_clear(s, true);
        return;
    case MGRAS_GL_SHADE_MODEL:
        s->gl_shade_model = payload[0];
        return;
    case MGRAS_GL_MATRIX_MODE:
        s->gl_matrix_mode = payload[0];
        return;
    case MGRAS_GL_LOAD_MATRIX:
    case MGRAS_GL_LOAD_IDENTITY:
    case MGRAS_GL_MULT_MATRIX:
    case MGRAS_GL_FRUSTUM:
    case MGRAS_GL_ORTHO:
        mgras_gl_matrix_command(s, header, payload);
        return;
    case MGRAS_GL_DEPTH_TEST:
        /*
         * glEnable(GL_DEPTH_TEST) sent 0x6604 with 1 and no other word in
         * the probe's depth case, and every context setup sends it with 0
         * (run irix-20260927T172059-fdbada).
         */
        s->gl_depth_test = payload[0] != 0;
        return;
    case MGRAS_GL_DEPTH_FUNC:
        s->gl_depth_func = payload[0] & 7;
        return;
    case MGRAS_GL_DEPTH_MASK:
        s->gl_depth_mask = payload[0] != 0;
        return;
    case MGRAS_GL_DRAW_BUFFER:
        s->gl_draw_buffers = payload[0];
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
        memcpy(s->gl_color, payload, 3 * sizeof(uint32_t));
        memcpy(&s->gl_color[3], &one, sizeof(s->gl_color[3]));
        return;
    case MGRAS_GL_COLOR4F:
        memcpy(s->gl_color, payload, sizeof(s->gl_color));
        return;
    case MGRAS_GL_BEGIN_TRIANGLES:
        s->gl_begin = header;
        s->gl_vertex_count = 0;
        return;
    case MGRAS_GL_END_TRIANGLES:
        /* A trailing partial triangle draws nothing (2.6.1). */
        s->gl_begin = 0;
        s->gl_vertex_count = 0;
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
    case MGRAS_GL_FLUSH:
        /* Approximated: every command has already run to completion. */
        return;
    default:
        g_assert_not_reached();
    }
}
