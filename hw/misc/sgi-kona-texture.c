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
    s->objects = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->filter_curves = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                             NULL, g_free);
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

static SGIKonaTextureObject *texture_object(SGIKonaTextureState *s,
                                            uint32_t handle, bool create)
{
    SGIKonaTextureObject *o;

    if (handle == UINT32_MAX) {
        return NULL;
    }
    o = g_hash_table_lookup(s->objects, GUINT_TO_POINTER(handle));
    if (!o && create &&
        TEXTURE_OBJECT_BYTES <= SGI_KONA_TEXTURE_BYTES - s->bytes) {
        o = g_try_new0(SGIKonaTextureObject, 1);
        if (o) {
            o->handle = handle;
            sgi_kona_texture_defaults(&o->params);
            g_hash_table_insert(s->objects, GUINT_TO_POINTER(handle), o);
            s->bytes += TEXTURE_OBJECT_BYTES;
        }
    }
    return o;
}

static bool texture_format(uint32_t base, uint32_t internal)
{
    switch (base) {
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
        SGI_KONA_TEXTURE_BYTES - (s->bytes - old)) {
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

static bool texture_native_key(const uint32_t a[20], const uint32_t b[20])
{
    /* DMA byte layout is normalized before these image fragments arrive. */
    return a[0] == b[0] && a[9] == b[9] &&
           a[16] == b[16] && (a[17] & ~4u) == (b[17] & ~4u) &&
           a[18] == b[18] && a[19] == b[19];
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
    uint8_t *coverage;
    uint64_t old = 0, bytes, count, covered = 0;

    if ((!volume && h[0] != SGI_GL_TEXTURE_2D) ||
        !rgba || h[10] == UINT32_MAX ||
        (h[17] & 0x60) != (volume ? 0x40 : 0) ||
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
        (o && o->native_width &&
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
        SGI_KONA_TEXTURE_BYTES - (s->bytes - old)) {
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
         sgi_kona_gl_texture_internal_format(args[3], &base, &internal)));
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
        p->min_lod > p->max_lod) {
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

void sgi_kona_texture_delete(SGIKonaTextureState *s, uint32_t handle)
{
    SGIKonaTextureObject *o = texture_object(s, handle, false);

    if (o) {
        for (unsigned k = 0; k < o->count; k++) {
            s->bytes -= level_bytes(&o->levels[k]);
        }
        for (unsigned k = 0; k < o->native_count; k++) {
            s->bytes -= native_bytes(&o->native[k]);
        }
        s->bytes -= TEXTURE_OBJECT_BYTES;
        g_hash_table_remove(s->objects, GUINT_TO_POINTER(handle));
        texture_object_free(o);
    }
    if (s->bound && s->bound_handle == handle) {
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
    *rgba = l->pixels[(size_t)y * l->width + x];
    if (l->base_format == SGI_GL_RGB) {
        *rgba |= 0xff;
    }
    return true;
}

static bool texture_native_read_3d(void *opaque, uint32_t level, uint32_t x,
                                   uint32_t y, uint32_t z, uint32_t *rgba)
{
    const SGIKonaTextureLevel *l = opaque;

    if (x >= l->width || y >= l->height || z >= l->depth || !l->pixels) {
        return false;
    }
    *rgba = l->pixels[((size_t)z * l->height + y) * l->width + x];
    if (l->base_format == SGI_GL_RGB) {
        *rgba |= 0xff;
    }
    return true;
}

static uint32_t texture_native_view(SGIKonaTextureObject *o)
{
    unsigned count = 0;
    bool volume = o->native_depth != 0;
    unsigned limit = volume ? 10 : SGI_KONA_TEXTURE_TABLE_LEVELS;

    for (unsigned level = 0; level < limit; level++) {
        SGIKonaTextureLevel *image = NULL;
        uint32_t descriptor = o->native_table[level];
        bool ambiguous = false;

        for (unsigned k = 0; k < o->native_count; k++) {
            SGIKonaTextureNativeImage *n = &o->native[k];
            const uint32_t *h = n->header;
            bool matches;

            if (volume) {
                matches = h[0] == SGI_GL_TEXTURE_3D &&
                    ((h[17] >> 3) & 1) == (level & 1) &&
                    h[18] == o->native_table[level & ~1u] &&
                    h[19] == o->native_table[(level & ~1u) + 1] &&
                    n->image.depth == MAX(1u, o->native_depth >> level);
            } else {
                matches = h[0] == SGI_GL_TEXTURE_2D && (level & 1 ?
                    (!h[18] && h[19] == descriptor) :
                    (!h[19] && h[18] == descriptor));
            }

            matches = matches && n->covered == native_pixels(n) &&
                      texture_dimensions(n->image.width, n->image.height) &&
                      (!o->native_format_valid || h[9] == o->native_format);
            if (matches && o->native_width) {
                matches = n->image.width == MAX(1u, o->native_width >> level) &&
                          n->image.height == MAX(1u, o->native_height >> level);
            }
            if (matches) {
                if (image) {
                    ambiguous = true;
                    break;
                }
                image = &n->image;
            }
        }
        if (ambiguous) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-kona: ambiguous native texture level %u "
                          "for handle 0x%x\n", level, o->handle);
            if (volume) {
                return UINT32_MAX;
            }
        } else if (image) {
            SGIKonaTextureLevel *view = &o->native_view[count];

            *view = *image;
            if (o->native_internal_valid) {
                view->base_format = o->native_base_format;
                view->internal_format = o->native_internal_format;
            }
            o->view[count++] = (SGIGLTexLevel) {
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
    if (!sgi_kona_gl_texture_internal_format(args[3], &base, &internal)) {
        return false;
    }
    storage = base == SGI_GL_ALPHA ? 0xe : base == SGI_GL_LUMINANCE ? 0xd :
              base == SGI_GL_LUMINANCE_ALPHA ? 8 : 0x30;
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
        .pyramid = &o->pyramid,
        .lod_scale_s = o->params.lod_scale_s,
        .lod_scale_t = o->params.lod_scale_t,
        .lod_scale_r = o->params.lod_scale_r,
        .post_filter = texture_post_filter, .opaque = o,
        .read_filter_weight = texture_filter_weight,
        .detail_level = o->params.detail_level,
        .detail_mode = o->params.detail_mode,
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

static bool texture_params_load(QEMUFile *f, SGIKonaTextureParams *p,
                                uint32_t version)
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
    p->lod_scale_s = p->lod_scale_t = 1;
    if (version >= 4) {
        wide.u = qemu_get_be64(f);
        p->lod_scale_s = wide.d;
        wide.u = qemu_get_be64(f);
        p->lod_scale_t = wide.d;
    }
    return !qemu_file_get_error(f) && texture_params(p);
}

static bool texture_save(QEMUFile *f, void *pv, size_t size,
                          const VMStateField *field, JSONWriter *vmdesc,
                          Error **errp)
{
    SGIKonaTextureState *s = pv;
    GHashTableIter iter;
    gpointer value;

    qemu_put_be32(f, 9);
    qemu_put_be32(f, g_hash_table_size(s->objects));
    qemu_put_be32(f, s->bound);
    qemu_put_be32(f, s->bound_handle);
    g_hash_table_iter_init(&iter, s->objects);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        SGIKonaTextureObject *o = value;

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

    sgi_kona_texture_init(&tmp);
    if ((version < 1 || version > 9) || bound > 1 ||
        count > SGI_KONA_TEXTURE_BYTES / TEXTURE_OBJECT_BYTES) {
        goto invalid;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = qemu_get_be32(f), levels = qemu_get_be32(f);
        SGIKonaTextureObject *o;

        if (levels > SGI_GL_TEX_LEVELS_MAX || id == UINT32_MAX ||
            texture_object(&tmp, id, false)) {
            goto invalid;
        }
        o = texture_object(&tmp, id, true);
        if (!o || !texture_params_load(f, &o->params, version)) {
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
            if (bytes > SGI_KONA_TEXTURE_BYTES - tmp.bytes ||
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
        if (version >= 2) {
            uint32_t valid = qemu_get_be32(f), native_count;

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
                uint64_t bytes;
                bool volume;

                for (unsigned j = 0; j < ARRAY_SIZE(n->header); j++) {
                    h[j] = qemu_get_be32(f);
                }
                l->width = qemu_get_be32(f);
                l->height = qemu_get_be32(f);
                l->base_format = qemu_get_be32(f);
                l->internal_format = qemu_get_be32(f);
                l->depth = version >= 9 ? qemu_get_be32(f) : 1;
                volume = h[0] == SGI_GL_TEXTURE_3D;

                if (!native_volume_dimensions(l->width, l->height, l->depth) ||
                    (!volume && l->depth != 1) ||
                    (volume && (version < 9 || h[15] >= l->depth)) ||
                    !(version >= 5 ? native_dimensions(l->width, l->height) :
                      texture_dimensions(l->width, l->height)) ||
                    !texture_format(l->base_format, l->internal_format) ||
                    (!volume && h[0] != SGI_GL_TEXTURE_2D) || h[10] != id ||
                    (h[17] & 0x60) != (volume ? 0x40 : 0) ||
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
                if (bytes > SGI_KONA_TEXTURE_BYTES - tmp.bytes ||
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
                if (version >= 5) {
                    uint64_t pixels_count = native_pixels(n);

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
                } else {
                    n->covered = native_pixels(n);
                    for (uint64_t p = 0; p < n->covered; p++) {
                        native_cover(n->coverage, p);
                    }
                }
            }
        }
        if (version >= 3) {
            o->native_width = qemu_get_be32(f);
            o->native_height = qemu_get_be32(f);
            if ((o->native_width || o->native_height) &&
                !texture_dimensions(o->native_width, o->native_height)) {
                goto invalid;
            }
        }
        if (version >= 5) {
            uint32_t valid = qemu_get_be32(f);

            o->native_format = qemu_get_be32(f);
            if (valid > 1 || o->native_format > 0x7f) {
                goto invalid;
            }
            o->native_format_valid = valid;
        }
        if (version >= 7) {
            o->params.filter_curve_slot = qemu_get_be32(f);
            if (o->params.filter_curve_slot >= SGI_KONA_TEXTURE_CURVE_SLOTS) {
                goto invalid;
            }
        }
        if (version >= 8) {
            uint32_t valid = qemu_get_be32(f);

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
        }
        if (version >= 9) {
            union { double d; uint64_t u; } scale_r;

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
        }
    }
    if (version >= 6) {
        uint32_t curves = qemu_get_be32(f);

        if (curves > SGI_KONA_TEXTURE_CURVE_SLOTS) {
            goto invalid;
        }
        for (unsigned i = 0; i < curves; i++) {
            uint32_t slot = qemu_get_be32(f), words[33] = { 0 };

            if (slot >= SGI_KONA_TEXTURE_CURVE_SLOTS ||
                sgi_kona_texture_curve(&tmp, slot)) {
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
    }
    if (version >= 8) {
        uint32_t valid = qemu_get_be32(f);

        for (unsigned k = 0; k < ARRAY_SIZE(tmp.detail_args); k++) {
            tmp.detail_args[k] = qemu_get_be32(f);
        }
        if (valid > 1 || (valid &&
            (tmp.detail_args[0] == UINT32_MAX ||
             !texture_detail_args(tmp.detail_args)))) {
            goto invalid;
        }
        tmp.detail_bound = valid;
    }
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
    SGIKonaTextureCurve next, *curve;

    if (slot >= SGI_KONA_TEXTURE_CURVE_SLOTS ||
        !sgi_kona_gl_texture_filter_curve(words, next.values)) {
        return false;
    }
    next.slot = slot;
    memcpy(next.words, words, sizeof(next.words));
    curve = g_hash_table_lookup(s->filter_curves, GUINT_TO_POINTER(slot));
    if (!curve) {
        if (TEXTURE_CURVE_BYTES > SGI_KONA_TEXTURE_BYTES - s->bytes) {
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

    return curve ? curve->values : NULL;
}
