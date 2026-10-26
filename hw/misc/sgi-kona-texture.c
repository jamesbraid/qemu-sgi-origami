/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include <math.h>
#include "hw/misc/sgi-kona-texture.h"
#include "hw/misc/sgi-kona-gl-decode.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "migration/qemu-file.h"

/* Reserve table bookkeeping as well as the fixed level descriptors. */
#define TEXTURE_OBJECT_BYTES (sizeof(SGIKonaTextureObject) + 64)

typedef struct SGIKonaTextureCurve {
    uint32_t slot;
    uint32_t words[32];
    /* Whether the words decode as 64 spline coefficients. */
    bool spline;
    double values[64];
} SGIKonaTextureCurve;

#define TEXTURE_CURVE_BYTES (sizeof(SGIKonaTextureCurve) + 64)

void sgi_kona_texture_defaults(SGIKonaTextureParams *p)
{
    *p = (SGIKonaTextureParams) {
        .min_filter = SGI_GL_NEAREST_MIPMAP_LINEAR,
        .mag_filter = SGI_GL_LINEAR,
        .wrap_s = SGI_GL_REPEAT, .wrap_t = SGI_GL_REPEAT,
        .wrap_r = SGI_GL_REPEAT,
        .max_level = 1000, .min_lod = -1000, .max_lod = 1000,
        .post_scale = { 1, 1, 1, 1 },
        .lod_scale_s = 1, .lod_scale_t = 1, .lod_scale_r = 1,
        .detail_mode = SGI_GL_MODULATE,
    };
}

void sgi_kona_texture_init(SGIKonaTextureState *s)
{
    memset(s, 0, sizeof(*s));
    s->budget = SGI_KONA_TEXTURE_BUDGET(SGI_KONA_TEXTURE_MEMORY);
    s->objects = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->filter_curves = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                             NULL, g_free);
}

void sgi_kona_texture_set_memory(SGIKonaTextureState *s, uint64_t memory)
{
    s->budget = SGI_KONA_TEXTURE_BUDGET(memory);
}

static uint64_t level_bytes(const SGIKonaTextureLevel *l)
{
    return (uint64_t)l->width * l->height * sizeof(*l->pixels);
}

static uint64_t native_pixels(const SGIKonaTextureNativeImage *n)
{
    return (uint64_t)n->image.width * n->image.height * n->image.depth;
}

static uint64_t native_coverage_bytes(const SGIKonaTextureNativeImage *n)
{
    return (native_pixels(n) + 7) / 8;
}

static uint64_t native_bytes(const SGIKonaTextureNativeImage *n)
{
    return native_pixels(n) * sizeof(uint32_t) + native_coverage_bytes(n);
}

static bool native_covered(const uint8_t *coverage, uint64_t pixel)
{
    return coverage[pixel / 8] & (1u << (pixel % 8));
}

static void native_cover(uint8_t *coverage, uint64_t pixel)
{
    coverage[pixel / 8] |= 1u << (pixel % 8);
}

/* Physical rectangles may be incomplete with non-power-of-two extents. */
static bool native_dimensions(uint32_t width, uint32_t height)
{
    return width && height && width <= INT32_MAX && height <= INT32_MAX &&
           (uint64_t)width * height <= SGI_KONA_TEXTURE_BYTES / 4;
}

static bool native_volume_dimensions(uint32_t w, uint32_t h, uint32_t d)
{
    return native_dimensions(w, h) && d && d <= INT32_MAX &&
           (uint64_t)w * h * d <= SGI_KONA_TEXTURE_BYTES / 4;
}

static void texture_object_free(SGIKonaTextureObject *o)
{
    for (unsigned k = 0; k < o->count; k++) {
        g_free(o->levels[k].pixels);
    }
    for (unsigned k = 0; k < o->native_count; k++) {
        g_free(o->native[k].image.pixels);
        g_free(o->native[k].coverage);
    }
    g_free(o);
}

void sgi_kona_texture_reset(SGIKonaTextureState *s)
{
    GHashTableIter iter;
    gpointer value;

    g_hash_table_iter_init(&iter, s->objects);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        texture_object_free(value);
    }
    g_hash_table_remove_all(s->objects);
    g_hash_table_remove_all(s->filter_curves);
    s->bound = false;
    s->bound_handle = 0;
    s->owner = 0;
    memset(s->owner_bound, 0, sizeof(s->owner_bound));
    memset(s->owner_bound_handle, 0, sizeof(s->owner_bound_handle));
    s->detail_bound = false;
    memset(s->detail_args, 0, sizeof(s->detail_args));
    s->bytes = 0;
}

void sgi_kona_texture_cleanup(SGIKonaTextureState *s)
{
    sgi_kona_texture_reset(s);
    g_clear_pointer(&s->objects, g_hash_table_destroy);
    g_clear_pointer(&s->filter_curves, g_hash_table_destroy);
}

QEMU_BUILD_BUG_ON(sizeof(gsize) < 8);

static gpointer texture_key(uint32_t owner, uint32_t handle)
{
    return GSIZE_TO_POINTER((gsize)owner << 32 | handle);
}

void sgi_kona_texture_set_owner(SGIKonaTextureState *s, uint32_t owner)
{
    assert(owner < SGI_KONA_TEXTURE_OWNERS);
    if (owner == s->owner) {
        return;
    }
    s->owner_bound[s->owner] = s->bound;
    s->owner_bound_handle[s->owner] = s->bound_handle;
    s->owner = owner;
    s->bound = s->owner_bound[owner];
    s->bound_handle = s->owner_bound_handle[owner];
}

SGIKonaTextureObject *sgi_kona_texture_lookup(SGIKonaTextureState *s,
                                              uint32_t owner, uint32_t handle)
{
    return g_hash_table_lookup(s->objects, texture_key(owner, handle));
}

static SGIKonaTextureObject *texture_object(SGIKonaTextureState *s,
                                            uint32_t handle, bool create)
{
    SGIKonaTextureObject *o;

    if (handle == UINT32_MAX) {
        return NULL;
    }
    o = g_hash_table_lookup(s->objects, texture_key(s->owner, handle));
    if (!o && create &&
        TEXTURE_OBJECT_BYTES <= s->budget - s->bytes) {
        o = g_try_new0(SGIKonaTextureObject, 1);
        if (o) {
            o->handle = handle;
            o->owner = s->owner;
            sgi_kona_texture_defaults(&o->params);
            g_hash_table_insert(s->objects, texture_key(s->owner, handle), o);
            s->bytes += TEXTURE_OBJECT_BYTES;
        }
    }
    return o;
}

static bool texture_format(uint32_t base, uint32_t internal)
{
    switch (base) {
    case SGI_GL_DEPTH_COMPONENT:
        return internal == base ||
               (internal >= SGI_GL_DEPTH_COMPONENT16_SGIX &&
                internal <= SGI_GL_DEPTH_COMPONENT32_SGIX);
    case SGI_GL_ALPHA:
        return internal == base || (internal >= 0x803b && internal <= 0x803e);
    case SGI_GL_LUMINANCE:
        return internal == base || (internal >= 0x803f && internal <= 0x8042);
    case SGI_GL_LUMINANCE_ALPHA:
        return internal == base || (internal >= 0x8043 && internal <= 0x8048);
    case SGI_GL_INTENSITY:
        return internal == base || (internal >= 0x804a && internal <= 0x804d);
    case SGI_GL_RGB:
        return internal == base || internal == 0x2a10 ||
               (internal >= 0x804f && internal <= 0x8054);
    case SGI_GL_RGBA:
        return internal == base || (internal >= 0x8055 && internal <= 0x805b);
    default:
        return false;
    }
}

static bool texture_dimensions(uint32_t w, uint32_t h)
{
    return w && h && w <= INT32_MAX && h <= INT32_MAX &&
           is_power_of_2(w) && is_power_of_2(h) &&
           (uint64_t)w * h <= SGI_KONA_TEXTURE_BYTES / sizeof(uint32_t);
}

bool sgi_kona_texture_upload(SGIKonaTextureState *s, uint32_t handle,
                            uint32_t level, uint32_t width, uint32_t height,
                            uint32_t base_format, uint32_t internal_format,
                            const uint32_t *rgba)
{
    SGIKonaTextureObject *o = texture_object(s, handle, false);
    uint32_t count = o ? o->count : 0, at = 0;
    uint64_t old = 0, bytes = (uint64_t)width * height * sizeof(*rgba);
    uint32_t *pixels;
    bool replace;

    if (handle == UINT32_MAX || level >= SGI_GL_TEX_LEVELS_MAX || !rgba ||
        !texture_dimensions(width, height) ||
        !texture_format(base_format, internal_format)) {
        return false;
    }
    while (at < count && o->levels[at].level < level) {
        at++;
    }
    replace = at < count && o->levels[at].level == level;
    if (replace) {
        old = level_bytes(&o->levels[at]);
    }
    if ((!replace && count == SGI_GL_TEX_LEVELS_MAX) ||
        bytes + (o ? 0 : TEXTURE_OBJECT_BYTES) >
        s->budget - (s->bytes - old)) {
        return false;
    }
    pixels = g_try_malloc(bytes);
    if (!pixels) {
        return false;
    }
    memcpy(pixels, rgba, bytes);
    o = texture_object(s, handle, true);
    if (!o) {
        g_free(pixels);
        return false;
    }
    if (replace) {
        g_free(o->levels[at].pixels);
    } else {
        memmove(&o->levels[at + 1], &o->levels[at],
                (count - at) * sizeof(o->levels[0]));
        o->count++;
    }
    o->levels[at] = (SGIKonaTextureLevel) {
        .level = level, .width = width, .height = height,
        .base_format = base_format, .internal_format = internal_format,
        .pixels = pixels,
    };
    s->bytes = s->bytes - old + bytes;
    return true;
}

bool sgi_kona_texture_subimage(SGIKonaTextureState *s, uint32_t handle,
                              uint32_t level, uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height,
                              const uint32_t *rgba)
{
    SGIKonaTextureObject *o = texture_object(s, handle, false);
    SGIKonaTextureLevel *l = NULL;
    uint32_t *pixels;
    size_t bytes;

    if (!o) {
        return false;
    }
    for (unsigned k = 0; k < o->count; k++) {
        if (o->levels[k].level == level) {
            l = &o->levels[k];
            break;
        }
    }
    if (!l || x > l->width || y > l->height ||
        width > l->width - x || height > l->height - y) {
        return false;
    }
    if (!width || !height) {
        return true;
    }
    if (!rgba) {
        return false;
    }
    bytes = (size_t)width * height * sizeof(*pixels);
    pixels = g_try_malloc(bytes);
    if (!pixels) {
        return false;
    }
    /* Stage aliased sources before changing any destination row. */
    memcpy(pixels, rgba, bytes);
    for (unsigned row = 0; row < height; row++) {
        memcpy(l->pixels + (size_t)(y + row) * l->width + x,
               pixels + (size_t)row * width, width * sizeof(*pixels));
    }
    g_free(pixels);
    return true;
}

/*
 * Header word 17's layout mode: zero for 1D and 2D, one or two for a
 * volume. Mode one is the library's single-level volume layout, mode two
 * its mipmapped one.
 */
static bool texture_native_mode_valid(const uint32_t h[20])
{
    uint32_t mode = h[17] & 0x60;

    return h[0] == SGI_GL_TEXTURE_3D ? mode == 0x20 || mode == 0x40 : !mode;
}

static bool texture_native_key(const uint32_t a[20], const uint32_t b[20])
{
    /* DMA byte layout is normalized before these image fragments arrive. */
    return a[0] == b[0] && a[9] == b[9] &&
           a[16] == b[16] && (a[17] & ~4u) == (b[17] & ~4u) &&
           a[18] == b[18] && a[19] == b[19];
}

unsigned sgi_kona_texture_find_native(SGIKonaTextureState *s, uint32_t target,
                                      uint32_t storage, uint32_t geometry,
                                      bool bank_one, uint32_t bank_a,
                                      uint32_t bank_b, uint32_t width,
                                      uint32_t height, uint32_t *handle,
                                      const SGIKonaTextureLevel **image)
{
    GHashTableIter iter;
    gpointer value;
    unsigned found = 0;

    g_hash_table_iter_init(&iter, s->objects);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        SGIKonaTextureObject *o = value;

        if (o->owner != s->owner) {
            continue;
        }
        for (unsigned k = 0; k < o->native_count; k++) {
            const SGIKonaTextureNativeImage *n = &o->native[k];

            if (n->header[0] == target && n->header[9] == storage &&
                n->header[16] == geometry &&
                !!(n->header[17] & 8) == bank_one &&
                n->header[18] == bank_a && n->header[19] == bank_b &&
                n->image.width >= width && n->image.height >= height) {
                found++;
                *handle = o->handle;
                *image = &n->image;
                break;
            }
        }
    }
    return found;
}

bool sgi_kona_texture_native_upload(SGIKonaTextureState *s,
                                   const uint32_t h[20], const uint32_t *rgba,
                                   uint32_t base_format,
                                   uint32_t internal_format)
{
    SGIKonaTextureObject *o = texture_object(s, h[10], false);
    SGIKonaTextureNativeImage *n = NULL;
    uint32_t width, height, depth, *pixels;
    bool volume = h[0] == SGI_GL_TEXTURE_3D;
    bool one_dimensional = h[0] == SGI_GL_TEXTURE_1D;
    uint8_t *coverage;
    uint64_t old = 0, bytes, count, covered = 0;

    if ((!volume && !one_dimensional && h[0] != SGI_GL_TEXTURE_2D) ||
        (one_dimensional && (h[2] != 1 || h[12] || h[14] || h[15])) ||
        !rgba || h[10] == UINT32_MAX || !texture_native_mode_valid(h) ||
        (!volume && h[18] && h[19]) ||
        !h[1] || !h[2] || h[13] < h[11] || h[14] < h[12] ||
        (uint64_t)h[13] - h[11] + 1 != h[1] ||
        (uint64_t)h[14] - h[12] + 1 != h[2] ||
        !texture_format(base_format, internal_format)) {
        return false;
    }
    if (o) {
        for (unsigned k = 0; k < o->native_count; k++) {
            if (texture_native_key(o->native[k].header, h)) {
                n = &o->native[k];
                break;
            }
        }
    }
    if (!n && o && o->native_count == SGI_GL_TEX_LEVELS_MAX) {
        return false;
    }
    if (h[13] == UINT32_MAX || h[14] == UINT32_MAX ||
        (volume && h[15] == UINT32_MAX)) {
        return false;
    }
    width = MAX(n ? n->image.width : 0, h[13] + 1);
    height = MAX(n ? n->image.height : 0, h[14] + 1);
    depth = volume ? MAX(n ? n->image.depth : 0, h[15] + 1) : 1;
    if (!native_volume_dimensions(width, height, depth) ||
        (o && o->native_width && !o->native_layout_valid &&
         (width > o->native_width || height > o->native_height ||
          (volume && o->native_depth && depth > o->native_depth))) ||
        (n && (n->image.base_format != base_format ||
               n->image.internal_format != internal_format))) {
        return false;
    }
    if (n) {
        old = native_bytes(n);
        covered = n->covered;
    }
    count = (uint64_t)width * height * depth;
    bytes = count * sizeof(*pixels) + (count + 7) / 8;
    if (bytes + (o ? 0 : TEXTURE_OBJECT_BYTES) >
        s->budget - (s->bytes - old)) {
        return false;
    }
    pixels = g_try_malloc0(count * sizeof(*pixels));
    coverage = g_try_malloc0((count + 7) / 8);
    if (!pixels || !coverage) {
        g_free(pixels);
        g_free(coverage);
        return false;
    }
    if (n) {
        for (uint32_t z = 0; z < n->image.depth; z++) {
            for (uint32_t y = 0; y < n->image.height; y++) {
                uint64_t old_row = ((uint64_t)z * n->image.height + y) *
                                   n->image.width;
                uint64_t row = ((uint64_t)z * height + y) * width;

                memcpy(pixels + row, n->image.pixels + old_row,
                       n->image.width * sizeof(*pixels));
                for (uint32_t x = 0; x < n->image.width; x++) {
                    if (native_covered(n->coverage, old_row + x)) {
                        native_cover(coverage, row + x);
                    }
                }
            }
        }
    }
    for (unsigned y = 0; y < h[2]; y++) {
        uint32_t z = volume ? h[15] : 0;
        uint64_t row = ((uint64_t)z * height + h[12] + y) * width + h[11];

        memcpy(pixels + row, rgba + (size_t)y * h[1],
               h[1] * sizeof(*pixels));
        for (uint32_t x = 0; x < h[1]; x++) {
            if (!native_covered(coverage, row + x)) {
                covered++;
                native_cover(coverage, row + x);
            }
        }
    }
    o = texture_object(s, h[10], true);
    if (!o) {
        g_free(pixels);
        g_free(coverage);
        return false;
    }
    if (!n) {
        n = &o->native[o->native_count++];
    } else {
        g_free(n->image.pixels);
        g_free(n->coverage);
    }
    n->coverage = coverage;
    n->covered = covered;
    memcpy(n->header, h, sizeof(n->header));
    n->image = (SGIKonaTextureLevel) {
        .width = width, .height = height, .depth = depth,
        .base_format = base_format,
        .internal_format = internal_format, .pixels = pixels,
    };
    s->bytes = s->bytes - old + bytes;
    return true;
}

bool sgi_kona_texture_native_table(SGIKonaTextureState *s, uint32_t handle,
                                  const uint32_t table[16])
{
    SGIKonaTextureObject *o;

    if (!table) {
        return false;
    }
    o = texture_object(s, handle, true);
    if (!o) {
        return false;
    }
    memcpy(o->native_table, table, sizeof(o->native_table));
    o->native_table_valid = true;
    return true;
}

bool sgi_kona_texture_native_bank(SGIKonaTextureState *s, uint32_t handle,
                                  bool bank_one)
{
    SGIKonaTextureObject *o = texture_object(s, handle, true);

    if (!o) {
        return false;
    }
    o->native_bank_one = bank_one;
    return true;
}

/*
 * Whether o's stored table is table: for a volume's record the table
 * alone, as a volume's levels do not alternate banks from bind bit 28,
 * and otherwise the table and the first level's bank. The object's own
 * kind is not compared. A 0x178 that frees a 2D texture while a volume's
 * record is current gives that texture the volume's size until its own
 * next record, which must still find it.
 */
static bool texture_table_matches(const SGIKonaTextureObject *o,
                                  const uint32_t table[16], bool volume,
                                  bool bank_one)
{
    return o->native_table_valid &&
           (volume || o->native_bank_one == bank_one) &&
           !memcmp(o->native_table, table, sizeof(o->native_table));
}

bool sgi_kona_texture_find_table(SGIKonaTextureState *s,
                                 const uint32_t table[16], bool volume,
                                 bool bank_one, uint32_t *handle)
{
    GHashTableIter iter;
    gpointer value;
    unsigned matches = 0;

    g_hash_table_iter_init(&iter, s->objects);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const SGIKonaTextureObject *o = value;

        if (o->owner == s->owner &&
            texture_table_matches(o, table, volume, bank_one)) {
            *handle = o->handle;
            matches++;
        }
    }
    return matches == 1;
}

bool sgi_kona_texture_table_matches(SGIKonaTextureState *s, uint32_t handle,
                                    const uint32_t table[16], bool volume,
                                    bool bank_one)
{
    const SGIKonaTextureObject *o = texture_object(s, handle, false);

    return o && texture_table_matches(o, table, volume, bank_one);
}

bool sgi_kona_texture_native_dimensions(SGIKonaTextureState *s, uint32_t handle,
                                       uint32_t width, uint32_t height)
{
    SGIKonaTextureObject *o;

    if (!texture_dimensions(width, height)) {
        return false;
    }
    o = texture_object(s, handle, true);
    if (!o) {
        return false;
    }
    o->native_width = width;
    o->native_height = height;
    o->native_depth = 0;
    o->native_one_dimensional = false;
    o->native_layout_valid = false;
    /* Only a volume has a layout mode one; the stream refuses it on 2D. */
    o->native_volume_single = false;
    return true;
}

bool sgi_kona_texture_native_dimensions_1d(SGIKonaTextureState *s,
                                           uint32_t handle, uint32_t width)
{
    SGIKonaTextureObject *o;

    if (!sgi_kona_texture_native_dimensions(s, handle, width, 1)) {
        return false;
    }
    o = texture_object(s, handle, false);
    o->native_one_dimensional = true;
    return true;
}

bool sgi_kona_texture_native_layout(SGIKonaTextureState *s, uint32_t handle,
                                   uint32_t physical_size)
{
    SGIKonaTextureObject *o = texture_object(s, handle, false);

    if (!o || !o->native_width ||
        (physical_size & ~(o->native_depth ? 0xfffu : 0xffu)) ||
        (physical_size & 15) > 13 || ((physical_size >> 4) & 15) > 13 ||
        (physical_size >> 8) > 13) {
        return false;
    }
    o->native_layout = physical_size;
    o->native_layout_valid = true;
    return true;
}

bool sgi_kona_texture_native_volume_dimensions(SGIKonaTextureState *s,
                                              uint32_t handle, uint32_t width,
                                              uint32_t height, uint32_t depth)
{
    SGIKonaTextureObject *o;

    if (!texture_dimensions(width, height) || !is_power_of_2(depth) ||
        !native_volume_dimensions(width, height, depth)) {
        return false;
    }
    o = texture_object(s, handle, true);
    if (!o) {
        return false;
    }
    o->native_width = width;
    o->native_height = height;
    o->native_depth = depth;
    o->native_one_dimensional = false;
    o->native_layout_valid = false;
    o->native_volume_single = false;
    return true;
}

bool sgi_kona_texture_native_volume_single(SGIKonaTextureState *s,
                                           uint32_t handle, bool single)
{
    SGIKonaTextureObject *o = texture_object(s, handle, false);

    if (!o || !o->native_depth) {
        return false;
    }
    o->native_volume_single = single;
    return true;
}

bool sgi_kona_texture_native_format(SGIKonaTextureState *s, uint32_t handle,
                                   uint32_t storage_format)
{
    SGIKonaTextureObject *o;

    if (storage_format > 0x7f) {
        return false;
    }
    o = texture_object(s, handle, true);
    if (!o) {
        return false;
    }
    o->native_format = storage_format;
    o->native_format_valid = true;
    return true;
}

bool sgi_kona_texture_native_internal_format(SGIKonaTextureState *s,
                                             uint32_t handle, uint32_t base,
                                             uint32_t internal)
{
    SGIKonaTextureObject *o;

    if (!texture_format(base, internal)) {
        return false;
    }
    o = texture_object(s, handle, true);
    if (!o) {
        return false;
    }
    o->native_base_format = base;
    o->native_internal_format = internal;
    o->native_internal_valid = true;
    return true;
}

static bool texture_detail_args(const uint32_t args[6])
{
    uint32_t base, internal;

    return args && (args[0] == UINT32_MAX ||
        (args[1] < 31 && args[2] < 31 &&
         texture_dimensions(1u << args[1], 1u << args[2]) &&
         sgi_kona_gl_texture_internal_format(args[3], &base, &internal) &&
         base != SGI_GL_DEPTH_COMPONENT));
}

bool sgi_kona_texture_detail_bind(SGIKonaTextureState *s,
                                  const uint32_t args[6])
{
    if (!texture_detail_args(args)) {
        return false;
    }
    memcpy(s->detail_args, args, sizeof(s->detail_args));
    s->detail_bound = args[0] != UINT32_MAX;
    return true;
}

static bool texture_wrap(uint32_t wrap)
{
    return wrap == SGI_GL_REPEAT || wrap == SGI_GL_CLAMP ||
           wrap == SGI_GL_CLAMP_TO_EDGE || wrap == SGI_GL_CLAMP_TO_BORDER;
}

static bool texture_params(const SGIKonaTextureParams *p)
{
    if ((p->min_filter != SGI_GL_NEAREST && p->min_filter != SGI_GL_LINEAR &&
         (p->min_filter < SGI_GL_NEAREST_MIPMAP_NEAREST ||
          p->min_filter > SGI_GL_LINEAR_MIPMAP_LINEAR)) ||
        (p->mag_filter != SGI_GL_NEAREST && p->mag_filter != SGI_GL_LINEAR &&
         p->mag_filter != SGI_GL_LINEAR_SHARPEN_SGIS &&
         p->mag_filter != SGI_GL_LINEAR_SHARPEN_COLOR_SGIS &&
         p->mag_filter != SGI_GL_LINEAR_SHARPEN_ALPHA_SGIS &&
         p->mag_filter != SGI_GL_LINEAR_DETAIL_SGIS &&
         p->mag_filter != SGI_GL_LINEAR_DETAIL_COLOR_SGIS &&
         p->mag_filter != SGI_GL_LINEAR_DETAIL_ALPHA_SGIS) ||
        p->detail_level > 0 ||
        (p->detail_mode != SGI_GL_ADD && p->detail_mode != SGI_GL_MODULATE) ||
        p->filter_curve_slot >= SGI_KONA_TEXTURE_CURVE_SLOTS ||
        !texture_wrap(p->wrap_s) || !texture_wrap(p->wrap_t) ||
        !texture_wrap(p->wrap_r) ||
        !isfinite(p->min_lod) || !isfinite(p->max_lod) ||
        !isfinite(p->lod_scale_s) || !isfinite(p->lod_scale_t) ||
        !isfinite(p->lod_scale_r) ||
        p->lod_scale_s <= 0 || p->lod_scale_t <= 0 || p->lod_scale_r <= 0 ||
        p->min_lod > p->max_lod ||
        (p->shadow_compare &&
         p->shadow_compare != SGI_GL_TEXTURE_LEQUAL_R_SGIX &&
         p->shadow_compare != SGI_GL_TEXTURE_GEQUAL_R_SGIX) ||
        !isfinite(p->shadow_ambient) || p->shadow_ambient < 0 ||
        p->shadow_ambient > 1) {
        return false;
    }
    for (unsigned k = 0; k < 4; k++) {
        if (!isfinite(p->border_color[k]) || !isfinite(p->post_scale[k]) ||
            !isfinite(p->post_bias[k])) {
            return false;
        }
    }
    return true;
}

bool sgi_kona_texture_bind(SGIKonaTextureState *s, uint32_t handle,
                          const SGIKonaTextureParams *params)
{
    SGIKonaTextureObject *o;

    if (handle == UINT32_MAX) {
        s->bound = false;
        return true;
    }
    if (params && !texture_params(params)) {
        return false;
    }
    o = texture_object(s, handle, true);
    if (!o) {
        return false;
    }
    if (params) {
        o->params = *params;
    }
    s->bound = true;
    s->bound_handle = handle;
    return true;
}

static void texture_object_drop(SGIKonaTextureState *s,
                                SGIKonaTextureObject *o)
{
    for (unsigned k = 0; k < o->count; k++) {
        s->bytes -= level_bytes(&o->levels[k]);
    }
    for (unsigned k = 0; k < o->native_count; k++) {
        s->bytes -= native_bytes(&o->native[k]);
    }
    s->bytes -= TEXTURE_OBJECT_BYTES;
    g_hash_table_remove(s->objects, texture_key(o->owner, o->handle));
    texture_object_free(o);
}

void sgi_kona_texture_delete(SGIKonaTextureState *s, uint32_t handle)
{
    SGIKonaTextureObject *o = texture_object(s, handle, false);

    if (o) {
        texture_object_drop(s, o);
    }
    if (s->bound && s->bound_handle == handle) {
        s->bound = false;
    }
}

void sgi_kona_texture_drop_owner(SGIKonaTextureState *s, uint32_t owner)
{
    GHashTableIter iter;
    gpointer value;
    GPtrArray *drop = g_ptr_array_new();

    assert(owner < SGI_KONA_TEXTURE_OWNERS);
    g_hash_table_iter_init(&iter, s->objects);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (((SGIKonaTextureObject *)value)->owner == owner) {
            g_ptr_array_add(drop, value);
        }
    }
    for (unsigned i = 0; i < drop->len; i++) {
        texture_object_drop(s, g_ptr_array_index(drop, i));
    }
    g_ptr_array_free(drop, true);
    s->owner_bound[owner] = false;
    if (owner == s->owner) {
        s->bound = false;
    }
}

static bool texture_read(void *opaque, uint32_t level, uint32_t x, uint32_t y,
                          uint32_t *rgba)
{
    const SGIKonaTextureLevel *l = opaque;

    if (l->level != level || x >= l->width || y >= l->height || !l->pixels) {
        return false;
    }
    *rgba = l->pixels[(size_t)y * l->width + x];
    return true;
}

static bool texture_native_read(void *opaque, uint32_t level, uint32_t x,
                                uint32_t y, uint32_t *rgba)
{
    const SGIKonaTextureLevel *l = opaque;

    if (x >= l->width || y >= l->height || !l->pixels) {
        return false;
    }
    *rgba = l->pixels[(size_t)(y + l->origin_y) *
                      (l->stride_width ? l->stride_width : l->width) +
                      x + l->origin_x];
    if (l->base_format == SGI_GL_RGB) {
        *rgba |= 0xff;
    }
    return true;
}

static bool texture_native_read_border_3d(void *opaque, uint32_t level,
                                          int32_t x, int32_t y, int32_t z,
                                          uint32_t *rgba)
{
    const SGIKonaTextureLevel *l = opaque;
    const SGIKonaTextureNativeImage *n = l->native_border;
    uint32_t sx, sy;
    uint64_t at;

    /* R border addressing is not established by the inspected producer. */
    if (!n || z < 0 || z >= (int32_t)l->depth ||
        (n->header[0] == SGI_GL_TEXTURE_1D && y) ||
        x < -1 || y < -1 || x > (int32_t)l->width ||
        y > (int32_t)l->height ||
        (x >= 0 && x < l->width && y >= 0 && y < l->height)) {
        return false;
    }
    sx = x < 0 ? (1u << (n->header[16] & 15)) - 1 : x;
    sy = y < 0 ? (1u << ((n->header[16] >> 4) & 15)) - 1 : y;
    /* A shared plane's interior coverage cannot establish a negative border. */
    if ((n->image.pixels == l->pixels &&
         ((x < 0 && sx < l->width) || (y < 0 && sy < l->height))) ||
        sx >= n->image.width || sy >= n->image.height ||
        z >= n->image.depth) {
        return false;
    }
    at = ((uint64_t)z * n->image.height + sy) * n->image.width + sx;
    if (!native_covered(n->coverage, at)) {
        return false;
    }
    *rgba = n->image.pixels[at];
    if (l->base_format == SGI_GL_RGB) {
        *rgba |= 0xff;
    }
    return true;
}

static bool texture_native_read_border(void *opaque, uint32_t level,
                                       int32_t x, int32_t y, uint32_t *rgba)
{
    return texture_native_read_border_3d(opaque, level, x, y, 0, rgba);
}

static bool texture_native_uses_border(uint32_t wrap)
{
    return wrap == SGI_GL_CLAMP || wrap == SGI_GL_CLAMP_TO_BORDER;
}

/* The word 17 layout mode an object's bind expects of its uploads. */
static uint32_t texture_native_object_mode(const SGIKonaTextureObject *o)
{
    return !o->native_depth ? 0 : o->native_volume_single ? 0x20 : 0x40;
}

/*
 * The physical T exponent's floor, the tile minimum of the object's
 * layout mode: two in modes zero and one, one in mode two.
 */
static int texture_native_t_minimum(const SGIKonaTextureObject *o)
{
    return texture_native_object_mode(o) == 0x40 ? 1 : 2;
}

static const SGIKonaTextureNativeImage *texture_native_border_image(
    const SGIKonaTextureObject *o, const SGIKonaTextureNativeImage *interior,
    unsigned level)
{
    uint32_t h[20];
    uint32_t width = MAX(1u, o->native_width >> level);
    uint32_t height = MAX(1u, o->native_height >> level);
    bool volume = o->native_depth != 0;

    memcpy(h, interior->header, sizeof(h));
    h[16] = MIN(ctz32(width) + 1, 3) |
            (MIN(ctz32(height) + 1, texture_native_t_minimum(o)) << 4);
    if (volume) {
        uint32_t depth = MAX(1u, o->native_depth >> level);

        h[16] |= MIN(ctz32(depth) + 1, 1) << 8;
    }
    for (unsigned k = 0; k < o->native_count; k++) {
        if (texture_native_key(o->native[k].header, h) &&
            (o->native[k].image.base_format == SGI_GL_DEPTH_COMPONENT) ==
            (interior->image.base_format == SGI_GL_DEPTH_COMPONENT)) {
            return &o->native[k];
        }
    }
    return NULL;
}

static bool texture_native_read_3d(void *opaque, uint32_t level, uint32_t x,
                                   uint32_t y, uint32_t z, uint32_t *rgba)
{
    const SGIKonaTextureLevel *l = opaque;

    if (x >= l->width || y >= l->height || z >= l->depth || !l->pixels) {
        return false;
    }
    *rgba = l->pixels[((size_t)z * (l->stride_height ? l->stride_height :
                                  l->height) + y) *
                     (l->stride_width ? l->stride_width : l->width) + x];
    if (l->base_format == SGI_GL_RGB) {
        *rgba |= 0xff;
    }
    return true;
}

static bool texture_native_covers(const SGIKonaTextureNativeImage *n,
                                   uint32_t x0, uint32_t y0, uint32_t width,
                                   uint32_t height, uint32_t depth,
                                   uint64_t *tests)
{
    /*
     * covered counts the set coverage bits exactly, so a complete image
     * covers every rectangle in it. Resolving runs for every draw, and
     * scanning a whole large image's bits each time cost pfCity's draw
     * thread most of its time.
     */
    if (n->covered == native_pixels(n)) {
        return true;
    }
    for (uint32_t z = 0; z < depth; z++) {
        for (uint32_t y = 0; y < height; y++) {
            uint64_t row = ((uint64_t)z * n->image.height + y0 + y) *
                           n->image.width + x0;

            for (uint32_t x = 0; x < width; x++) {
                ++*tests;
                if (!native_covered(n->coverage, row + x)) {
                    return false;
                }
            }
        }
    }
    return true;
}

/*
 * Whether native image n holds level `level` of o's interior, and where:
 * *x0 and *y0 are the level's texel (0, 0) in the image.
 *
 * A 2D level smaller than its tile minimum (eight by four texels in
 * layout mode zero) is either at the tile's origin or in its far corner,
 * at the tile's extent minus the level's. tex_cube (IRIX 6.5.30) loaded
 * its 4x4, 2x2 and 1x1 levels into 8x4 tiles at (4, 0), (6, 2) and
 * (7, 3), each header naming its own origin; perfly's first two textures
 * put theirs at the origin under the same tile size. What makes the
 * library choose one placement is not read, so the model takes the level
 * where its texels were received: the origin first, then the far corner.
 */
static bool texture_native_interior(const SGIKonaTextureObject *o,
                                      const SGIKonaTextureNativeImage *n,
                                      unsigned level, uint32_t *x0,
                                      uint32_t *y0, uint64_t *tests)
{
    bool volume = o->native_depth != 0;
    uint32_t width = MAX(1u, o->native_width >> level);
    uint32_t height = MAX(1u, o->native_height >> level);
    uint32_t depth = volume ? MAX(1u, o->native_depth >> level) : 1;
    int ps = o->native_layout & 15;
    int pt = (o->native_layout >> 4) & 15;
    int pr = (o->native_layout >> 8) & 15;
    int t_minimum = texture_native_t_minimum(o);
    uint32_t geometry = MAX(ps - (int)level, 3) |
                        (MAX(pt - (int)level, t_minimum) << 4);

    if (volume) {
        geometry |= MAX(pr - (int)level, 1) << 8;
    }
    *x0 = *y0 = 0;
    if (o->params.base_level || !o->native_width ||
        (n->header[17] & 0x60) != texture_native_object_mode(o) ||
        n->header[16] != geometry || width > n->image.width ||
        height > n->image.height || depth > n->image.depth) {
        return false;
    }
    if (texture_native_covers(n, 0, 0, width, height, depth, tests)) {
        return true;
    }
    if (volume || (width == n->image.width && height == n->image.height)) {
        return false;
    }
    *x0 = n->image.width - width;
    *y0 = n->image.height - height;
    return texture_native_covers(n, *x0, *y0, width, height, 1, tests);
}

static uint32_t texture_native_view(SGIKonaTextureObject *o)
{
    unsigned count = 0;
    bool volume = o->native_depth != 0;
    unsigned limit = volume ? 10 : SGI_KONA_TEXTURE_TABLE_LEVELS;

    if (o->native_one_dimensional && !o->native_layout_valid) {
        return 0;
    }
    for (unsigned level = 0; level < limit; level++) {
        SGIKonaTextureNativeImage *interior = NULL;
        SGIKonaTextureLevel *image = NULL;
        uint32_t image_x = 0, image_y = 0, ox = 0, oy = 0;
        uint32_t descriptor = o->native_table[level];
        bool ambiguous = false;

        for (unsigned k = 0; k < o->native_count; k++) {
            SGIKonaTextureNativeImage *n = &o->native[k];
            const uint32_t *h = n->header;
            bool matches;

            if (volume && o->native_volume_single) {
                /*
                 * Mode one stores one level at the table's first pair.
                 * Its bit three is the bank state, as in mode zero, so
                 * it does not choose among levels.
                 */
                matches = h[0] == SGI_GL_TEXTURE_3D && !level &&
                    (h[17] & 0x60) == 0x20 &&
                    h[18] == o->native_table[0] &&
                    h[19] == o->native_table[1] &&
                    (o->native_layout_valid ||
                     n->image.depth == o->native_depth);
            } else if (volume) {
                matches = h[0] == SGI_GL_TEXTURE_3D &&
                    (h[17] & 0x60) == 0x40 &&
                    ((h[17] >> 3) & 1) == (level & 1) &&
                    h[18] == o->native_table[level & ~1u] &&
                    h[19] == o->native_table[(level & ~1u) + 1] &&
                    (o->native_layout_valid ||
                     n->image.depth == MAX(1u, o->native_depth >> level));
            } else {
                /*
                 * The level's table word is its address, which the load
                 * header carries in word 19 when its bank bit is set and
                 * in word 18 otherwise. Banks alternate by level from the
                 * one the binding gives the first.
                 */
                bool bank = o->native_bank_one ^ (level & 1);

                matches = h[0] == (o->native_one_dimensional ?
                                  SGI_GL_TEXTURE_1D : SGI_GL_TEXTURE_2D) &&
                    !!(h[17] & 8) == bank &&
                    (bank ? (!h[18] && h[19] == descriptor) :
                            (!h[19] && h[18] == descriptor));
            }

            matches = matches &&
                      (!o->native_format_valid || h[9] == o->native_format);
            if (o->native_layout_valid) {
                matches = matches &&
                          texture_native_interior(o, n, level, &ox, &oy,
                                                  &o->coverage_tests);
            } else {
                matches = matches && n->covered == native_pixels(n) &&
                          texture_dimensions(n->image.width, n->image.height);
            }
            if (matches && o->native_width && !o->native_layout_valid) {
                matches = n->image.width == MAX(1u, o->native_width >> level) &&
                          n->image.height == MAX(1u, o->native_height >> level);
            }
            if (matches) {
                if (image) {
                    ambiguous = true;
                    break;
                }
                image = &n->image;
                interior = n;
                image_x = ox;
                image_y = oy;
            }
        }
        if (ambiguous) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-kona: ambiguous native texture level %u "
                          "for handle 0x%x\n", level, o->handle);
            if (volume || o->native_layout_valid) {
                return UINT32_MAX;
            }
        } else if (image) {
            SGIKonaTextureLevel *view = &o->native_view[count];
            bool border = o->native_layout_valid &&
                (texture_native_uses_border(o->params.wrap_s) ||
                 (!o->native_one_dimensional &&
                  texture_native_uses_border(o->params.wrap_t)) ||
                 (volume && texture_native_uses_border(o->params.wrap_r)));
            /*
             * Where a far-corner level's border texels sit is not read: the
             * border reader addresses from the tile origin, which there
             * holds the level's own interior. Give it no reader so a border
             * read refuses rather than returning interior texels.
             */
            bool border_read = border && !image_x && !image_y;

            if (o->native_internal_valid &&
                (image->base_format == SGI_GL_DEPTH_COMPONENT) !=
                (o->native_base_format == SGI_GL_DEPTH_COMPONENT)) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-kona: incompatible depth texture storage "
                              "for handle 0x%x\n", o->handle);
                return UINT32_MAX;
            }
            *view = *image;
            if (o->native_layout_valid) {
                view->stride_width = image->width;
                view->stride_height = image->height;
                view->depth = volume ? MAX(1u, o->native_depth >> level) : 1;
                view->width = MAX(1u, o->native_width >> level);
                view->height = MAX(1u, o->native_height >> level);
            }
            view->origin_x = image_x;
            view->origin_y = image_y;
            if (o->native_internal_valid) {
                view->base_format = o->native_base_format;
                view->internal_format = o->native_internal_format;
            }
            view->native_border = border_read ?
                texture_native_border_image(o, interior, level) : NULL;
            o->view[count++] = (SGIGLTexLevel) {
                .border = border,
                .read_border_texel = border_read && !volume ?
                                      texture_native_read_border : NULL,
                .read_border_texel_3d = border_read && volume ?
                                         texture_native_read_border_3d : NULL,
                .level = level, .width = view->width, .height = view->height,
                .depth = view->depth,
                .base_format = view->base_format,
                .internal_format = view->internal_format,
                .read_texel = texture_native_read,
                .read_texel_3d = texture_native_read_3d, .opaque = view,
            };
        }
    }
    return count;
}

static void texture_post_filter(void *opaque, double rgba[4])
{
    const SGIKonaTextureObject *o = opaque;

    for (unsigned k = 0; k < 4; k++) {
        rgba[k] = CLAMP(rgba[k] * o->params.post_scale[k] +
                        o->params.post_bias[k], 0.0, 1.0);
    }
}

static bool texture_filter_weight(void *opaque, double lambda, double *weight)
{
    const SGIKonaTextureObject *o = opaque;
    double position, fraction;
    unsigned node;

    if (!o->filter_curve || !isfinite(lambda)) {
        return false;
    }
    /* Model interpolation in rho; the physical receiver remains unknown. */
    position = lambda >= 0 ? 63 : MIN(exp2(lambda) * 64, 63);
    node = floor(position);
    fraction = position - node;
    *weight = o->filter_curve[node] * (1 - fraction) +
              o->filter_curve[MIN(node + 1, 63)] * fraction;
    *weight *= o->params.mag_filter == SGI_GL_LINEAR_DETAIL_SGIS ||
               o->params.mag_filter == SGI_GL_LINEAR_DETAIL_COLOR_SGIS ||
               o->params.mag_filter == SGI_GL_LINEAR_DETAIL_ALPHA_SGIS ?
               0.5 : -1;
    return true;
}

static bool texture_detail_view(SGIKonaTextureState *s,
                                  SGIKonaTextureObject *primary)
{
    const uint32_t *args = s->detail_args;
    SGIKonaTextureObject *o;
    SGIKonaTextureLevel *image = NULL;
    uint32_t base, internal, storage;

    if (!s->detail_bound) {
        return true;
    }
    if (!sgi_kona_gl_texture_internal_format(args[3], &base, &internal) ||
        !sgi_kona_gl_texture_format_storage(args[3], &storage)) {
        return false;
    }
    o = texture_object(s, args[0], false);
    if (o) {
        for (unsigned k = 0; k < o->native_count; k++) {
            SGIKonaTextureNativeImage *n = &o->native[k];

            if (n->header[0] == SGI_GL_TEXTURE_2D &&
                n->header[18] == args[4] && n->header[19] == args[5] &&
                n->header[9] == storage && n->covered == native_pixels(n) &&
                n->image.width == (1u << args[1]) &&
                n->image.height == (1u << args[2])) {
                if (image) {
                    qemu_log_mask(LOG_UNIMP,
                                  "sgi-kona: ambiguous detail image "
                                  "for handle 0x%x\n", args[0]);
                    return false;
                }
                image = &n->image;
            }
        }
    }
    if (!image) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: unresolved detail image for handle 0x%x\n",
                      args[0]);
        return false;
    }
    primary->detail_pixels = *image;
    primary->detail_pixels.base_format = base;
    primary->detail_pixels.internal_format = internal;
    primary->detail_view = (SGIGLTexLevel) {
        .width = image->width, .height = image->height,
        .base_format = base, .internal_format = internal,
        .read_texel = texture_native_read, .opaque = &primary->detail_pixels,
    };
    primary->image.detail_image = &primary->detail_view;
    return true;
}

const SGIGLTexImage *sgi_kona_texture_resolve(SGIKonaTextureState *s)
{
    SGIKonaTextureObject *o = s->bound ?
        texture_object(s, s->bound_handle, false) : NULL;

    if (!o) {
        return NULL;
    }
    for (unsigned k = 0; k < o->count; k++) {
        SGIKonaTextureLevel *l = &o->levels[k];

        o->view[k] = (SGIGLTexLevel) {
            .level = l->level, .width = l->width, .height = l->height,
            .base_format = l->base_format,
            .internal_format = l->internal_format,
            .read_texel = texture_read, .opaque = l,
        };
    }
    o->pyramid = (SGIGLTexPyramid) {
        .levels = o->view,
        .count = o->native_table_valid ? texture_native_view(o) : o->count,
        .base_level = o->params.base_level, .max_level = o->params.max_level,
        .min_lod = o->params.min_lod, .max_lod = o->params.max_lod,
    };
    o->filter_curve = sgi_kona_texture_curve(s, o->params.filter_curve_slot);
    o->image = (SGIGLTexImage) {
        .min_filter = o->params.min_filter, .mag_filter = o->params.mag_filter,
        .wrap_s = o->params.wrap_s, .wrap_t = o->params.wrap_t,
        .wrap_r = o->params.wrap_r, .volume = o->native_depth != 0,
        .one_dimensional = o->native_one_dimensional,
        .pyramid = &o->pyramid,
        .lod_scale_s = o->params.lod_scale_s,
        .lod_scale_t = o->params.lod_scale_t,
        .lod_scale_r = o->params.lod_scale_r,
        .post_filter = texture_post_filter, .opaque = o,
        .read_filter_weight = texture_filter_weight,
        .detail_level = o->params.detail_level,
        .detail_mode = o->params.detail_mode,
        .shadow_compare = o->params.shadow_compare,
        .shadow_ambient = o->params.shadow_ambient,
    };
    if ((o->params.mag_filter == SGI_GL_LINEAR_DETAIL_SGIS ||
         o->params.mag_filter == SGI_GL_LINEAR_DETAIL_ALPHA_SGIS ||
         o->params.mag_filter == SGI_GL_LINEAR_DETAIL_COLOR_SGIS) &&
        !texture_detail_view(s, o)) {
        o->image.mag_filter = UINT32_MAX;
    }
    memcpy(o->image.border_color, o->params.border_color,
            sizeof(o->image.border_color));
    return &o->image;
}

static void texture_params_save(QEMUFile *f, const SGIKonaTextureParams *p)
{
    union { float f; uint32_t u; } single;
    union { double d; uint64_t u; } wide;

    qemu_put_be32(f, p->min_filter);
    qemu_put_be32(f, p->mag_filter);
    qemu_put_be32(f, p->wrap_s);
    qemu_put_be32(f, p->wrap_t);
    for (unsigned k = 0; k < 4; k++) {
        single.f = p->border_color[k];
        qemu_put_be32(f, single.u);
    }
    qemu_put_be32(f, p->base_level);
    qemu_put_be32(f, p->max_level);
    wide.d = p->min_lod;
    qemu_put_be64(f, wide.u);
    wide.d = p->max_lod;
    qemu_put_be64(f, wide.u);
    for (unsigned k = 0; k < 4; k++) {
        wide.d = p->post_scale[k];
        qemu_put_be64(f, wide.u);
        wide.d = p->post_bias[k];
        qemu_put_be64(f, wide.u);
    }
    wide.d = p->lod_scale_s;
    qemu_put_be64(f, wide.u);
    wide.d = p->lod_scale_t;
    qemu_put_be64(f, wide.u);
}

static bool texture_params_load(QEMUFile *f, SGIKonaTextureParams *p)
{
    union { float f; uint32_t u; } single;
    union { double d; uint64_t u; } wide;

    p->min_filter = qemu_get_be32(f);
    p->mag_filter = qemu_get_be32(f);
    p->wrap_s = qemu_get_be32(f);
    p->wrap_t = qemu_get_be32(f);
    for (unsigned k = 0; k < 4; k++) {
        single.u = qemu_get_be32(f);
        p->border_color[k] = single.f;
    }
    p->base_level = qemu_get_be32(f);
    p->max_level = qemu_get_be32(f);
    wide.u = qemu_get_be64(f);
    p->min_lod = wide.d;
    wide.u = qemu_get_be64(f);
    p->max_lod = wide.d;
    for (unsigned k = 0; k < 4; k++) {
        wide.u = qemu_get_be64(f);
        p->post_scale[k] = wide.d;
        wide.u = qemu_get_be64(f);
        p->post_bias[k] = wide.d;
    }
    wide.u = qemu_get_be64(f);
    p->lod_scale_s = wide.d;
    wide.u = qemu_get_be64(f);
    p->lod_scale_t = wide.d;
    return !qemu_file_get_error(f) && texture_params(p);
}

/*
 * The layout marker the table's own stream starts with. Migration is
 * between identical builds only, so the loader takes this layout alone.
 */
#define TEXTURE_STREAM_VERSION 10

static bool texture_save(QEMUFile *f, void *pv, size_t size,
                          const VMStateField *field, JSONWriter *vmdesc,
                          Error **errp)
{
    SGIKonaTextureState *s = pv;
    GHashTableIter iter;
    gpointer value;

    qemu_put_be32(f, TEXTURE_STREAM_VERSION);
    qemu_put_be32(f, g_hash_table_size(s->objects));
    qemu_put_be32(f, s->bound);
    qemu_put_be32(f, s->bound_handle);
    qemu_put_be32(f, s->owner);
    for (unsigned k = 0; k < SGI_KONA_TEXTURE_OWNERS; k++) {
        qemu_put_be32(f, s->owner_bound[k]);
        qemu_put_be32(f, s->owner_bound_handle[k]);
    }
    g_hash_table_iter_init(&iter, s->objects);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        SGIKonaTextureObject *o = value;

        qemu_put_be32(f, o->owner);
        qemu_put_be32(f, o->handle);
        qemu_put_be32(f, o->count);
        texture_params_save(f, &o->params);
        for (unsigned k = 0; k < o->count; k++) {
            SGIKonaTextureLevel *l = &o->levels[k];

            qemu_put_be32(f, l->level);
            qemu_put_be32(f, l->width);
            qemu_put_be32(f, l->height);
            qemu_put_be32(f, l->base_format);
            qemu_put_be32(f, l->internal_format);
            for (uint64_t p = 0; p < (uint64_t)l->width * l->height; p++) {
                qemu_put_be32(f, l->pixels[p]);
            }
        }
        qemu_put_be32(f, o->native_table_valid);
        for (unsigned k = 0; k < SGI_KONA_TEXTURE_TABLE_LEVELS; k++) {
            qemu_put_be32(f, o->native_table[k]);
        }
        qemu_put_be32(f, o->native_count);
        for (unsigned k = 0; k < o->native_count; k++) {
            SGIKonaTextureNativeImage *n = &o->native[k];
            SGIKonaTextureLevel *l = &n->image;

            for (unsigned j = 0; j < ARRAY_SIZE(n->header); j++) {
                qemu_put_be32(f, n->header[j]);
            }
            qemu_put_be32(f, l->width);
            qemu_put_be32(f, l->height);
            qemu_put_be32(f, l->base_format);
            qemu_put_be32(f, l->internal_format);
            qemu_put_be32(f, l->depth);
            for (uint64_t p = 0; p < native_pixels(n); p++) {
                qemu_put_be32(f, l->pixels[p]);
            }
            qemu_put_buffer(f, n->coverage, native_coverage_bytes(n));
        }
        qemu_put_be32(f, o->native_width);
        qemu_put_be32(f, o->native_height);
        qemu_put_be32(f, o->native_format_valid);
        qemu_put_be32(f, o->native_format);
        qemu_put_be32(f, o->params.filter_curve_slot);
        qemu_put_be32(f, o->native_internal_valid);
        qemu_put_be32(f, o->native_base_format);
        qemu_put_be32(f, o->native_internal_format);
        qemu_put_be32(f, o->params.detail_level);
        qemu_put_be32(f, o->params.detail_mode);
        qemu_put_be32(f, o->native_depth);
        qemu_put_be32(f, o->params.wrap_r);
        union { double d; uint64_t u; } scale_r = {
            .d = o->params.lod_scale_r,
        };
        qemu_put_be64(f, scale_r.u);
        qemu_put_be32(f, o->native_layout_valid |
                         (o->native_one_dimensional << 1) |
                         (o->native_bank_one << 2) |
                         (o->native_volume_single << 3));
        qemu_put_be32(f, o->native_layout);
        qemu_put_be32(f, o->params.shadow_compare);
        union { double d; uint64_t u; } ambient = {
            .d = o->params.shadow_ambient,
        };
        qemu_put_be64(f, ambient.u);
    }
    qemu_put_be32(f, g_hash_table_size(s->filter_curves));
    g_hash_table_iter_init(&iter, s->filter_curves);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        SGIKonaTextureCurve *curve = value;

        qemu_put_be32(f, curve->slot);
        for (unsigned k = 0; k < ARRAY_SIZE(curve->words); k++) {
            qemu_put_be32(f, curve->words[k]);
        }
    }
    qemu_put_be32(f, s->detail_bound);
    for (unsigned k = 0; k < ARRAY_SIZE(s->detail_args); k++) {
        qemu_put_be32(f, s->detail_args[k]);
    }
    if (qemu_file_get_error(f)) {
        error_setg(errp, "KONA texture state could not be saved");
        return false;
    }
    return true;
}

/* Check each count, dimension and budget before allocating its image. */
static bool texture_load(QEMUFile *f, void *pv, size_t size,
                          const VMStateField *field, Error **errp)
{
    SGIKonaTextureState *s = pv;
    SGIKonaTextureState tmp;
    uint32_t version = qemu_get_be32(f), count = qemu_get_be32(f);
    uint32_t bound = qemu_get_be32(f), handle = qemu_get_be32(f);
    uint32_t owner = qemu_get_be32(f);
    uint32_t curves, detail_valid;

    sgi_kona_texture_init(&tmp);
    tmp.budget = s->budget;
    for (unsigned k = 0; k < SGI_KONA_TEXTURE_OWNERS; k++) {
        uint32_t b = qemu_get_be32(f);

        tmp.owner_bound[k] = b;
        tmp.owner_bound_handle[k] = qemu_get_be32(f);
        if (b > 1) {
            goto invalid;
        }
    }
    if (version != TEXTURE_STREAM_VERSION || bound > 1 ||
        owner >= SGI_KONA_TEXTURE_OWNERS ||
        count > s->budget / TEXTURE_OBJECT_BYTES) {
        goto invalid;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t object_owner = qemu_get_be32(f);
        uint32_t id = qemu_get_be32(f), levels = qemu_get_be32(f);
        uint32_t valid, native_count;
        union { double d; uint64_t u; } scale_r, ambient;
        SGIKonaTextureObject *o;

        if (object_owner >= SGI_KONA_TEXTURE_OWNERS) {
            goto invalid;
        }
        tmp.owner = object_owner;
        if (levels > SGI_GL_TEX_LEVELS_MAX || id == UINT32_MAX ||
            texture_object(&tmp, id, false)) {
            goto invalid;
        }
        o = texture_object(&tmp, id, true);
        if (!o || !texture_params_load(f, &o->params)) {
            goto invalid;
        }
        for (uint32_t k = 0; k < levels; k++) {
            SGIKonaTextureLevel *l = &o->levels[k];
            uint64_t bytes;

            l->level = qemu_get_be32(f);
            l->width = qemu_get_be32(f);
            l->height = qemu_get_be32(f);
            l->base_format = qemu_get_be32(f);
            l->internal_format = qemu_get_be32(f);
            if (l->level >= SGI_GL_TEX_LEVELS_MAX ||
                !texture_dimensions(l->width, l->height) ||
                !texture_format(l->base_format, l->internal_format) ||
                (k && o->levels[k - 1].level >= l->level)) {
                goto invalid;
            }
            bytes = level_bytes(l);
            if (bytes > tmp.budget - tmp.bytes ||
                qemu_file_get_error(f)) {
                goto invalid;
            }
            l->pixels = g_try_malloc(bytes);
            if (!l->pixels) {
                goto invalid;
            }
            o->count++;
            tmp.bytes += bytes;
            for (uint64_t p = 0; p < bytes / sizeof(*l->pixels); p++) {
                l->pixels[p] = qemu_get_be32(f);
                if (qemu_file_get_error(f)) {
                    goto invalid;
                }
            }
        }
        valid = qemu_get_be32(f);

        if (valid > 1) {
            goto invalid;
        }
        o->native_table_valid = valid;
        for (unsigned k = 0; k < SGI_KONA_TEXTURE_TABLE_LEVELS; k++) {
            o->native_table[k] = qemu_get_be32(f);
        }
        native_count = qemu_get_be32(f);
        if (native_count > SGI_GL_TEX_LEVELS_MAX) {
            goto invalid;
        }
        for (unsigned k = 0; k < native_count; k++) {
            SGIKonaTextureNativeImage *n = &o->native[k];
            SGIKonaTextureLevel *l = &n->image;
            uint32_t *h = n->header;
            uint64_t bytes, pixels_count;
            bool volume;

            for (unsigned j = 0; j < ARRAY_SIZE(n->header); j++) {
                h[j] = qemu_get_be32(f);
            }
            l->width = qemu_get_be32(f);
            l->height = qemu_get_be32(f);
            l->base_format = qemu_get_be32(f);
            l->internal_format = qemu_get_be32(f);
            l->depth = qemu_get_be32(f);
            volume = h[0] == SGI_GL_TEXTURE_3D;

            if (!native_volume_dimensions(l->width, l->height, l->depth) ||
                (!volume && l->depth != 1) ||
                (volume && h[15] >= l->depth) ||
                !native_dimensions(l->width, l->height) ||
                !texture_format(l->base_format, l->internal_format) ||
                (!volume && h[0] != SGI_GL_TEXTURE_2D &&
                 h[0] != SGI_GL_TEXTURE_1D) ||
                (h[0] == SGI_GL_TEXTURE_1D &&
                 (l->height != 1 || h[2] != 1 || h[12] ||
                  h[14] || h[15])) || h[10] != id ||
                !texture_native_mode_valid(h) ||
                (!volume && h[18] && h[19]) ||
                !h[1] || !h[2] || h[13] < h[11] || h[14] < h[12] ||
                (uint64_t)h[13] - h[11] + 1 != h[1] ||
                (uint64_t)h[14] - h[12] + 1 != h[2] ||
                h[13] >= l->width || h[14] >= l->height) {
                goto invalid;
            }
            for (unsigned j = 0; j < k; j++) {
                if (texture_native_key(o->native[j].header, h)) {
                    goto invalid;
                }
            }
            bytes = native_bytes(n);
            if (bytes > tmp.budget - tmp.bytes ||
                qemu_file_get_error(f)) {
                goto invalid;
            }
            l->pixels = g_try_malloc(native_pixels(n) * sizeof(*l->pixels));
            n->coverage = g_try_malloc0(native_coverage_bytes(n));
            if (!l->pixels || !n->coverage) {
                g_free(l->pixels);
                g_free(n->coverage);
                l->pixels = NULL;
                n->coverage = NULL;
                goto invalid;
            }
            o->native_count++;
            tmp.bytes += bytes;
            for (uint64_t p = 0; p < native_pixels(n); p++) {
                l->pixels[p] = qemu_get_be32(f);
                if (qemu_file_get_error(f)) {
                    goto invalid;
                }
            }
            pixels_count = native_pixels(n);
            if (qemu_get_buffer(f, n->coverage,
                                native_coverage_bytes(n)) !=
                native_coverage_bytes(n) ||
                (pixels_count % 8 &&
                 (n->coverage[pixels_count / 8] >>
                  (pixels_count % 8)))) {
                goto invalid;
            }
            for (uint64_t p = 0; p < pixels_count; p++) {
                if (native_covered(n->coverage, p)) {
                    n->covered++;
                } else if (l->pixels[p]) {
                    goto invalid;
                }
            }
            for (uint32_t y = h[12]; y <= h[14]; y++) {
                uint64_t row =
                    ((uint64_t)(volume ? h[15] : 0) * l->height + y) *
                    l->width;

                for (uint32_t x = h[11]; x <= h[13]; x++) {
                    if (!native_covered(n->coverage, row + x)) {
                        goto invalid;
                    }
                }
            }
        }
        o->native_width = qemu_get_be32(f);
        o->native_height = qemu_get_be32(f);
        if ((o->native_width || o->native_height) &&
            !texture_dimensions(o->native_width, o->native_height)) {
            goto invalid;
        }
        valid = qemu_get_be32(f);
        o->native_format = qemu_get_be32(f);
        if (valid > 1 || o->native_format > 0x7f) {
            goto invalid;
        }
        o->native_format_valid = valid;
        o->params.filter_curve_slot = qemu_get_be32(f);
        if (o->params.filter_curve_slot >= SGI_KONA_TEXTURE_CURVE_SLOTS) {
            goto invalid;
        }
        valid = qemu_get_be32(f);
        o->native_base_format = qemu_get_be32(f);
        o->native_internal_format = qemu_get_be32(f);
        o->params.detail_level = qemu_get_be32(f);
        o->params.detail_mode = qemu_get_be32(f);
        if (valid > 1 ||
            (valid && !texture_format(o->native_base_format,
                                      o->native_internal_format)) ||
            !texture_params(&o->params)) {
            goto invalid;
        }
        o->native_internal_valid = valid;
        o->native_depth = qemu_get_be32(f);
        o->params.wrap_r = qemu_get_be32(f);
        scale_r.u = qemu_get_be64(f);
        o->params.lod_scale_r = scale_r.d;
        if ((o->native_depth &&
             (!is_power_of_2(o->native_depth) ||
              !native_volume_dimensions(o->native_width, o->native_height,
                                         o->native_depth))) ||
            !texture_params(&o->params)) {
            goto invalid;
        }
        valid = qemu_get_be32(f);
        o->native_layout = qemu_get_be32(f);
        if (valid > 15 || ((valid & 8) && !o->native_depth) ||
            (o->native_layout & ~(o->native_depth ? 0xfffu : 0xffu)) ||
            (o->native_layout & 15) > 13 ||
            ((o->native_layout >> 4) & 15) > 13 ||
            (o->native_layout >> 8) > 13 ||
            ((valid & 1) && !o->native_width) ||
            ((valid & 2) && (o->native_depth || !o->native_width ||
                             o->native_height != 1))) {
            goto invalid;
        }
        o->native_layout_valid = valid & 1;
        o->native_one_dimensional = valid & 2;
        o->native_bank_one = valid & 4;
        o->native_volume_single = valid & 8;
        o->params.shadow_compare = qemu_get_be32(f);
        ambient.u = qemu_get_be64(f);
        o->params.shadow_ambient = ambient.d;
        if (!texture_params(&o->params)) {
            goto invalid;
        }
    }
    curves = qemu_get_be32(f);
    if (curves > SGI_KONA_TEXTURE_CURVE_SLOTS) {
        goto invalid;
    }
    for (unsigned i = 0; i < curves; i++) {
        uint32_t slot = qemu_get_be32(f), words[33] = { 0 };

        if (slot >= SGI_KONA_TEXTURE_CURVE_SLOTS ||
            g_hash_table_contains(tmp.filter_curves, GUINT_TO_POINTER(slot))) {
            goto invalid;
        }
        for (unsigned j = 0; j < 32; j++) {
            words[j] = qemu_get_be32(f);
        }
        if (qemu_file_get_error(f) ||
            !sgi_kona_texture_curve_upload(&tmp, slot, words)) {
            goto invalid;
        }
    }
    detail_valid = qemu_get_be32(f);
    for (unsigned k = 0; k < ARRAY_SIZE(tmp.detail_args); k++) {
        tmp.detail_args[k] = qemu_get_be32(f);
    }
    if (detail_valid > 1 || (detail_valid &&
        (tmp.detail_args[0] == UINT32_MAX ||
         !texture_detail_args(tmp.detail_args)))) {
        goto invalid;
    }
    tmp.detail_bound = detail_valid;
    tmp.owner = owner;
    if (qemu_file_get_error(f) ||
        (bound && !texture_object(&tmp, handle, false))) {
        goto invalid;
    }
    tmp.bound = bound;
    tmp.bound_handle = handle;
    sgi_kona_texture_cleanup(s);
    *s = tmp;
    return true;

invalid:
    sgi_kona_texture_cleanup(&tmp);
    error_setg(errp,
               "KONA texture state has invalid images, sampler or budget");
    return false;
}

const VMStateInfo vmstate_info_sgi_kona_texture = {
    .name = "sgi-kona-texture",
    .load = texture_load,
    .save = texture_save,
};

bool sgi_kona_texture_curve_upload(SGIKonaTextureState *s, uint32_t slot,
                                  const uint32_t words[33])
{
    SGIKonaTextureCurve next = { .slot = slot }, *curve;

    assert(words);
    if (slot >= SGI_KONA_TEXTURE_CURVE_SLOTS) {
        return false;
    }
    next.spline = sgi_kona_gl_texture_filter_curve(words, next.values);
    memcpy(next.words, words, sizeof(next.words));
    curve = g_hash_table_lookup(s->filter_curves, GUINT_TO_POINTER(slot));
    if (!curve) {
        if (TEXTURE_CURVE_BYTES > s->budget - s->bytes) {
            return false;
        }
        curve = g_try_new(SGIKonaTextureCurve, 1);
        if (!curve) {
            return false;
        }
        g_hash_table_insert(s->filter_curves, GUINT_TO_POINTER(slot), curve);
        s->bytes += TEXTURE_CURVE_BYTES;
    }
    *curve = next;
    return true;
}

const double *sgi_kona_texture_curve(SGIKonaTextureState *s, uint32_t slot)
{
    const SGIKonaTextureCurve *curve =
        g_hash_table_lookup(s->filter_curves, GUINT_TO_POINTER(slot));

    return curve && curve->spline ? curve->values : NULL;
}
