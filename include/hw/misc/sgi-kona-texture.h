/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_KONA_TEXTURE_H
#define HW_MISC_SGI_KONA_TEXTURE_H

#include "hw/display/sgi/rasterizer.h"
#include "migration/vmstate.h"

/* The largest single image and staged load: a model bound. */
#define SGI_KONA_TEXTURE_BYTES (64u * 1024u * 1024u)

/*
 * The texture memory a pipe with no configured product reports to the
 * library: 64 MB, as on each RM7 board of the InfiniteReality2 capture. A
 * configured pipe reports its raster managers' size instead. The library
 * places resident textures in that pool (class 3), so it keeps them
 * within it.
 */
#define SGI_KONA_TEXTURE_MEMORY (64u * 1024u * 1024u)

/*
 * The host memory the model may spend on all resident textures, for a
 * pipe that reports `memory` bytes of texture memory. It keeps every
 * texel as 32 bits plus a coverage bit, whatever the native texel size,
 * and each texture's own record. A texture memory full of 4-bit texels,
 * the narrowest the library's formats name (inferred; the native sizes
 * are not read), takes eight times its size in texels and a quarter of
 * it in coverage; the ninth share is for the records, about 4,000
 * resident textures per 64 MB. So the worst-case host cost is about
 * 576 MiB for a 64 MB pipe and 2.25 GiB for a 256 MB one. A budget of
 * only the reported size refused textures pfCity keeps resident.
 */
#define SGI_KONA_TEXTURE_BUDGET(memory) (9ull * (memory))

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
    uint32_t shadow_compare;
    double shadow_ambient;
} SGIKonaTextureParams;

typedef struct SGIKonaTextureLevel {
    uint32_t level, width, height, depth, base_format, internal_format;
    uint32_t *pixels;
    /* Borrowed views may use logical dimensions over a larger backing. */
    uint32_t stride_width, stride_height;
    /* Where a view's texel (0, 0) sits in that backing; not migrated. */
    uint32_t origin_x, origin_y;
    const struct SGIKonaTextureNativeImage *native_border;
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
    uint32_t owner;             /* the context whose handle this is */
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
    bool native_one_dimensional;
    uint32_t native_count, native_format;
    bool native_format_valid;
    bool native_internal_valid;
    uint32_t native_base_format, native_internal_format;
    bool native_layout_valid;
    uint32_t native_layout;
    bool native_table_valid;
    uint32_t native_table[SGI_KONA_TEXTURE_TABLE_LEVELS];
    /* The bank of an ordinary texture's first table level. */
    bool native_bank_one;
    /* A volume bound in layout mode one, the single-level layout. */
    bool native_volume_single;
    /*
     * Coverage bits tested while resolving the object's levels, so their
     * cost can be seen; not migrated.
     */
    uint64_t coverage_tests;
    SGIKonaTextureNativeImage native[SGI_GL_TEX_LEVELS_MAX];
} SGIKonaTextureObject;

/*
 * Texture handles are a context's own: the client library numbers its
 * textures from 0 in each context, and places them in that context's own
 * texture-memory region. Objects are therefore kept per owner, the
 * context sgi_kona_texture_set_owner() last named (0 for none, k + 1 for
 * context table entry k), and each owner keeps its own binding.
 */
#define SGI_KONA_TEXTURE_OWNERS 66

typedef struct SGIKonaTextureState {
    /* SGI_KONA_TEXTURE_BUDGET of the pipe's texture memory; not migrated. */
    uint64_t budget;
    bool bound;
    uint32_t bound_handle;
    uint32_t owner;
    bool owner_bound[SGI_KONA_TEXTURE_OWNERS];
    uint32_t owner_bound_handle[SGI_KONA_TEXTURE_OWNERS];
    uint64_t bytes;
    GHashTable *objects;
    GHashTable *filter_curves;
    bool detail_bound;
    uint32_t detail_args[6];
} SGIKonaTextureState;

void sgi_kona_texture_init(SGIKonaTextureState *s);
/* Size the host budget for a pipe reporting `memory` bytes of texture. */
void sgi_kona_texture_set_memory(SGIKonaTextureState *s, uint64_t memory);
void sgi_kona_texture_reset(SGIKonaTextureState *s);
void sgi_kona_texture_cleanup(SGIKonaTextureState *s);
/* Make `owner` the context whose handles and binding the calls below use. */
void sgi_kona_texture_set_owner(SGIKonaTextureState *s, uint32_t owner);
/* Drop every object `owner` holds, as when its context entry is reused. */
void sgi_kona_texture_drop_owner(SGIKonaTextureState *s, uint32_t owner);
/* `owner`'s object for `handle`, or NULL. */
SGIKonaTextureObject *sgi_kona_texture_lookup(SGIKonaTextureState *s,
                                              uint32_t owner, uint32_t handle);
void sgi_kona_texture_defaults(SGIKonaTextureParams *p);
/*
 * A complete normalized image replaces one level transactionally.
 * DEPTH_COMPONENT pixels are unsigned normalized 32-bit values, not RGBA.
 */
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
/* An ordinary texture's levels alternate banks from this one. */
bool sgi_kona_texture_native_bank(SGIKonaTextureState *s, uint32_t handle,
                                  bool bank_one);
/*
 * The one object whose stored level table equals table, if exactly one,
 * with its first level in the same bank unless volume is set.
 */
bool sgi_kona_texture_find_table(SGIKonaTextureState *s,
                                 const uint32_t table[16], bool volume,
                                 bool bank_one, uint32_t *handle);
/*
 * The native images whose destination a texel readback names: a target,
 * a storage code (header word 9), the geometry word (16), the bank (bit
 * 3 of word 17) and the two bank addresses (words 18 and 19), at least
 * width x height. Returns how many objects hold one; when exactly one
 * does, *handle and *image name it.
 */
unsigned sgi_kona_texture_find_native(SGIKonaTextureState *s, uint32_t target,
                                      uint32_t storage, uint32_t geometry,
                                      bool bank_one, uint32_t bank_a,
                                      uint32_t bank_b, uint32_t width,
                                      uint32_t height, uint32_t *handle,
                                      const SGIKonaTextureLevel **image);
/* Whether handle's stored level table is table, as find_table matches. */
bool sgi_kona_texture_table_matches(SGIKonaTextureState *s, uint32_t handle,
                                    const uint32_t table[16], bool volume,
                                    bool bank_one);
bool sgi_kona_texture_native_dimensions(SGIKonaTextureState *s, uint32_t handle,
                                       uint32_t width, uint32_t height);
bool sgi_kona_texture_native_dimensions_1d(SGIKonaTextureState *s,
                                           uint32_t handle, uint32_t width);
/* Optional emitted physical size for base-zero ordinary and volume views. */
bool sgi_kona_texture_native_layout(SGIKonaTextureState *s, uint32_t handle,
                                   uint32_t physical_size);
bool sgi_kona_texture_native_volume_dimensions(SGIKonaTextureState *s,
                                              uint32_t handle, uint32_t width,
                                              uint32_t height, uint32_t depth);
/* Whether a volume's bind names layout mode one rather than mode two. */
bool sgi_kona_texture_native_volume_single(SGIKonaTextureState *s,
                                           uint32_t handle, bool single);
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
/*
 * Filter tables have device slot identity independent of image handles.
 * The pipe is not told a table's type, so any table is stored; only the
 * first 32 words are kept, which loses a sampled table's last entry.
 */
bool sgi_kona_texture_curve_upload(SGIKonaTextureState *s, uint32_t slot,
                                  const uint32_t words[33]);
/*
 * The slot's 64 spline coefficients, or NULL when the slot is empty or
 * its table is not a spline. Borrowed until the slot is updated or the
 * texture state is reset.
 */
const double *sgi_kona_texture_curve(SGIKonaTextureState *s, uint32_t slot);
void sgi_kona_texture_delete(SGIKonaTextureState *s, uint32_t handle);
/* Borrowed for one synchronous draw; updates/deletion must not overlap it. */
const SGIGLTexImage *sgi_kona_texture_resolve(SGIKonaTextureState *s);
extern const VMStateInfo vmstate_info_sgi_kona_texture;

#define VMSTATE_SGI_KONA_TEXTURE(_field, _state) \
    VMSTATE_SINGLE(_field, _state, 1, vmstate_info_sgi_kona_texture, \
                   SGIKonaTextureState)

#endif
