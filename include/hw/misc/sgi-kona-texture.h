/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_KONA_TEXTURE_H
#define HW_MISC_SGI_KONA_TEXTURE_H

#include "hw/display/sgi/rasterizer.h"
#include "migration/vmstate.h"

/* Model resource bounds, not physical texture-memory capacities. */
#define SGI_KONA_TEXTURE_BYTES (64u * 1024u * 1024u)

typedef struct SGIKonaTextureParams {
    uint32_t min_filter, mag_filter, wrap_s, wrap_t, wrap_r;
    float border_color[4];
    uint32_t base_level, max_level;
    double min_lod, max_lod;
    double post_scale[4], post_bias[4];
    double lod_scale_s, lod_scale_t, lod_scale_r;
    uint32_t filter_curve_slot;
    int32_t detail_level;
    uint32_t detail_mode;
} SGIKonaTextureParams;

typedef struct SGIKonaTextureLevel {
    uint32_t level, width, height, depth, base_format, internal_format;
    uint32_t *pixels;
} SGIKonaTextureLevel;

#define SGI_KONA_TEXTURE_TABLE_LEVELS 16u
#define SGI_KONA_TEXTURE_CURVE_SLOTS 4096u

typedef struct SGIKonaTextureNativeImage {
    uint32_t header[20];
    SGIKonaTextureLevel image;
    uint8_t *coverage;
    uint64_t covered;
} SGIKonaTextureNativeImage;

typedef struct SGIKonaTextureObject {
    uint32_t handle, count;
    SGIKonaTextureParams params;
    SGIKonaTextureLevel levels[SGI_GL_TEX_LEVELS_MAX];
    SGIGLTexLevel view[SGI_GL_TEX_LEVELS_MAX];
    SGIKonaTextureLevel native_view[SGI_GL_TEX_LEVELS_MAX];
    SGIKonaTextureLevel detail_pixels;
    SGIGLTexLevel detail_view;
    SGIGLTexPyramid pyramid;
    SGIGLTexImage image;
    const double *filter_curve;
    uint32_t native_width, native_height, native_depth;
    uint32_t native_count, native_format;
    bool native_format_valid;
    bool native_internal_valid;
    uint32_t native_base_format, native_internal_format;
    bool native_table_valid;
    uint32_t native_table[SGI_KONA_TEXTURE_TABLE_LEVELS];
    SGIKonaTextureNativeImage native[SGI_GL_TEX_LEVELS_MAX];
} SGIKonaTextureObject;

typedef struct SGIKonaTextureState {
    bool bound;
    uint32_t bound_handle;
    uint64_t bytes;
    GHashTable *objects;
    GHashTable *filter_curves;
    bool detail_bound;
    uint32_t detail_args[6];
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
/* Replace a validated rectangle in an existing normalized level. */
bool sgi_kona_texture_subimage(SGIKonaTextureState *s, uint32_t handle,
                              uint32_t level, uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height,
                              const uint32_t *rgba);
/* Physical descriptors remain independent of the later logical level table. */
bool sgi_kona_texture_native_upload(SGIKonaTextureState *s,
                                   const uint32_t header[20],
                                   const uint32_t *rgba,
                                   uint32_t base_format,
                                   uint32_t internal_format);
bool sgi_kona_texture_native_table(SGIKonaTextureState *s, uint32_t handle,
                                  const uint32_t table[16]);
bool sgi_kona_texture_native_dimensions(SGIKonaTextureState *s, uint32_t handle,
                                       uint32_t width, uint32_t height);
bool sgi_kona_texture_native_volume_dimensions(SGIKonaTextureState *s,
                                              uint32_t handle, uint32_t width,
                                              uint32_t height, uint32_t depth);
bool sgi_kona_texture_native_format(SGIKonaTextureState *s, uint32_t handle,
                                   uint32_t storage_format);
/* Logical destination format does not alter the retained upload components. */
bool sgi_kona_texture_native_internal_format(SGIKonaTextureState *s,
                                             uint32_t handle, uint32_t base,
                                             uint32_t internal);
/* Selector binding is independent of the primary image handle. */
bool sgi_kona_texture_detail_bind(SGIKonaTextureState *s,
                                  const uint32_t args[6]);
/* NULL params retain the object's sampler, including for an incomplete view. */
bool sgi_kona_texture_bind(SGIKonaTextureState *s, uint32_t handle,
                          const SGIKonaTextureParams *params);
/* Filter tables have device slot identity independent of image handles. */
bool sgi_kona_texture_curve_upload(SGIKonaTextureState *s, uint32_t slot,
                                  const uint32_t words[33]);
/* Borrowed until the slot is updated or the texture state is reset. */
const double *sgi_kona_texture_curve(SGIKonaTextureState *s, uint32_t slot);
void sgi_kona_texture_delete(SGIKonaTextureState *s, uint32_t handle);
/* Borrowed for one synchronous draw; updates/deletion must not overlap it. */
const SGIGLTexImage *sgi_kona_texture_resolve(SGIKonaTextureState *s);
extern const VMStateInfo vmstate_info_sgi_kona_texture;

#define VMSTATE_SGI_KONA_TEXTURE(_field, _state) \
    VMSTATE_SINGLE(_field, _state, 1, vmstate_info_sgi_kona_texture, \
                   SGIKonaTextureState)

#endif
