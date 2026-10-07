/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_KONA_MESA_PROVIDER_H
#define HW_MISC_SGI_KONA_MESA_PROVIDER_H

#include "hw/display/sgi/board-memory.h"
#include "hw/display/sgi/rasterizer.h"

typedef struct SGIKonaMesaProvider SGIKonaMesaProvider;

/* The six current board plane IDs, in their model-defined order. */
typedef struct SGIKonaMesaPlanes {
    uint32_t front, back, front_tag, back_tag, depth, stencil;
    uint8_t direct_tag; /* Pass SGI_KONA_TAG_DIRECT from the device. */
} SGIKonaMesaPlanes;

typedef struct SGIKonaMesaDirtyOps {
    void (*plane)(void *opaque, uint32_t plane_id,
                  uint32_t y0, uint32_t y1);
    void (*texture)(void *opaque, uint32_t handle, uint32_t level,
                    uint32_t image_id);
} SGIKonaMesaDirtyOps;

/*
 * A selected texture image. The owner resolves a logical level to its native
 * fragment or normalized image before binding; this module does not infer the
 * fragment table from a GL level. Pixels are the model's derived, packed
 * RGBA8 words. Original upload component widths are not retained there.
 * The pointers remain valid through the synchronous submission only.
 */
typedef struct SGIKonaMesaTextureView {
    uint32_t handle, level, image_id;
    uint32_t width, height, depth;
    uint8_t border[3];
    bool native_fragment;
    bool bottom_origin;
    uint32_t *pixels;
    uint8_t *coverage;
    uint64_t *covered;
} SGIKonaMesaTextureView;

SGIKonaMesaProvider *sgi_kona_mesa_provider_new(
    SGIBoardMemory *memory, const SGIKonaMesaPlanes *planes,
    const SGIKonaMesaDirtyOps *dirty, void *dirty_opaque, Error **errp);
void sgi_kona_mesa_provider_free(SGIKonaMesaProvider *provider);

/*
 * Attach these to a typed-only SGIRasterTarget whose opaque is provider.
 * Covered RGB commits write each selected bank's direct tag only where the
 * Mesa write set says a fragment survived.
 */
const SGIRenderResource *sgi_kona_mesa_resources(
    const SGIKonaMesaProvider *provider, size_t *count);
const SGIRenderResourceOps *sgi_kona_mesa_resource_ops(void);

/*
 * Call under BQL before each checked submission. Advance the six board
 * backings and their views for a fresh snapshot; native bytes stay unchanged.
 */
void sgi_kona_mesa_refresh_board_views(SGIKonaMesaProvider *provider);

bool sgi_kona_mesa_bind_texture(SGIKonaMesaProvider *provider,
                                const SGIKonaMesaTextureView *view,
                                Error **errp);
void sgi_kona_mesa_unbind_texture(SGIKonaMesaProvider *provider);

/* Call after native writes, texture uploads, subimages, and deletion. */
void sgi_kona_mesa_touch_plane(SGIKonaMesaProvider *provider,
                               uint32_t plane_id, uint32_t y0, uint32_t y1);
void sgi_kona_mesa_touch_texture(SGIKonaMesaProvider *provider,
                                 uint32_t handle, int level);

#endif
