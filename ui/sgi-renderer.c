/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "ui/sgi-renderer.h"

struct SGIRenderer {
    const SGIRendererOps *ops;
    void *opaque;
    bool failed;
};

static const SGIRendererOps *gl_ops;

void sgi_renderer_register_gl(const SGIRendererOps *ops)
{
    assert(ops && ops->create && ops->draw && ops->destroy);
    assert(!gl_ops || gl_ops == ops);
    gl_ops = ops;
}

SGIRenderer *sgi_renderer_new(Error **errp)
{
    Error *local_err = NULL;
    SGIRenderer *renderer;

    if (!gl_ops) {
        ui_module_load("opengl", &local_err);
    }
    if (!gl_ops) {
        if (local_err) {
            error_propagate(errp, local_err);
        } else {
            error_setg(errp, "SGI OpenGL renderer is unavailable");
        }
        return NULL;
    }
    error_free(local_err);
    renderer = g_try_new0(SGIRenderer, 1);
    if (!renderer) {
        error_setg(errp, "Cannot allocate SGI OpenGL renderer");
        return NULL;
    }
    renderer->ops = gl_ops;
    renderer->opaque = gl_ops->create(errp);
    if (!renderer->opaque) {
        g_free(renderer);
        return NULL;
    }
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
    return true;
}

void sgi_renderer_free(SGIRenderer *renderer)
{
    if (renderer) {
        renderer->ops->destroy(renderer->opaque);
        g_free(renderer);
    }
}
