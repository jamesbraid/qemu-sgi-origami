/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SI MGRAS OpenGL command execution.
 *
 * On the board the HQ4 and GE11 run SGI programs that turn the OpenGL
 * client's command words into raster-engine work. Those programs are
 * opaque here, so the model reads the words at the boundary the IRIX
 * 6.5.30 client library defines (docs/datasheets/mgras-gl-commands.md in
 * the Origami repository) and draws what the OpenGL 1.2.1 specification
 * says those commands produce. This is an approximation of the geometry
 * engine's function, not of its program.
 *
 * What it preserves: the pixels a clear and a flat-shaded, untextured,
 * unclipped triangle leave in the window, and the main-port framing, so
 * the words after a packet still parse as headers. What it does not: the
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
#define MGRAS_GL_BEGIN_TRIANGLES 0x00001a00
#define MGRAS_GL_END_TRIANGLES   0x00002400
#define MGRAS_GL_CLEAR_COLOR_BUF 0x00001500
#define MGRAS_GL_VIEWPORT        0x00003310
#define MGRAS_GL_FLUSH           0x00003600
#define MGRAS_GL_SHADE_MODEL     0x00004704
#define MGRAS_GL_CLEAR_COLOR     0x0000ba10

/* Vertex-attribute headers: the class names the source format. */
#define MGRAS_GL_VERTEX2F        0x40000008
#define MGRAS_GL_VERTEX3F        0x2000000c
#define MGRAS_GL_VERTEX4F        0x00000010
#define MGRAS_GL_COLOR3F         0x2000020c
#define MGRAS_GL_COLOR4F         0x00000210

#define MGRAS_GL_FLAT            0x1d00
#define MGRAS_GL_SMOOTH          0x1d01

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
 * Window coordinates are snapped to 1/256 pixel before rasterization. The
 * Octane Technical Report (section 4.2.3) gives the RE4 eight bits of
 * subpixel precision; how the GE rounds to them is not published, so
 * rounding to nearest is the model's choice. The discriminating test is
 * gl-triangle-snaps-to-the-nearest-subpixel.
 */
#define MGRAS_GL_SUBPIXEL_BITS   8

/* Screen columns, and rows counted up from the bottom of the screen. */
typedef struct MgrasGLWindow {
    int ox, oy;           /* the window's lower-left pixel */
    int x0, y0, x1, y1;   /* the pixels it owns, inclusive */
} MgrasGLWindow;

bool sgi_mgras_gl_header(uint32_t header)
{
    switch (header) {
    case MGRAS_GL_BEGIN_TRIANGLES:
    case MGRAS_GL_END_TRIANGLES:
    case MGRAS_GL_CLEAR_COLOR_BUF:
    case MGRAS_GL_VIEWPORT:
    case MGRAS_GL_FLUSH:
    case MGRAS_GL_SHADE_MODEL:
    case MGRAS_GL_CLEAR_COLOR:
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

/* OpenGL 1.2.1 initial state (2.7, 2.10.1, 2.13.7, 4.2.3). */
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
}

static float mgras_gl_float(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

/*
 * A component clamped to [0, 1] and rounded to the nearest of eight bits
 * (OpenGL 1.2.1, 2.13.9). The 12-bit-per-channel visual the probe uses
 * (visual 0x30, 12 red bits) is stored at the eight bits a direct-colour
 * pixel holds in this model; 0.0 and 1.0 are exact either way.
 */
static uint32_t mgras_gl_channel(uint32_t bits)
{
    float c = mgras_gl_float(bits);

    if (!(c > 0.0f)) {
        return 0;
    }
    if (c >= 1.0f) {
        return 0xff;
    }
    return (uint32_t)(c * 255.0f + 0.5f);
}

static uint32_t mgras_gl_rgb(const uint32_t color[4])
{
    return (mgras_gl_channel(color[0]) << 16) |
           (mgras_gl_channel(color[1]) << 8) | mgras_gl_channel(color[2]);
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
 * Store a direct-colour pixel at screen column x and row y counted from
 * the bottom. The GL window's WID selects an XMAP mode, 0x1408 for the
 * probe's visual, whose colour map is never loaded; an RGB pixel bypasses
 * that map (sgi-mgras-display.c), which is what makes the drawing visible.
 */
static void mgras_gl_put(SGIMgrasState *s, int x, int y, uint32_t rgb)
{
    unsigned int idx = (SGI_MGRAS_HEIGHT - 1 - y) * SGI_MGRAS_WIDTH + x;

    s->front[idx] = rgb;
    s->format[idx] = SGI_MGRAS_PIXEL_FORMAT_RGB;
}

/*
 * OpenGL 1.2.1, 4.2.3: a clear passes only the pixel ownership, scissor
 * and dither steps, and the viewport does not bound it. The model takes
 * ownership as the image's window rectangle, which is the whole window
 * when nothing covers it; the scissor test and dithering are not modelled.
 */
static void mgras_gl_clear(SGIMgrasState *s)
{
    MgrasGLWindow win;
    uint32_t rgb = mgras_gl_rgb(s->gl_clear_color);

    if (!mgras_gl_window(s, &win)) {
        return;
    }
    for (int y = win.y0; y <= win.y1; y++) {
        for (int x = win.x0; x <= win.x1; x++) {
            mgras_gl_put(s, x, y, rgb);
        }
    }
    s->dirty = true;
}

static int64_t mgras_gl_edge(const int64_t a[2], const int64_t b[2],
                             int64_t px, int64_t py)
{
    int64_t cross = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);

    return cross;
}

/*
 * A counter-clockwise edge owns the pixel centres on it when it is a left
 * edge (running down) or a top edge (horizontal, running left), so two
 * triangles sharing an edge never both draw its centres, as OpenGL 1.2.1,
 * 3.5.1 requires. Which edges own them is the model's choice. The
 * discriminating test is gl-triangle-shared-edge-centres-drawn-once.
 */
static bool mgras_gl_owns(const int64_t a[2], const int64_t b[2], int64_t e)
{
    if (e != 0) {
        return e > 0;
    }
    return b[1] < a[1] || (b[1] == a[1] && b[0] < a[0]);
}

/*
 * Transform, map and rasterize the collected triangle. Modelview and
 * projection are the identity the client leaves them at: the decoder does
 * not yet execute the matrix commands, and the parser logs those as
 * unimplemented. Clipping to the view volume is approximated by bounding
 * the fragments to the viewport rectangle, which matches it in x and y for
 * a positive w; z is not clipped and no depth is stored. A vertex with
 * w <= 0 drops the triangle. The discriminating tests are
 * gl-triangle-bounded-by-the-viewport and
 * gl-triangle-with-negative-w-is-dropped.
 */
static void mgras_gl_triangle(SGIMgrasState *s)
{
    const uint32_t *provoking = s->gl_vertex_color[2];
    MgrasGLWindow win;
    int64_t v[3][2];
    int64_t min_x, max_x, min_y, max_y, area;
    int vx = s->gl_viewport[0], vy = s->gl_viewport[1];
    int vw = s->gl_viewport[2], vh = s->gl_viewport[3];
    int px0, px1, py0, py1;
    uint32_t rgb;

    if (s->gl_shade_model != MGRAS_GL_FLAT &&
        (memcmp(s->gl_vertex_color[0], provoking,
                sizeof(s->gl_vertex_color[0])) ||
         memcmp(s->gl_vertex_color[1], provoking,
                sizeof(s->gl_vertex_color[1])))) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: smooth-shaded GL triangle "
                      "drawn flat in its third vertex's colour\n");
    }
    /* Table 2.9: an independent triangle's third vertex provokes. */
    rgb = mgras_gl_rgb(provoking);

    if (!mgras_gl_window(s, &win) || vw <= 0 || vh <= 0) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        float x = mgras_gl_float(s->gl_vertex[i][0]);
        float y = mgras_gl_float(s->gl_vertex[i][1]);
        float w = mgras_gl_float(s->gl_vertex[i][3]);
        double xw, yw;

        if (!(w > 0.0f)) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL vertex with w %g "
                          "needs clipping, triangle dropped\n", w);
            return;
        }
        /* 2.10.1: xw = (px / 2) xd + ox, ox = x + w / 2. */
        xw = ((double)x / w + 1.0) * vw / 2.0 + vx;
        yw = ((double)y / w + 1.0) * vh / 2.0 + vy;
        if (!(fabs(xw) < 65536.0 && fabs(yw) < 65536.0)) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL vertex outside the "
                          "rasterizer's range, triangle dropped\n");
            return;
        }
        v[i][0] = llround(xw * (1 << MGRAS_GL_SUBPIXEL_BITS));
        v[i][1] = llround(yw * (1 << MGRAS_GL_SUBPIXEL_BITS));
    }

    area = mgras_gl_edge(v[0], v[1], v[2][0], v[2][1]);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        /* Culling is off (3.5.1): draw either winding the same way. */
        int64_t t[2] = { v[1][0], v[1][1] };

        v[1][0] = v[2][0];
        v[1][1] = v[2][1];
        v[2][0] = t[0];
        v[2][1] = t[1];
    }

    min_x = MIN(v[0][0], MIN(v[1][0], v[2][0])) >> MGRAS_GL_SUBPIXEL_BITS;
    max_x = MAX(v[0][0], MAX(v[1][0], v[2][0])) >> MGRAS_GL_SUBPIXEL_BITS;
    min_y = MIN(v[0][1], MIN(v[1][1], v[2][1])) >> MGRAS_GL_SUBPIXEL_BITS;
    max_y = MAX(v[0][1], MAX(v[1][1], v[2][1])) >> MGRAS_GL_SUBPIXEL_BITS;

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

            if (mgras_gl_owns(v[0], v[1], mgras_gl_edge(v[0], v[1], cx, cy)) &&
                mgras_gl_owns(v[1], v[2], mgras_gl_edge(v[1], v[2], cx, cy)) &&
                mgras_gl_owns(v[2], v[0], mgras_gl_edge(v[2], v[0], cx, cy))) {
                mgras_gl_put(s, win.ox + px, win.oy + py, rgb);
            }
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

void sgi_mgras_gl_execute(SGIMgrasState *s, uint32_t header,
                          const uint32_t *payload, uint32_t words)
{
    static const float one = 1.0f;

    if (words != (header & 0xff) / sizeof(uint32_t)) {
        return;
    }
    switch (header) {
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
        mgras_gl_clear(s);
        return;
    case MGRAS_GL_SHADE_MODEL:
        s->gl_shade_model = payload[0];
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
