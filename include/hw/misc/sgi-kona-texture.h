/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_KONA_TEXTURE_H
#define HW_MISC_SGI_KONA_TEXTURE_H

#include "hw/display/sgi/rasterizer.h"
#include "migration/vmstate.h"

/* Model resource bounds, not physical texture-memory capacities. */
#define SGI_KONA_TEXTURE_BYTES (64u * 1024u * 1024u)

typedef struct SGIKonaTextureParams {
    uint32_t min_filter, mag_filter, wrap_s, wrap_t;
    float border_color[4];
    uint32_t base_level, max_level;
    double min_lod, max_lod;
    double post_scale[4], post_bias[4];
} SGIKonaTextureParams;

typedef struct SGIKonaTextureLevel {
    uint32_t level, width, height, base_format, internal_format;
    uint32_t *pixels;
} SGIKonaTextureLevel;

typedef struct SGIKonaTextureObject {
    uint32_t handle, count;
    SGIKonaTextureParams params;
    SGIKonaTextureLevel levels[SGI_GL_TEX_LEVELS_MAX];
    SGIGLTexLevel view[SGI_GL_TEX_LEVELS_MAX];
    SGIGLTexPyramid pyramid;
    SGIGLTexImage image;
} SGIKonaTextureObject;

typedef struct SGIKonaTextureState {
    bool bound;
    uint32_t bound_handle;
    uint64_t bytes;
    GHashTable *objects;
} SGIKonaTextureState;

void sgi_kona_texture_init(SGIKonaTextureState *s);
void sgi_kona_texture_reset(SGIKonaTextureState *s);
void sgi_kona_texture_cleanup(SGIKonaTextureState *s);
void sgi_kona_texture_defaults(SGIKonaTextureParams *p);
/* A complete normalized image replaces one level transactionally. */
bool sgi_kona_texture_upload(SGIKonaTextureState *s, uint32_t handle,
                            uint32_t level, uint32_t width, uint32_t height,
                            uint32_t base_format, uint32_t internal_format,
                            const uint32_t *rgba);
/* NULL params retain the object's sampler, including for an incomplete view. */
bool sgi_kona_texture_bind(SGIKonaTextureState *s, uint32_t handle,
                          const SGIKonaTextureParams *params);
void sgi_kona_texture_delete(SGIKonaTextureState *s, uint32_t handle);
/* Borrowed for one synchronous draw; updates/deletion must not overlap it. */
const SGIGLTexImage *sgi_kona_texture_resolve(SGIKonaTextureState *s);
extern const VMStateInfo vmstate_info_sgi_kona_texture;

#define VMSTATE_SGI_KONA_TEXTURE(_field, _state) \
    VMSTATE_SINGLE(_field, _state, 1, vmstate_info_sgi_kona_texture, \
                   SGIKonaTextureState)

#endif
