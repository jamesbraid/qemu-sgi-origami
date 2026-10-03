/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UI_SGI_RENDERER_H
#define UI_SGI_RENDERER_H

#include "qapi/error.h"

#define SGI_RENDERER_SIZE 128
#define SGI_RENDERER_BYTES (SGI_RENDERER_SIZE * SGI_RENDERER_SIZE * 4)

typedef struct SGIRenderer SGIRenderer;

typedef struct SGIRendererRequest {
    uint64_t generation;
    float vertices[3][4];
    uint8_t rgb[3];
    int32_t clip[4]; /* Inclusive local ownership rectangle. */
    uint8_t rgba[SGI_RENDERER_BYTES];
} SGIRendererRequest;

typedef struct SGIRendererOps {
    void *(*create)(Error **errp);
    bool (*draw)(void *opaque, SGIRendererRequest *request, Error **errp);
    void (*destroy)(void *opaque);
} SGIRendererOps;

void sgi_renderer_register_gl(const SGIRendererOps *ops);
SGIRenderer *sgi_renderer_new(Error **errp);
bool sgi_renderer_draw(SGIRenderer *renderer, SGIRendererRequest *request,
                       Error **errp);
void sgi_renderer_free(SGIRenderer *renderer);
bool sgi_renderer_request_valid(const SGIRendererRequest *request);
bool sgi_renderer_staging_valid(const SGIRendererRequest *request);

#endif
