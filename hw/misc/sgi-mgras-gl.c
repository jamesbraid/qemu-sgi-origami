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
 * What it preserves: the pixels a colour clear leaves in the window, and
 * the main-port framing, so
 * the words after a packet still parse as headers. What it does not: the
 * raster-engine register writes the GE would make, the timing of the
 * work, any GE state visible to a later readback, and every command
 * outside the subset below, which the parser logs through LOG_UNIMP and
 * consumes by length as before.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/sgi-mgras-gl.h"

/* Class-0 opcodes, bits 15:8 of the header, with their byte lengths. */
#define MGRAS_GL_CLEAR_COLOR_BUF 0x00001500
#define MGRAS_GL_FLUSH           0x00003600
#define MGRAS_GL_CLEAR_COLOR     0x0000ba10

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

/* Screen columns, and rows counted up from the bottom of the screen. */
typedef struct MgrasGLWindow {
    int ox, oy;           /* the window's lower-left pixel */
    int x0, y0, x1, y1;   /* the pixels it owns, inclusive */
} MgrasGLWindow;

bool sgi_mgras_gl_header(uint32_t header)
{
    switch (header) {
    case MGRAS_GL_CLEAR_COLOR_BUF:
    case MGRAS_GL_FLUSH:
    case MGRAS_GL_CLEAR_COLOR:
        return true;
    default:
        return false;
    }
}

/* OpenGL 1.2.1 initial state (4.2.3). */
void sgi_mgras_gl_reset(SGIMgrasState *s)
{
    memset(s->gl_clear_color, 0, sizeof(s->gl_clear_color));
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

void sgi_mgras_gl_execute(SGIMgrasState *s, uint32_t header,
                          const uint32_t *payload, uint32_t words)
{
    if (words != (header & 0xff) / sizeof(uint32_t)) {
        return;
    }
    switch (header) {
    case MGRAS_GL_CLEAR_COLOR:
        memcpy(s->gl_clear_color, payload, sizeof(s->gl_clear_color));
        return;
    case MGRAS_GL_CLEAR_COLOR_BUF:
        mgras_gl_clear(s);
        return;
    case MGRAS_GL_FLUSH:
        /* Approximated: every command has already run to completion. */
        return;
    default:
        g_assert_not_reached();
    }
}
