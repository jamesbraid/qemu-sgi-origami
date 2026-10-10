/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UI_SGI_RENDERER_H
#define UI_SGI_RENDERER_H

#include "qapi/error.h"

#define SGI_RENDERER_CLIP_PLANES 6
#define SGI_RENDERER_SIZE 128
#define SGI_RENDERER_COORD_LIMIT 16384
#define SGI_RENDERER_BYTES (SGI_RENDERER_SIZE * SGI_RENDERER_SIZE * 4)

typedef struct SGIRenderer SGIRenderer;

typedef enum SGIRenderCreateResult {
    SGI_RENDER_CREATE_OK,
    SGI_RENDER_CREATE_UNAVAILABLE,
    SGI_RENDER_CREATE_OOM,
    SGI_RENDER_CREATE_HOST_ERROR,
} SGIRenderCreateResult;

typedef enum SGIRendererTextureFormat {
    SGI_RENDERER_TEXTURE_ALPHA,
    SGI_RENDERER_TEXTURE_LUMINANCE,
    SGI_RENDERER_TEXTURE_LUMINANCE_ALPHA,
    SGI_RENDERER_TEXTURE_INTENSITY,
    SGI_RENDERER_TEXTURE_RGB,
    SGI_RENDERER_TEXTURE_RGBA,
} SGIRendererTextureFormat;

typedef enum SGIRendererTextureWrap {
    SGI_RENDERER_TEXTURE_REPEAT,
    SGI_RENDERER_TEXTURE_CLAMP,
    SGI_RENDERER_TEXTURE_EDGE,
    SGI_RENDERER_TEXTURE_BORDER,
} SGIRendererTextureWrap;

typedef enum SGIRendererTextureEnvironment {
    SGI_RENDERER_TEXTURE_REPLACE,
    SGI_RENDERER_TEXTURE_MODULATE,
    SGI_RENDERER_TEXTURE_DECAL,
    SGI_RENDERER_TEXTURE_BLEND,
} SGIRendererTextureEnvironment;

/* Owned by the synchronous caller; the worker only reads the snapshot. */
typedef struct SGIRendererTexture {
    uint64_t serial;
    uint32_t width, height;
    SGIRendererTextureFormat format;
    SGIRendererTextureWrap wrap[2];
    SGIRendererTextureEnvironment environment;
    bool linear;
    float border[4], env_colour[4];
    double scale[4], bias[4];
    const uint8_t *texels;
    size_t bytes;
    /* Called on the device thread to check the worker's texture result. */
    bool (*apply)(void *opaque, const double coord[4], double colour[4]);
    void *opaque;
} SGIRendererTexture;

typedef struct SGIRendererRequest {
    uint64_t generation;
    float vertices[3][4];
    uint8_t rgb[3];
    bool smooth;
    double alpha;             /* Flat source alpha, not stored plane alpha. */
    double colour[3][4];      /* Clamped vertex RGBA before interpolation. */
    double inverse_w[3];      /* Reciprocal clip w. */
    const SGIRendererTexture *texture;
    double texcoord[3][4];
    uint32_t clip_planes;
    double clip_distance[3][SGI_RENDERER_CLIP_PLANES];
    double offset;            /* Filled polygon bias in window depth. */
    double depth[3];          /* Normalized depth, linear in window space. */
    int32_t clip[4]; /* Inclusive local rectangle; {0,0,-1,-1} is empty. */
    uint8_t rgba[SGI_RENDERER_BYTES];
} SGIRendererRequest;

#define SGI_RENDERER_FRAME_VERTEX_FLOATS 12
/* Bounds a textured frame, which is one clipped fan. */
#define SGI_RENDERER_FRAME_VERTICES_MAX 9
/* Bounds an untextured frame of independent primitives. */
#define SGI_RENDERER_FRAME_PRIMITIVE_VERTICES_MAX (1u << 18)
#define SGI_RENDERER_CLEAR_RECTS 16

/* OpenGL wrap values accepted at the direct renderer-frame boundary. */
typedef enum SGIRendererFrameTextureWrap {
    SGI_RENDERER_WRAP_REPEAT = 0x2901,
    SGI_RENDERER_WRAP_CLAMP = 0x2900,
    SGI_RENDERER_WRAP_EDGE = 0x812f,
    SGI_RENDERER_WRAP_BORDER = 0x812d,
} SGIRendererFrameTextureWrap;

/* OpenGL texture environment values accepted at the direct frame boundary. */
typedef enum SGIRendererFrameTextureEnvironment {
    SGI_RENDERER_ENV_REPLACE = 0x1e01,
    SGI_RENDERER_ENV_MODULATE = 0x2100,
    SGI_RENDERER_ENV_DECAL = 0x2101,
    SGI_RENDERER_ENV_BLEND = 0x0be2,
} SGIRendererFrameTextureEnvironment;

/* What an untextured frame's vertices draw; zero keeps triangles. */
typedef enum SGIRendererPrimitive {
    SGI_RENDERER_TRIANGLES,
    SGI_RENDERER_POINTS,
    SGI_RENDERER_WINDOW_TRIANGLES,
    SGI_RENDERER_LINES,
} SGIRendererPrimitive;

/*
 * Window triangle frames draw in window pixels mapped through the fixed
 * viewport (-SGI_RENDERER_WINDOW_REACH, -reach, 4 * reach, 4 * reach),
 * whose power-of-two scale keeps every vertex on the 1/256 grid exact.
 * Rows are mapped as the frame's other rows are, top first.
 */
#define SGI_RENDERER_WINDOW_REACH 4096
#define SGI_RENDERER_WINDOW_TRIANGLES_MAX 65536

/*
 * One triangle as the scalar rasterizer scans it: snapped window positions
 * in 1/256 subpixels, wound counter-clockwise, each vertex's normalized
 * depth, 1 / clip w and clamped colour. Each fragment's depth and colour
 * come from its centre's barycentric weights, the colour corrected by
 * 1 / w, computed in double; a depth outside [0, 1] drops the fragment.
 * In a flat-shaded frame every vertex carries the provoking colour, which
 * is used as it is.
 */
typedef struct SGIRendererTriangle {
    double x[3], y[3];
    double z[3];
    double inv_w[3];
    float colour[3][4];
} SGIRendererTriangle;

/*
 * Line frames draw in window pixels mapped through the fixed viewport
 * (-SGI_RENDERER_LINE_REACH, -SGI_RENDERER_LINE_REACH, 4 * reach,
 * 4 * reach), whose power-of-two scale keeps every vertex on the 1/256
 * grid exact. Vertices must lie strictly inside it.
 */
#define SGI_RENDERER_LINE_REACH 4096
#define SGI_RENDERER_LINE_SEGMENTS_MAX 65536

/*
 * One aliased segment as the scalar rasterizer draws it: a parallelogram
 * covering the width band around the segment, of which only major-axis
 * steps lo to hi draw. Each fragment's colour comes from its centre's
 * clamped parameter along a to b, weighted by 1 / clip w. A flat segment
 * takes colour b.
 */
typedef struct SGIRendererLine {
    float quad[4][2];       /* window pixels, a convex parallelogram */
    double a[2], b[2];      /* snapped endpoints, window subpixels of 1/256 */
    double inv_w[2];
    float colour[2][4];     /* clamped to [0, 1] */
    int32_t lo, hi;
    bool x_major;
    bool reverse;           /* the segment runs down the major axis */
    uint32_t counter;       /* stipple counter at the first base pixel */
} SGIRendererLine;

typedef struct SGIRendererTexture2D {
    uint32_t width, height;
    uint32_t env_mode, wrap_s, wrap_t;
    bool linear_filter;
    uint8_t component_bits;
    double border_bytes[4];
    double environment_bytes[4];
    const uint32_t *slots; /* Raw RGBA slots, bottom row first. */
    size_t capacity_slots;
} SGIRendererTexture2D;

typedef struct SGIRendererClearRect {
    uint32_t x0, y0, x1, y1; /* Inclusive, bottom-origin plane pixels. */
} SGIRendererClearRect;

typedef struct SGIRendererFrame {
    /* A textured frame is a clipped fan, others independent primitives. */
    uint32_t width, height, count;
    const float *vertices; /* position xyzw, colour rgba, texcoord strq */
    SGIRendererTexture2D texture;
    uint32_t primitive; /* SGIRendererPrimitive; textured frames are fans */
    float point_size; /* Aliased integer size; zero means one. */
    /* A window triangle frame's triangles, count of them. */
    const SGIRendererTriangle *triangles;
    /* A line frame's segments, count of them; vertices are unused. */
    const SGIRendererLine *lines;
    bool line_stipple;
    uint32_t line_stipple_repeat, line_stipple_pattern;
    uint32_t shade_model;
    uint32_t blend, blend_src, blend_dst, blend_equation;
    /*
     * Untextured fragment colour steps per component, red to alpha,
     * applied before blending; zero leaves that component unrounded.
     */
    uint32_t colour_steps[4];
    bool viewport_given;
    int32_t viewport[4]; /* Guest viewport in native plane pixels. */
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
    bool (*supports_texture)(void *opaque, Error **errp);
    bool (*supports_viewport)(void *opaque, const int32_t viewport[4],
                              Error **errp);
    bool (*supports_primitive)(void *opaque, uint32_t primitive, float size,
                               Error **errp);
    bool (*clear)(void *opaque, SGIRendererClear *clear, Error **errp);
    void (*destroy)(void *opaque);
} SGIRendererOps;

/*
 * A refused draw staged an image that differs from the software model; the
 * renderer stays usable and the caller draws in software. A failed draw was
 * malformed, or its backend failed or changed the request inputs.
 */
typedef enum SGIRenderDrawResult {
    SGI_RENDER_DRAW_OK,
    SGI_RENDER_DRAW_REFUSED,
    SGI_RENDER_DRAW_FAILED,
} SGIRenderDrawResult;

void sgi_renderer_register_gl(const SGIRendererOps *ops);
/* Only UNAVAILABLE permits an optional caller to choose another renderer. */
SGIRenderCreateResult sgi_renderer_create(SGIRenderer **renderer,
                                          Error **errp);
/* Legacy wrapper: NULL does not distinguish absence from setup failure. */
SGIRenderer *sgi_renderer_new(Error **errp);
SGIRenderDrawResult sgi_renderer_draw_checked(SGIRenderer *renderer,
                                              SGIRendererRequest *request,
                                              Error **errp);
bool sgi_renderer_draw(SGIRenderer *renderer, SGIRendererRequest *request,
                       Error **errp);
bool sgi_renderer_draw_frame(SGIRenderer *renderer, SGIRendererFrame *frame,
                             Error **errp);
bool sgi_renderer_texture_supported(SGIRenderer *renderer, Error **errp);
/* False when the host cannot map a viewport exactly; no failure latches. */
bool sgi_renderer_viewport_supported(SGIRenderer *renderer,
                                     const int32_t viewport[4], Error **errp);
/*
 * The shared draw code's cached line probe outcome for this renderer:
 * zero until it runs, positive when it passed, negative when it failed.
 */
int sgi_renderer_line_probe_result(SGIRenderer *renderer);
void sgi_renderer_set_line_probe_result(SGIRenderer *renderer, int result);
/* The same for the smooth triangle probe. */
int sgi_renderer_window_probe_result(SGIRenderer *renderer);
void sgi_renderer_set_window_probe_result(SGIRenderer *renderer, int result);
/* False when the host cannot draw the primitive at this aliased size. */
bool sgi_renderer_primitive_supported(SGIRenderer *renderer,
                                      uint32_t primitive, float size,
                                      Error **errp);
bool sgi_renderer_clear(SGIRenderer *renderer, SGIRendererClear *clear,
                        Error **errp);
/* All requests remain staged until the caller commits a successful batch. */
SGIRenderDrawResult sgi_renderer_draw_batch_checked(
    SGIRenderer *renderer, SGIRendererRequest *requests, unsigned count,
    Error **errp);
bool sgi_renderer_draw_batch(SGIRenderer *renderer,
                             SGIRendererRequest *requests, unsigned count,
                             Error **errp);
/*
 * Test-only: fail the nth otherwise successful draw on this renderer.
 * Zero disables injection; arming does not clear a latched failure.
 */
void sgi_renderer_test_fail_draw(SGIRenderer *renderer, unsigned nth_draw);
void sgi_renderer_free(SGIRenderer *renderer);
bool sgi_renderer_request_valid(const SGIRendererRequest *request);
bool sgi_renderer_staging_valid(const SGIRendererRequest *request);
/* Source colour at a covered pixel, before fragment operations. */
bool sgi_renderer_fragment(const SGIRendererRequest *request, unsigned x,
                            unsigned y, double colour[4]);
bool sgi_renderer_depth(const SGIRendererRequest *request, unsigned x,
                         unsigned y, double *depth);
bool sgi_renderer_request_cull(SGIRendererRequest *request, bool clockwise,
                                bool cull_front, bool cull_back);
/* Translate snapped window vertices and inclusive ownership into one tile. */
bool sgi_renderer_request_tile(SGIRendererRequest *request,
                                int64_t x, int64_t y, unsigned width,
                                unsigned height,
                                const int64_t bounds[4]);

#endif
