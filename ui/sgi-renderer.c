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
                      y > -reach && y < 3 * reach)) {
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
    for (unsigned i = 0; i < 3; i++) {
        const float *vertex = request->vertices[i];

        if (vertex[2] != 0 || vertex[3] != 1) {
            return false;
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

static int64_t renderer_edge(const int64_t a[2], const int64_t b[2],
                             int64_t x, int64_t y)
{
    int64_t dx = b[0] - a[0], dy = b[1] - a[1];

    return dx * (y - a[1]) - dy * (x - a[0]);
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

bool sgi_renderer_staging_valid(const SGIRendererRequest *request)
{
    int64_t vertices[3][2], area;

    if (!sgi_renderer_request_valid(request)) {
        return false;
    }
    for (unsigned i = 0; i < 3; i++) {
        for (unsigned axis = 0; axis < 2; axis++) {
            vertices[i][axis] = request->vertices[i][axis] * 256;
        }
    }
    area = renderer_edge(vertices[0], vertices[1],
                         vertices[2][0], vertices[2][1]);
    if (area < 0) {
        for (unsigned axis = 0; axis < 2; axis++) {
            int64_t tmp = vertices[1][axis];

            vertices[1][axis] = vertices[2][axis];
            vertices[2][axis] = tmp;
        }
    }
    for (unsigned y = 0; y < SGI_RENDERER_SIZE; y++) {
        for (unsigned x = 0; x < SGI_RENDERER_SIZE; x++) {
            bool covered = (int32_t)x >= request->clip[0] &&
                           (int32_t)x <= request->clip[2] &&
                           (int32_t)y >= request->clip[1] &&
                           (int32_t)y <= request->clip[3] &&
                           renderer_covered(vertices, area, x, y);
            const uint8_t *pixel = request->rgba +
                                  (y * SGI_RENDERER_SIZE + x) * 4;

            if (pixel[3] != (covered ? 255 : 0)) {
                return false;
            }
            for (unsigned c = 0; c < 3; c++) {
                if (pixel[c] != (covered ? request->rgb[c] : 0)) {
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

bool sgi_renderer_draw(SGIRenderer *renderer, SGIRendererRequest *request,
                       Error **errp)
{
    uint64_t generation = request->generation;
    uint8_t rgb[3];
    float vertices[3][4];
    int32_t clip[4];

    memcpy(rgb, request->rgb, sizeof(rgb));
    memcpy(clip, request->clip, sizeof(clip));
    memcpy(vertices, request->vertices, sizeof(vertices));
    if (renderer->failed) {
        error_setg(errp, "SGI OpenGL renderer has failed");
        return false;
    }
    if (!sgi_renderer_request_valid(request)) {
        error_setg(errp, "Unsupported SGI OpenGL renderer request");
        return false;
    }
    if (!renderer->ops->draw(renderer->opaque, request, errp)) {
        renderer->failed = true;
        return false;
    }
    if (request->generation != generation ||
        memcmp(request->rgb, rgb, sizeof(rgb)) ||
        memcmp(request->clip, clip, sizeof(clip)) ||
        memcmp(request->vertices, vertices, sizeof(vertices)) ||
        !sgi_renderer_staging_valid(request)) {
        renderer->failed = true;
        error_setg(errp, "Invalid SGI OpenGL renderer staging");
        return false;
    }
    return sgi_renderer_test_draw_complete(renderer, errp);
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

bool sgi_renderer_draw_batch(SGIRenderer *renderer,
                             SGIRendererRequest *requests, unsigned count,
                             Error **errp)
{
    for (unsigned i = 0; i < count; i++) {
        if (!sgi_renderer_request_valid(&requests[i])) {
            error_setg(errp, "Unsupported SGI OpenGL renderer batch request");
            return false;
        }
    }
    for (unsigned i = 0; i < count; i++) {
        if (!sgi_renderer_draw(renderer, &requests[i], errp)) {
            return false;
        }
    }
    return true;
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
