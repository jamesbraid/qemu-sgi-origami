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
};

static const SGIRendererOps *gl_ops;

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
    static const float vertices[3][4] = {
        { 16.25f, 16.25f, 0.0f, 1.0f },
        { 111.25f, 16.25f, 0.0f, 1.0f },
        { 16.25f, 111.25f, 0.0f, 1.0f },
    };

    if (memcmp(request->vertices, vertices, sizeof(vertices))) {
        return false;
    }
    if (request->clip[0] < 0 || request->clip[1] < 0 ||
        request->clip[2] >= SGI_RENDERER_SIZE ||
        request->clip[3] >= SGI_RENDERER_SIZE ||
        request->clip[0] > request->clip[2] ||
        request->clip[1] > request->clip[3]) {
        return false;
    }
    for (unsigned c = 0; c < 3; c++) {
        if (request->rgb[c] != 0 && request->rgb[c] != 255) {
            return false;
        }
    }
    return true;
}

bool sgi_renderer_staging_valid(const SGIRendererRequest *request)
{
    if (!sgi_renderer_request_valid(request)) {
        return false;
    }
    for (unsigned y = 0; y < SGI_RENDERER_SIZE; y++) {
        for (unsigned x = 0; x < SGI_RENDERER_SIZE; x++) {
            bool covered = x >= 16 && y >= 16 && x + y <= 126 &&
                           x >= request->clip[0] && x <= request->clip[2] &&
                           y >= request->clip[1] && y <= request->clip[3];
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
    if (renderer->test_draw_countdown && !--renderer->test_draw_countdown) {
        renderer->failed = true;
        error_setg(errp, "Injected SGI renderer test draw failure");
        return false;
    }
    return true;
}

bool sgi_renderer_draw_frame(SGIRenderer *renderer, SGIRendererFrame *frame,
                             Error **errp)
{
    if (!renderer || !frame || !frame->vertices || !frame->rgba ||
        !frame->written ||
        !frame->width || !frame->height || frame->width > INT_MAX ||
        frame->height > INT_MAX || frame->count != 3 ||
        frame->width > SIZE_MAX / frame->height / 4 /
                       sizeof(*frame->rgba) ||
        frame->written_capacity < (size_t)frame->width * frame->height ||
        frame->clip_count > SGI_RENDERER_CLEAR_RECTS) {
        error_setg(errp, "Invalid SGI OpenGL frame request");
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
    return true;
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
