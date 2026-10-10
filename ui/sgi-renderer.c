/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/module.h"
#include "ui/sgi-renderer.h"

struct SGIRenderer {
    const SGIRendererOps *ops;
    void *opaque;
    bool failed;
    unsigned test_draw_countdown;
    int line_probe;
};

static const SGIRendererOps *gl_ops;

static bool sgi_renderer_texture_wrap_valid(uint32_t wrap)
{
    return wrap == SGI_RENDERER_WRAP_REPEAT ||
           wrap == SGI_RENDERER_WRAP_CLAMP ||
           wrap == SGI_RENDERER_WRAP_EDGE ||
           wrap == SGI_RENDERER_WRAP_BORDER;
}

static bool sgi_renderer_texture_env_valid(uint32_t mode)
{
    return mode == SGI_RENDERER_ENV_REPLACE ||
           mode == SGI_RENDERER_ENV_MODULATE ||
           mode == SGI_RENDERER_ENV_DECAL ||
           mode == SGI_RENDERER_ENV_BLEND;
}

/* Whole independent primitives within the frame's vertex bound. */
static bool sgi_renderer_frame_count_valid(const SGIRendererFrame *frame)
{
    if (frame->texture.slots) {
        return frame->primitive == SGI_RENDERER_TRIANGLES &&
               frame->count >= 3 &&
               frame->count <= SGI_RENDERER_FRAME_VERTICES_MAX;
    }
    if (frame->primitive == SGI_RENDERER_WINDOW_TRIANGLES) {
        const double reach = (double)SGI_RENDERER_WINDOW_REACH * 256;

        if (!frame->triangles || !frame->count ||
            frame->count > SGI_RENDERER_WINDOW_TRIANGLES_MAX) {
            return false;
        }
        /* Inside the fixed viewport, whichever way the rows run. */
        for (uint32_t i = 0; i < frame->count; i++) {
            const SGIRendererTriangle *t = &frame->triangles[i];

            for (unsigned v = 0; v < 3; v++) {
                double y = frame->height * 256.0 - t->y[v];

                if (!(t->x[v] > -reach && t->x[v] < 3 * reach &&
                      t->y[v] > -reach && t->y[v] < 3 * reach &&
                      y > -reach && y < 3 * reach) ||
                    !(t->z[v] >= 0 && t->z[v] <= 1)) {
                    return false;
                }
            }
        }
    } else if (frame->primitive == SGI_RENDERER_LINES) {
        if (!frame->lines || !frame->count ||
            frame->count > SGI_RENDERER_LINE_SEGMENTS_MAX ||
            (frame->line_stipple &&
             (!frame->line_stipple_repeat ||
              frame->line_stipple_repeat > 256))) {
            return false;
        }
        for (uint32_t i = 0; i < frame->count; i++) {
            const SGIRendererLine *l = &frame->lines[i];

            if (l->lo > l->hi) {
                return false;
            }
            for (unsigned v = 0; v < 4; v++) {
                for (unsigned k = 0; k < 2; k++) {
                    if (!(fabsf(l->quad[v][k]) < SGI_RENDERER_LINE_REACH * 3)) {
                        return false;
                    }
                }
            }
        }
    } else if (!frame->vertices || !frame->count ||
        frame->count > SGI_RENDERER_FRAME_PRIMITIVE_VERTICES_MAX ||
        !isfinite(frame->point_size) || frame->point_size < 0) {
        return false;
    }
    for (unsigned c = 0; c < 4; c++) {
        if (frame->colour_steps[c] > UINT16_MAX) {
            return false;
        }
    }
    switch (frame->primitive) {
    case SGI_RENDERER_TRIANGLES:
        return frame->count % 3 == 0;
    case SGI_RENDERER_POINTS:
    case SGI_RENDERER_WINDOW_TRIANGLES:
    case SGI_RENDERER_LINES:
        return true;
    default:
        return false;
    }
}

void sgi_renderer_register_gl(const SGIRendererOps *ops)
{
    assert(ops && ops->create && ops->draw && ops->destroy);
    assert(!gl_ops || gl_ops == ops);
    gl_ops = ops;
}

SGIRenderCreateResult sgi_renderer_create(SGIRenderer **out, Error **errp)
{
    Error *local_err = NULL;
    Error *backend_err = NULL;
    SGIRenderer *renderer;
    SGIRenderCreateResult backend_result = SGI_RENDER_CREATE_HOST_ERROR;
    int loaded = 2;

    if (!out) {
        error_setg(errp, "Missing SGI OpenGL renderer output");
        return SGI_RENDER_CREATE_HOST_ERROR;
    }
    *out = NULL;

    if (!gl_ops) {
        loaded = ui_module_load("opengl", &local_err);
    }
    if (!gl_ops) {
        bool unavailable = !local_err && loaded == 0;

#if !defined(CONFIG_MODULES) && !defined(CONFIG_OPENGL)
        unavailable = !local_err;
#endif
        if (!local_err) {
            error_setg(&local_err, unavailable ?
                       "SGI OpenGL renderer is unavailable" :
                       "SGI OpenGL module did not register a renderer");
        }
        error_propagate(errp, local_err);
        return unavailable ? SGI_RENDER_CREATE_UNAVAILABLE :
                             SGI_RENDER_CREATE_HOST_ERROR;
    }
    error_free(local_err);
    renderer = g_try_new0(SGIRenderer, 1);
    if (!renderer) {
        error_setg(errp, "Cannot allocate SGI OpenGL renderer");
        return SGI_RENDER_CREATE_OOM;
    }
    renderer->ops = gl_ops;
    renderer->opaque = gl_ops->create(&backend_result, &backend_err);
    if (!renderer->opaque || backend_result != SGI_RENDER_CREATE_OK ||
        backend_err) {
        bool inconsistent = renderer->opaque ||
                            backend_result == SGI_RENDER_CREATE_OK;

        if (renderer->opaque) {
            gl_ops->destroy(renderer->opaque);
        }
        g_free(renderer);
        if (inconsistent || (backend_result != SGI_RENDER_CREATE_UNAVAILABLE &&
                             backend_result != SGI_RENDER_CREATE_OOM &&
                             backend_result != SGI_RENDER_CREATE_HOST_ERROR)) {
            backend_result = SGI_RENDER_CREATE_HOST_ERROR;
        }
        if (!backend_err) {
            error_setg(&backend_err, "SGI OpenGL backend creation failed");
        }
        error_propagate(errp, backend_err);
        return backend_result;
    }
    *out = renderer;
    return SGI_RENDER_CREATE_OK;
}

SGIRenderer *sgi_renderer_new(Error **errp)
{
    SGIRenderer *renderer = NULL;

    sgi_renderer_create(&renderer, errp);
    return renderer;
}

bool sgi_renderer_request_valid(const SGIRendererRequest *request)
{
    if (!isfinite(request->alpha) || !isfinite(request->offset) ||
        request->clip_planes >> SGI_RENDERER_CLIP_PLANES) {
        return false;
    }
    if (request->texture) {
        const SGIRendererTexture *t = request->texture;

        if (!t->width || !t->height || t->width > INT32_MAX ||
            t->height > INT32_MAX || !t->texels || !t->apply ||
            t->bytes != (uint64_t)t->width * t->height * 4 ||
            (unsigned)t->format > SGI_RENDERER_TEXTURE_RGBA ||
            (unsigned)t->wrap[0] > SGI_RENDERER_TEXTURE_BORDER ||
            (unsigned)t->wrap[1] > SGI_RENDERER_TEXTURE_BORDER ||
            (unsigned)t->environment > SGI_RENDERER_TEXTURE_BLEND ||
            (t->environment == SGI_RENDERER_TEXTURE_DECAL &&
             t->format != SGI_RENDERER_TEXTURE_RGB &&
             t->format != SGI_RENDERER_TEXTURE_RGBA)) {
            return false;
        }
        for (unsigned c = 0; c < 4; c++) {
            if (!(t->border[c] >= 0 && t->border[c] <= 1) ||
                !(t->env_colour[c] >= 0 && t->env_colour[c] <= 1) ||
                !isfinite(t->scale[c]) || !isfinite(t->bias[c])) {
                return false;
            }
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        const float *vertex = request->vertices[i];

        if (!(request->depth[i] >= 0 && request->depth[i] <= 1)) {
            return false;
        }
        if (vertex[2] != 0 || vertex[3] != 1) {
            return false;
        }
        if ((request->smooth || request->texture || request->clip_planes) &&
            (!isfinite(request->inverse_w[i]) || request->inverse_w[i] <= 0)) {
            return false;
        }
        for (unsigned p = 0; p < SGI_RENDERER_CLIP_PLANES; p++) {
            if ((request->clip_planes & (1u << p)) &&
                !isfinite(request->clip_distance[i][p])) {
                return false;
            }
        }
        if (request->smooth || request->texture) {
            for (unsigned c = 0; c < 4; c++) {
                if (!(request->colour[i][c] >= 0 &&
                      request->colour[i][c] <= 1)) {
                    return false;
                }
            }
        }
        if (request->texture) {
            for (unsigned c = 0; c < 4; c++) {
                if (!isfinite(request->texcoord[i][c])) {
                    return false;
                }
            }
            if (request->texcoord[i][3] <= 0) {
                return false;
            }
        }
        for (unsigned axis = 0; axis < 2; axis++) {
            double subpixel = vertex[axis] * 256.0;

            if (!isfinite(subpixel) ||
                fabs(vertex[axis]) > SGI_RENDERER_COORD_LIMIT ||
                subpixel != floor(subpixel)) {
                return false;
            }
        }
    }
    if (request->clip[0] == 0 && request->clip[1] == 0 &&
        request->clip[2] == -1 && request->clip[3] == -1) {
        return true;
    }
    if (request->clip[0] < 0 || request->clip[1] < 0 ||
        request->clip[2] >= SGI_RENDERER_SIZE ||
        request->clip[3] >= SGI_RENDERER_SIZE ||
        request->clip[0] > request->clip[2] ||
        request->clip[1] > request->clip[3]) {
        return false;
    }
    return true;
}

bool sgi_renderer_request_tile(SGIRendererRequest *request,
                                int64_t x, int64_t y, unsigned width,
                                unsigned height,
                                const int64_t bounds[4])
{
    int64_t x0, y0, x1, y1;

    if (!width || !height || width > SGI_RENDERER_SIZE ||
        height > SGI_RENDERER_SIZE || x > INT64_MAX - SGI_RENDERER_SIZE ||
        y > INT64_MAX - SGI_RENDERER_SIZE) {
        return false;
    }
    for (unsigned i = 0; i < 3; i++) {
        request->vertices[i][0] = (double)request->vertices[i][0] - x;
        request->vertices[i][1] = (double)request->vertices[i][1] - y;
    }
    x0 = MAX(bounds[0], x);
    y0 = MAX(bounds[1], y);
    x1 = MIN(bounds[2], x + width - 1);
    y1 = MIN(bounds[3], y + height - 1);
    if (x0 > x1 || y0 > y1) {
        request->clip[0] = request->clip[1] = 0;
        request->clip[2] = request->clip[3] = -1;
    } else {
        request->clip[0] = x0 - x;
        request->clip[1] = y0 - y;
        request->clip[2] = x1 - x;
        request->clip[3] = y1 - y;
    }
    return sgi_renderer_request_valid(request);
}

static int64_t renderer_edge(const int64_t a[2], const int64_t b[2],
                             int64_t x, int64_t y)
{
    int64_t dx = b[0] - a[0], dy = b[1] - a[1];

    return dx * (y - a[1]) - dy * (x - a[0]);
}

static int64_t renderer_triangle(const SGIRendererRequest *request,
                                  int64_t vertices[3][2])
{
    for (unsigned i = 0; i < 3; i++) {
        for (unsigned axis = 0; axis < 2; axis++) {
            vertices[i][axis] = request->vertices[i][axis] * 256;
        }
    }
    return renderer_edge(vertices[0], vertices[1],
                         vertices[2][0], vertices[2][1]);
}

bool sgi_renderer_request_cull(SGIRendererRequest *request, bool clockwise,
                                bool cull_front, bool cull_back)
{
    int64_t vertices[3][2], area;
    bool front;

    if (!sgi_renderer_request_valid(request)) {
        return false;
    }
    area = renderer_triangle(request, vertices);
    front = clockwise ? area < 0 : area > 0;
    if (front ? cull_front : cull_back) {
        request->clip[0] = request->clip[1] = 0;
        request->clip[2] = request->clip[3] = -1;
    }
    return true;
}

/* Match the CPU model's bottom-origin top-left coverage and 8-bit grid. */
static bool renderer_covered(const int64_t vertices[3][2], int64_t area,
                             unsigned x, unsigned y)
{
    if (!area) {
        return false;
    }
    for (unsigned i = 0; i < 3; i++) {
        const int64_t *a = vertices[i], *b = vertices[(i + 1) % 3];
        int64_t edge = renderer_edge(a, b, x * 256 + 128, y * 256 + 128);

        if (edge < 0 || (!edge && !(b[1] < a[1] ||
                                  (b[1] == a[1] && b[0] < a[0])))) {
            return false;
        }
    }
    return true;
}

static bool renderer_user_clipped(const SGIRendererRequest *request,
                                   const int64_t vertices[3][2], int64_t area,
                                   const unsigned indices[3], unsigned x,
                                   unsigned y)
{
    double b1, b2, weights[3], sum;

    if (!request->clip_planes) {
        return false;
    }
    b1 = (double)renderer_edge(vertices[2], vertices[0],
                                x * 256 + 128, y * 256 + 128) / area;
    b2 = (double)renderer_edge(vertices[0], vertices[1],
                                x * 256 + 128, y * 256 + 128) / area;
    weights[0] = (1.0 - b1 - b2) * request->inverse_w[indices[0]];
    weights[1] = b1 * request->inverse_w[indices[1]];
    weights[2] = b2 * request->inverse_w[indices[2]];
    sum = weights[0] + weights[1] + weights[2];
    if (!isfinite(sum) || sum <= 0) {
        return true;
    }
    for (unsigned p = 0; p < SGI_RENDERER_CLIP_PLANES; p++) {
        if (request->clip_planes & (1u << p)) {
            double distance =
                (weights[0] * request->clip_distance[indices[0]][p] +
                 weights[1] * request->clip_distance[indices[1]][p] +
                 weights[2] * request->clip_distance[indices[2]][p]) / sum;

            if (!(distance >= 0)) {
                return true;
            }
        }
    }
    return false;
}

static bool renderer_fragment(const SGIRendererRequest *request,
                               const int64_t vertices[3][2], int64_t area,
                               const unsigned indices[3], unsigned x,
                               unsigned y, double colour[4])
{
    double b1, b2, weights[3], sum;

    if ((int32_t)x < request->clip[0] || (int32_t)x > request->clip[2] ||
        (int32_t)y < request->clip[1] || (int32_t)y > request->clip[3] ||
        !renderer_covered(vertices, area, x, y) ||
        renderer_user_clipped(request, vertices, area, indices, x, y)) {
        return false;
    }
    if (!request->smooth && !request->texture) {
        for (unsigned c = 0; c < 3; c++) {
            colour[c] = request->rgb[c] / 255.0;
        }
        colour[3] = request->alpha;
        return true;
    }
    b1 = (double)renderer_edge(vertices[2], vertices[0],
                                x * 256 + 128, y * 256 + 128) / area;
    b2 = (double)renderer_edge(vertices[0], vertices[1],
                                x * 256 + 128, y * 256 + 128) / area;
    weights[0] = (1.0 - b1 - b2) * request->inverse_w[indices[0]];
    weights[1] = b1 * request->inverse_w[indices[1]];
    weights[2] = b2 * request->inverse_w[indices[2]];
    sum = weights[0] + weights[1] + weights[2];
    if (!isfinite(sum) || sum <= 0) {
        return false;
    }
    for (unsigned c = 0; c < 4; c++) {
        colour[c] = request->smooth ?
            (weights[0] * request->colour[indices[0]][c] +
             weights[1] * request->colour[indices[1]][c] +
             weights[2] * request->colour[indices[2]][c]) / sum :
            request->colour[0][c];
        if (!isfinite(colour[c])) {
            return false;
        }
    }
    if (request->texture) {
        double coord[4];

        for (unsigned c = 0; c < 4; c++) {
            coord[c] = (weights[0] * request->texcoord[indices[0]][c] +
                        weights[1] * request->texcoord[indices[1]][c] +
                        weights[2] * request->texcoord[indices[2]][c]) / sum;
        }
        if (!request->texture->apply(request->texture->opaque, coord, colour)) {
            return false;
        }
        for (unsigned c = 0; c < 4; c++) {
            if (!isfinite(colour[c])) {
                return false;
            }
        }
    }
    return true;
}

static int64_t renderer_positive_triangle(const SGIRendererRequest *request,
                                           int64_t vertices[3][2],
                                           unsigned indices[3])
{
    int64_t area = renderer_triangle(request, vertices);

    indices[0] = 0;
    indices[1] = area < 0 ? 2 : 1;
    indices[2] = area < 0 ? 1 : 2;
    if (area < 0) {
        for (unsigned axis = 0; axis < 2; axis++) {
            int64_t tmp = vertices[1][axis];

            vertices[1][axis] = vertices[2][axis];
            vertices[2][axis] = tmp;
        }
        area = -area;
    }
    return area;
}

bool sgi_renderer_fragment(const SGIRendererRequest *request, unsigned x,
                            unsigned y, double colour[4])
{
    int64_t vertices[3][2], area;
    unsigned indices[3];

    if (x >= SGI_RENDERER_SIZE || y >= SGI_RENDERER_SIZE ||
        !sgi_renderer_request_valid(request)) {
        return false;
    }
    area = renderer_positive_triangle(request, vertices, indices);
    return renderer_fragment(request, vertices, area, indices, x, y, colour);
}

bool sgi_renderer_depth(const SGIRendererRequest *request, unsigned x,
                         unsigned y, double *depth)
{
    int64_t vertices[3][2], area;
    unsigned indices[3];
    double b1, b2;

    if (x >= SGI_RENDERER_SIZE || y >= SGI_RENDERER_SIZE ||
        !sgi_renderer_request_valid(request)) {
        return false;
    }
    area = renderer_positive_triangle(request, vertices, indices);
    if ((int32_t)x < request->clip[0] || (int32_t)x > request->clip[2] ||
        (int32_t)y < request->clip[1] || (int32_t)y > request->clip[3] ||
        !renderer_covered(vertices, area, x, y) ||
        renderer_user_clipped(request, vertices, area, indices, x, y)) {
        return false;
    }
    b1 = (double)renderer_edge(vertices[2], vertices[0],
                                x * 256 + 128, y * 256 + 128) / area;
    b2 = (double)renderer_edge(vertices[0], vertices[1],
                                x * 256 + 128, y * 256 + 128) / area;
    *depth = request->depth[indices[0]] +
             b1 * (request->depth[indices[1]] - request->depth[indices[0]]) +
             b2 * (request->depth[indices[2]] - request->depth[indices[0]]);
    return true;
}

bool sgi_renderer_staging_valid(const SGIRendererRequest *request)
{
    int64_t vertices[3][2], area;
    unsigned indices[3];

    if (!sgi_renderer_request_valid(request)) {
        return false;
    }
    area = renderer_positive_triangle(request, vertices, indices);
    for (unsigned y = 0; y < SGI_RENDERER_SIZE; y++) {
        for (unsigned x = 0; x < SGI_RENDERER_SIZE; x++) {
            double colour[4];
            bool covered = (int32_t)x >= request->clip[0] &&
                           (int32_t)x <= request->clip[2] &&
                           (int32_t)y >= request->clip[1] &&
                           (int32_t)y <= request->clip[3] &&
                           renderer_covered(vertices, area, x, y) &&
                           !renderer_user_clipped(request, vertices, area,
                                                   indices, x, y);
            const uint8_t *pixel = request->rgba +
                                  (y * SGI_RENDERER_SIZE + x) * 4;

            if (covered && !renderer_fragment(request, vertices, area, indices,
                                               x, y, colour)) {
                return false;
            }
            if (pixel[3] != (covered ? 255 : 0)) {
                return false;
            }
            for (unsigned c = 0; c < 3; c++) {
                uint8_t expected = covered ?
                    floor(CLAMP(colour[c], 0, 1) * 255 + 0.5) : 0;

                if (pixel[c] != expected) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool sgi_renderer_test_draw_complete(SGIRenderer *renderer,
                                            Error **errp)
{
    if (renderer->test_draw_countdown && !--renderer->test_draw_countdown) {
        renderer->failed = true;
        error_setg(errp, "Injected SGI renderer test draw failure");
        return false;
    }
    return true;
}

SGIRenderDrawResult sgi_renderer_draw_checked(SGIRenderer *renderer,
                                              SGIRendererRequest *request,
                                              Error **errp)
{
    uint64_t generation = request->generation;
    uint8_t rgb[3];
    float vertices[3][4];
    int32_t clip[4];
    bool smooth = request->smooth;
    uint32_t clip_planes = request->clip_planes;
    double clip_distance[3][SGI_RENDERER_CLIP_PLANES];
    double alpha = request->alpha, offset = request->offset;
    double colour[3][4], inverse_w[3], depth[3], texcoord[3][4];
    const SGIRendererTexture *texture = request->texture;
    SGIRendererTexture texture_inputs;

    if (texture) {
        memcpy(&texture_inputs, texture, sizeof(texture_inputs));
    }
    memcpy(texcoord, request->texcoord, sizeof(texcoord));
    memcpy(clip_distance, request->clip_distance, sizeof(clip_distance));

    memcpy(depth, request->depth, sizeof(depth));
    memcpy(colour, request->colour, sizeof(colour));
    memcpy(inverse_w, request->inverse_w, sizeof(inverse_w));
    memcpy(rgb, request->rgb, sizeof(rgb));
    memcpy(clip, request->clip, sizeof(clip));
    memcpy(vertices, request->vertices, sizeof(vertices));
    if (renderer->failed) {
        error_setg(errp, "SGI OpenGL renderer has failed");
        return SGI_RENDER_DRAW_FAILED;
    }
    if (!sgi_renderer_request_valid(request)) {
        error_setg(errp, "Unsupported SGI OpenGL renderer request");
        return SGI_RENDER_DRAW_FAILED;
    }
    if (!renderer->ops->draw(renderer->opaque, request, errp)) {
        renderer->failed = true;
        return SGI_RENDER_DRAW_FAILED;
    }
    if (request->generation != generation ||
        memcmp(request->rgb, rgb, sizeof(rgb)) ||
        memcmp(request->clip, clip, sizeof(clip)) ||
        memcmp(request->vertices, vertices, sizeof(vertices)) ||
        request->smooth != smooth || request->alpha != alpha ||
        request->offset != offset ||
        memcmp(request->colour, colour, sizeof(colour)) ||
        memcmp(request->inverse_w, inverse_w, sizeof(inverse_w)) ||
        memcmp(request->depth, depth, sizeof(depth)) ||
        memcmp(request->texcoord, texcoord, sizeof(texcoord)) ||
        request->texture != texture ||
        request->clip_planes != clip_planes ||
        memcmp(request->clip_distance, clip_distance, sizeof(clip_distance)) ||
        (texture && memcmp(texture, &texture_inputs, sizeof(texture_inputs)))) {
        renderer->failed = true;
        error_setg(errp, "SGI OpenGL renderer changed its request inputs");
        return SGI_RENDER_DRAW_FAILED;
    }
    /*
     * A host precision difference is refused, not a backend failure. So
     * is a fragment the software model cannot evaluate: the scalar redraw
     * meets the same deterministic result.
     */
    if (!sgi_renderer_staging_valid(request)) {
        error_setg(errp, "Invalid SGI OpenGL renderer staging");
        return SGI_RENDER_DRAW_REFUSED;
    }
    return sgi_renderer_test_draw_complete(renderer, errp) ?
           SGI_RENDER_DRAW_OK : SGI_RENDER_DRAW_FAILED;
}

bool sgi_renderer_draw(SGIRenderer *renderer, SGIRendererRequest *request,
                       Error **errp)
{
    return sgi_renderer_draw_checked(renderer, request, errp) ==
           SGI_RENDER_DRAW_OK;
}

bool sgi_renderer_draw_frame(SGIRenderer *renderer, SGIRendererFrame *frame,
                             Error **errp)
{
    if (!renderer || !frame || !frame->rgba || !frame->written ||
        !frame->width || !frame->height || frame->width > INT_MAX ||
        frame->height > INT_MAX || !sgi_renderer_frame_count_valid(frame) ||
        frame->width > SIZE_MAX / frame->height / 4 /
                       sizeof(*frame->rgba) ||
        frame->written_capacity < (size_t)frame->width * frame->height ||
        frame->clip_count > SGI_RENDERER_CLEAR_RECTS ||
        (frame->viewport_given &&
         (frame->viewport[2] < 0 || frame->viewport[3] < 0))) {
        error_setg(errp, "Invalid SGI OpenGL frame request");
        return false;
    }
    if (frame->viewport_given &&
        !sgi_renderer_viewport_supported(renderer, frame->viewport, errp)) {
        return false;
    }
    if ((frame->primitive == SGI_RENDERER_POINTS ||
         frame->primitive == SGI_RENDERER_WINDOW_TRIANGLES ||
         frame->primitive == SGI_RENDERER_LINES) &&
        !sgi_renderer_primitive_supported(renderer, frame->primitive,
                                          frame->point_size, errp)) {
        return false;
    }
    if (frame->texture.slots &&
        (!frame->texture.width || !frame->texture.height ||
         frame->texture.width > INT_MAX ||
         frame->texture.height > INT_MAX ||
         frame->texture.width > SIZE_MAX / frame->texture.height / 4 /
                                sizeof(*frame->texture.slots) ||
         frame->texture.component_bits != 8 ||
         frame->texture.capacity_slots <
             (size_t)frame->texture.width * frame->texture.height * 4)) {
        error_setg(errp, "Invalid SGI OpenGL texture request");
        return false;
    }
    if (frame->texture.slots) {
        if (!sgi_renderer_texture_env_valid(frame->texture.env_mode)) {
            error_setg(errp, "Invalid SGI OpenGL texture environment");
            return false;
        }
        if (!sgi_renderer_texture_wrap_valid(frame->texture.wrap_s) ||
            !sgi_renderer_texture_wrap_valid(frame->texture.wrap_t)) {
            error_setg(errp, "Invalid SGI OpenGL texture wrap");
            return false;
        }
        for (unsigned c = 0; c < 4; c++) {
            double value = frame->texture.border_bytes[c];

            if (!isfinite(value) || value < 0 || value > 255) {
                error_setg(errp, "Invalid SGI OpenGL texture border colour");
                return false;
            }
            value = frame->texture.environment_bytes[c];
            if (!isfinite(value) || value < 0 || value > 255) {
                error_setg(errp,
                           "Invalid SGI OpenGL texture environment colour");
                return false;
            }
        }
    }
    for (unsigned i = 0; i < frame->clip_count; i++) {
        const SGIRendererClearRect *rect = &frame->clips[i];

        if (rect->x0 > rect->x1 || rect->y0 > rect->y1 ||
            rect->x1 >= frame->width || rect->y1 >= frame->height) {
            error_setg(errp, "Invalid SGI OpenGL frame clip rectangle");
            return false;
        }
    }
    if (renderer->failed) {
        error_setg(errp, "SGI OpenGL renderer has failed");
        return false;
    }
    if (!renderer->ops->draw_frame) {
        error_setg(errp, "SGI OpenGL frame rendering is unavailable");
        return false;
    }
    if (!renderer->ops->draw_frame(renderer->opaque, frame, errp)) {
        renderer->failed = true;
        return false;
    }
    return sgi_renderer_test_draw_complete(renderer, errp);
}

bool sgi_renderer_texture_supported(SGIRenderer *renderer, Error **errp)
{
    if (!renderer || !renderer->ops->supports_texture ||
        !renderer->ops->supports_texture(renderer->opaque, errp)) {
        if (errp && !*errp) {
            error_setg(errp, "SGI OpenGL typed texture support is unavailable");
        }
        return false;
    }
    return true;
}

bool sgi_renderer_viewport_supported(SGIRenderer *renderer,
                                     const int32_t viewport[4], Error **errp)
{
    if (!renderer || !viewport || viewport[2] < 0 || viewport[3] < 0) {
        error_setg(errp, "Invalid SGI OpenGL viewport");
        return false;
    }
    if (!renderer->ops->supports_viewport ||
        !renderer->ops->supports_viewport(renderer->opaque, viewport, errp)) {
        if (errp && !*errp) {
            error_setg(errp, "SGI OpenGL viewport support is unavailable");
        }
        return false;
    }
    return true;
}

int sgi_renderer_line_probe_result(SGIRenderer *renderer)
{
    return renderer->line_probe;
}

void sgi_renderer_set_line_probe_result(SGIRenderer *renderer, int result)
{
    renderer->line_probe = result;
}

bool sgi_renderer_primitive_supported(SGIRenderer *renderer,
                                      uint32_t primitive, float size,
                                      Error **errp)
{
    if (!renderer || !isfinite(size) || size < 0) {
        error_setg(errp, "Invalid SGI OpenGL primitive size");
        return false;
    }
    if (primitive == SGI_RENDERER_TRIANGLES) {
        return true;
    }
    if (!renderer->ops->supports_primitive ||
        !renderer->ops->supports_primitive(renderer->opaque, primitive,
                                           size ? size : 1, errp)) {
        if (errp && !*errp) {
            error_setg(errp, "SGI OpenGL primitive support is unavailable");
        }
        return false;
    }
    return true;
}

bool sgi_renderer_clear(SGIRenderer *renderer, SGIRendererClear *clear,
                        Error **errp)
{
    if (!renderer || !clear ||
        (!clear->colour && !clear->depth && !clear->stencil) ||
        (clear->colour && !clear->pixels) ||
        ((clear->depth || clear->stencil) && !clear->depth_stencil) ||
        !clear->width ||
        !clear->height || clear->width > INT_MAX ||
        clear->height > INT_MAX ||
        clear->width > SIZE_MAX / clear->height / 4 /
                       sizeof(*clear->pixels) ||
        !clear->rect_count ||
        clear->rect_count > SGI_RENDERER_CLEAR_RECTS ||
        (clear->colour_mask & ~0xfu)) {
        error_setg(errp, "Invalid SGI OpenGL clear request");
        return false;
    }
    for (unsigned c = 0; c < 4; c++) {
        if (!isfinite(clear->rgba[c])) {
            error_setg(errp, "Invalid SGI OpenGL clear colour");
            return false;
        }
    }
    for (unsigned i = 0; i < clear->rect_count; i++) {
        SGIRendererClearRect *rect = &clear->rects[i];

        if (rect->x0 > rect->x1 || rect->y0 > rect->y1 ||
            rect->x1 >= clear->width || rect->y1 >= clear->height) {
            error_setg(errp, "Invalid SGI OpenGL clear rectangle");
            return false;
        }
    }
    if (renderer->failed) {
        error_setg(errp, "SGI OpenGL renderer has failed");
        return false;
    }
    if (!renderer->ops->clear) {
        error_setg(errp, "SGI OpenGL clearing is unavailable");
        return false;
    }
    if (!renderer->ops->clear(renderer->opaque, clear, errp)) {
        renderer->failed = true;
        return false;
    }
    return true;
}

SGIRenderDrawResult sgi_renderer_draw_batch_checked(
    SGIRenderer *renderer, SGIRendererRequest *requests, unsigned count,
    Error **errp)
{
    for (unsigned i = 0; i < count; i++) {
        if (!sgi_renderer_request_valid(&requests[i])) {
            error_setg(errp, "Unsupported SGI OpenGL renderer batch request");
            return SGI_RENDER_DRAW_FAILED;
        }
    }
    for (unsigned i = 0; i < count; i++) {
        SGIRenderDrawResult result =
            sgi_renderer_draw_checked(renderer, &requests[i], errp);

        if (result != SGI_RENDER_DRAW_OK) {
            return result;
        }
    }
    return SGI_RENDER_DRAW_OK;
}

bool sgi_renderer_draw_batch(SGIRenderer *renderer,
                             SGIRendererRequest *requests, unsigned count,
                             Error **errp)
{
    return sgi_renderer_draw_batch_checked(renderer, requests, count, errp) ==
           SGI_RENDER_DRAW_OK;
}

void sgi_renderer_test_fail_draw(SGIRenderer *renderer, unsigned nth_draw)
{
    renderer->test_draw_countdown = nth_draw;
}

void sgi_renderer_free(SGIRenderer *renderer)
{
    if (renderer) {
        renderer->ops->destroy(renderer->opaque);
        g_free(renderer);
    }
}
