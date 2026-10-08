/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UI_SGI_RENDERER_H
#define UI_SGI_RENDERER_H

#include "qapi/error.h"

#define SGI_RENDERER_SIZE 128
#define SGI_RENDERER_BYTES (SGI_RENDERER_SIZE * SGI_RENDERER_SIZE * 4)

typedef struct SGIRenderer SGIRenderer;

typedef enum SGIRenderCreateResult {
    SGI_RENDER_CREATE_OK,
    SGI_RENDER_CREATE_UNAVAILABLE,
    SGI_RENDER_CREATE_OOM,
    SGI_RENDER_CREATE_HOST_ERROR,
} SGIRenderCreateResult;

typedef struct SGIRendererRequest {
    uint64_t generation;
    float vertices[3][4];
    uint8_t rgb[3];
    int32_t clip[4]; /* Inclusive local ownership rectangle. */
    uint8_t rgba[SGI_RENDERER_BYTES];
} SGIRendererRequest;

#define SGI_RENDERER_FRAME_VERTEX_FLOATS 8
#define SGI_RENDERER_CLEAR_RECTS 16

typedef struct SGIRendererClearRect {
    uint32_t x0, y0, x1, y1; /* Inclusive, bottom-origin plane pixels. */
} SGIRendererClearRect;

typedef struct SGIRendererFrame {
    uint32_t width, height, count;
    const float *vertices; /* position xyzw, then premade colour rgba */
    uint32_t shade_model;
    uint32_t blend, blend_src, blend_dst, blend_equation;
    uint32_t clip_count; /* Zero means the full plane. */
    SGIRendererClearRect clips[SGI_RENDERER_CLEAR_RECTS];
    float modelview[16], projection[16];
    uint16_t *rgba; /* width * height * 4 host slots, bottom row first */
    uint8_t *written; /* width * height binary bytes, bottom row first */
    size_t written_capacity;
} SGIRendererFrame;

typedef struct SGIRendererClear {
    uint32_t width, height;
    uint32_t rect_count;
    SGIRendererClearRect rects[SGI_RENDERER_CLEAR_RECTS];
    float rgba[4];
    uint32_t colour_mask;
    uint16_t *pixels; /* width * height * 4 host slots, bottom row first */
    uint32_t *depth_stencil; /* Packed D24S8 words, bottom row first. */
    uint32_t depth24;
    uint8_t stencil8, stencil_mask;
    bool colour, depth, stencil;
} SGIRendererClear;

typedef struct SGIRendererOps {
    void *(*create)(SGIRenderCreateResult *result, Error **errp);
    bool (*draw)(void *opaque, SGIRendererRequest *request, Error **errp);
    bool (*draw_frame)(void *opaque, SGIRendererFrame *frame, Error **errp);
    bool (*clear)(void *opaque, SGIRendererClear *clear, Error **errp);
    void (*destroy)(void *opaque);
} SGIRendererOps;

void sgi_renderer_register_gl(const SGIRendererOps *ops);
/* Only UNAVAILABLE permits an optional caller to choose another renderer. */
SGIRenderCreateResult sgi_renderer_create(SGIRenderer **renderer,
                                          Error **errp);
/* Legacy wrapper: NULL does not distinguish absence from setup failure. */
SGIRenderer *sgi_renderer_new(Error **errp);
bool sgi_renderer_draw(SGIRenderer *renderer, SGIRendererRequest *request,
                       Error **errp);
bool sgi_renderer_draw_frame(SGIRenderer *renderer, SGIRendererFrame *frame,
                             Error **errp);
bool sgi_renderer_clear(SGIRenderer *renderer, SGIRendererClear *clear,
                        Error **errp);
/*
 * Test-only: fail the nth otherwise successful draw on this renderer.
 * Zero disables injection; arming does not clear a latched failure.
 */
void sgi_renderer_test_fail_draw(SGIRenderer *renderer, unsigned nth_draw);
void sgi_renderer_free(SGIRenderer *renderer);
bool sgi_renderer_request_valid(const SGIRendererRequest *request);
bool sgi_renderer_staging_valid(const SGIRendererRequest *request);

#endif
