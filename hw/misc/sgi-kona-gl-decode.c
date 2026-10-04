/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * InfiniteReality (KONA) OpenGL doorbell command decoder.
 *
 * The doorbell takes selector-first packets. The table lists the decoded
 * subset and marks each layout established, inferred, or unresolved. The
 * caller splits 64-bit stores into big-endian words before decoding.
 *
 * No doorbell kick or length register is modeled. The declared length is
 * available for transports that provide one.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "hw/misc/sgi-kona-gl-decode.h"

#include "sgi-kona-gl-selectors.c.inc"

const SGIKonaGLSelectorInfo *sgi_kona_gl_selector_table(size_t *count)
{
    *count = ARRAY_SIZE(sgi_kona_gl_selectors);
    return sgi_kona_gl_selectors;
}

const SGIKonaGLSelectorInfo *sgi_kona_gl_lookup(uint32_t selector)
{
    for (size_t i = 0; i < ARRAY_SIZE(sgi_kona_gl_selectors); i++) {
        if (sgi_kona_gl_selectors[i].selector == selector) {
            return &sgi_kona_gl_selectors[i];
        }
    }
    return NULL;
}

static bool arg0_expected(const SGIKonaGLSelectorInfo *info, uint32_t value)
{
    if (!info->num_arg0_values) {
        return true;
    }
    for (unsigned i = 0; i < info->num_arg0_values; i++) {
        if (info->arg0_values[i] == value) {
            return true;
        }
    }
    return false;
}

SGIKonaGLDecodeResult sgi_kona_gl_decode(const uint32_t *words, size_t nwords,
                                         size_t declared_len,
                                         SGIKonaGLCommand *cmd)
{
    const SGIKonaGLSelectorInfo *info;
    unsigned length;

    memset(cmd, 0, sizeof(*cmd));
    if (!nwords) {
        cmd->length = 1;
        return SGI_KONA_GL_DECODE_TRUNCATED;
    }

    cmd->selector = words[0];
    cmd->length = 1;
    info = sgi_kona_gl_lookup(words[0]);
    cmd->info = info;
    if (!info) {
        return SGI_KONA_GL_DECODE_UNKNOWN_SELECTOR;
    }
    if (info->evidence == SGI_KONA_GL_LAYOUT_UNRESOLVED) {
        return SGI_KONA_GL_DECODE_UNRESOLVED_LAYOUT;
    }

    assert(info->argc <= SGI_KONA_GL_MAX_ARGS);
    length = 1 + info->argc;
    cmd->length = length;
    cmd->argc = info->argc;

    if (declared_len != SGI_KONA_GL_LENGTH_UNDECLARED &&
        declared_len != length) {
        return SGI_KONA_GL_DECODE_LENGTH_MISMATCH;
    }
    if (nwords < length) {
        return SGI_KONA_GL_DECODE_TRUNCATED;
    }

    memcpy(cmd->args, &words[1], info->argc * sizeof(uint32_t));
    if (info->argc && !arg0_expected(info, cmd->args[0])) {
        return SGI_KONA_GL_DECODE_UNEXPECTED_ARGUMENT;
    }
    return SGI_KONA_GL_DECODE_OK;
}

const char *sgi_kona_gl_decode_result_name(SGIKonaGLDecodeResult result)
{
    static const char *const names[] = {
        [SGI_KONA_GL_DECODE_OK] = "ok",
        [SGI_KONA_GL_DECODE_UNKNOWN_SELECTOR] = "unknown selector",
        [SGI_KONA_GL_DECODE_UNRESOLVED_LAYOUT] = "unresolved layout",
        [SGI_KONA_GL_DECODE_TRUNCATED] = "truncated",
        [SGI_KONA_GL_DECODE_LENGTH_MISMATCH] = "length mismatch",
        [SGI_KONA_GL_DECODE_UNEXPECTED_ARGUMENT] = "unexpected argument",
    };

    if (result >= ARRAY_SIZE(names) || !names[result]) {
        return "invalid";
    }
    return names[result];
}

/* Cleared native query descriptors; state support is checked separately. */
static const SGIKonaGLQueryInfo sgi_kona_gl_queries[] = {
    { 0x0b10, 0x04, 1 },
    { 0x0b11, 0x03, 1 },
    { 0x0b14, 0x04, 6 },
    { 0x0b20, 0x04, 1 },
    { 0x0b21, 0x03, 1 },
    { 0x0b24, 0x04, 1 },
    { 0x0b25, 0x04, 1 },
    { 0x0b26, 0x04, 1 },
    { 0x0b40, 0x04, 2 },
    { 0x0b41, 0x04, 1 },
    { 0x0b42, 0x04, 1 },
    { 0x0b44, 0x04, 1 },
    { 0x0b45, 0x04, 1 },
    { 0x0b46, 0x04, 1 },
    { 0x0b50, 0x04, 1 },
    { 0x0b51, 0x04, 1 },
    { 0x0b52, 0x04, 1 },
    { 0x0b53, 0x13, 4 },
    { 0x0b54, 0x04, 1 },
    { 0x0b55, 0x04, 1 },
    { 0x0b56, 0x04, 1 },
    { 0x0b57, 0x04, 1 },
    { 0x0b60, 0x04, 1 },
    { 0x0b61, 0x03, 1 },
    { 0x0b62, 0x03, 1 },
    { 0x0b63, 0x03, 1 },
    { 0x0b64, 0x03, 1 },
    { 0x0b65, 0x04, 1 },
    { 0x0b66, 0x13, 4 },
    { 0x0b70, 0x12, 2 },
    { 0x0b71, 0x04, 1 },
    { 0x0b72, 0x04, 1 },
    { 0x0b73, 0x12, 1 },
    { 0x0b74, 0x04, 1 },
    { 0x0b80, 0x13, 4 },
    { 0x0b90, 0x04, 1 },
    { 0x0b91, 0x04, 1 },
    { 0x0b92, 0x04, 1 },
    { 0x0b93, 0x04, 1 },
    { 0x0b94, 0x04, 1 },
    { 0x0b95, 0x04, 1 },
    { 0x0b96, 0x04, 1 },
    { 0x0b97, 0x04, 1 },
    { 0x0b98, 0x04, 1 },
    { 0x0ba0, 0x04, 1 },
    { 0x0ba1, 0x04, 1 },
    { 0x0ba2, 0x04, 4 },
    { 0x0ba3, 0x04, 1 },
    { 0x0ba4, 0x04, 1 },
    { 0x0ba5, 0x04, 1 },
    { 0x0ba6, 0x03, 16 },
    { 0x0ba7, 0x03, 16 },
    { 0x0ba8, 0x03, 16 },
    { 0x0bb0, 0x04, 1 },
    { 0x0bc0, 0x04, 1 },
    { 0x0bc1, 0x04, 1 },
    { 0x0bc2, 0x13, 1 },
    { 0x0bd0, 0x04, 1 },
    { 0x0be0, 0x04, 1 },
    { 0x0be1, 0x04, 1 },
    { 0x0be2, 0x04, 1 },
    { 0x0bf0, 0x04, 1 },
    { 0x0bf1, 0x04, 1 },
    { 0x0bf2, 0x04, 1 },
    { 0x0c01, 0x04, 1 },
    { 0x0c02, 0x04, 1 },
    { 0x0c10, 0x04, 4 },
    { 0x0c11, 0x04, 1 },
    { 0x0c20, 0x03, 1 },
    { 0x0c21, 0x04, 1 },
    { 0x0c22, 0x13, 4 },
    { 0x0c23, 0x04, 4 },
    { 0x0c50, 0x04, 1 },
    { 0x0c51, 0x04, 1 },
    { 0x0c52, 0x04, 1 },
    { 0x0c53, 0x04, 1 },
    { 0x0c54, 0x04, 1 },
    { 0x0c60, 0x04, 1 },
    { 0x0c61, 0x04, 1 },
    { 0x0c62, 0x04, 1 },
    { 0x0c63, 0x04, 1 },
    { 0x0cb0, 0x04, 1 },
    { 0x0cb1, 0x04, 1 },
    { 0x0cb2, 0x04, 1 },
    { 0x0cb3, 0x04, 1 },
    { 0x0cb4, 0x04, 1 },
    { 0x0cb5, 0x04, 1 },
    { 0x0cb6, 0x04, 1 },
    { 0x0cb7, 0x04, 1 },
    { 0x0cb8, 0x04, 1 },
    { 0x0cb9, 0x04, 1 },
    { 0x0d70, 0x04, 1 },
    { 0x2a00, 0x04, 1 },
    { 0x2a01, 0x04, 1 },
    { 0x2a02, 0x04, 1 },
    { 0x3000, 0x04, 1 },
    { 0x3001, 0x04, 1 },
    { 0x3002, 0x04, 1 },
    { 0x3003, 0x04, 1 },
    { 0x3004, 0x04, 1 },
    { 0x3005, 0x04, 1 },
    { 0x4000, 0x04, 1 },
    { 0x4001, 0x04, 1 },
    { 0x4002, 0x04, 1 },
    { 0x4003, 0x04, 1 },
    { 0x4004, 0x04, 1 },
    { 0x4005, 0x04, 1 },
    { 0x4006, 0x04, 1 },
    { 0x4007, 0x04, 1 },
    { 0x8005, 0x13, 4 },
    { 0x8009, 0x04, 1 },
    { 0x8010, 0x04, 1 },
    { 0x8011, 0x04, 1 },
    { 0x8012, 0x04, 1 },
    { 0x8024, 0x04, 1 },
    { 0x802e, 0x04, 1 },
    { 0x8037, 0x04, 1 },
    { 0x8038, 0x03, 1 },
    { 0x8039, 0x03, 1 },
    { 0x8094, 0x04, 1 },
    { 0x809d, 0x04, 1 },
    { 0x809e, 0x04, 1 },
    { 0x809f, 0x04, 1 },
    { 0x80a0, 0x04, 1 },
    { 0x80aa, 0x03, 1 },
    { 0x80ab, 0x04, 1 },
    { 0x80ac, 0x04, 1 },
    { 0x80b1, 0x03, 16 },
    { 0x80b2, 0x04, 1 },
    { 0x80b4, 0x03, 1 },
    { 0x80b5, 0x03, 1 },
    { 0x80b6, 0x03, 1 },
    { 0x80b7, 0x03, 1 },
    { 0x80b8, 0x03, 1 },
    { 0x80b9, 0x03, 1 },
    { 0x80ba, 0x03, 1 },
    { 0x80bb, 0x03, 1 },
    { 0x80bc, 0x04, 1 },
    { 0x80d0, 0x04, 1 },
    { 0x80d1, 0x04, 1 },
    { 0x80d2, 0x04, 1 },
    { 0x8126, 0x03, 1 },
    { 0x8127, 0x03, 1 },
    { 0x8128, 0x03, 1 },
    { 0x8129, 0x03, 3 },
    { 0x812b, 0x04, 1 },
    { 0x8148, 0x00, 1 },
    { 0x8149, 0x04, 1 },
    { 0x814a, 0x03, 3 },
    { 0x814b, 0x03, 3 },
    { 0x817d, 0x04, 1 },
    { 0x817e, 0x03, 4 },
    { 0x8181, 0x04, 1 },
    { 0x8183, 0x04, 1 },
    { 0x8196, 0x04, 1 },
    { 0x8198, 0x04, 1 },
    { 0x8199, 0x03, 4 },
    { 0x81c4, 0x04, 1 },
    { 0x81c7, 0x04, 1 },
};

const SGIKonaGLQueryInfo *sgi_kona_gl_query_lookup(uint32_t pname)
{
    for (unsigned i = 0; i < ARRAY_SIZE(sgi_kona_gl_queries); i++) {
        if (sgi_kona_gl_queries[i].pname == pname) {
            return &sgi_kona_gl_queries[i];
        }
    }
    return NULL;
}

const SGIKonaGLQueryInfo *sgi_kona_gl_query_table(size_t *count)
{
    *count = ARRAY_SIZE(sgi_kona_gl_queries);
    return sgi_kona_gl_queries;
}

bool sgi_kona_gl_query_serialize(const SGIKonaGLQueryInfo *info,
                                 const uint64_t *values, unsigned count,
                                 uint8_t *out, size_t capacity, size_t *length)
{
    unsigned width;
    size_t bytes;

    *length = 0;
    if (!info || count != info->count) {
        return false;
    }
    switch (info->type) {
    case 0x00:
        width = 1;
        break;
    case 0x03:
    case 0x04:
    case 0x13:
        width = 4;
        break;
    case 0x12:
        width = 8;
        break;
    default:
        return false;
    }
    bytes = (size_t)count * width;
    if (bytes > capacity) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        switch (width) {
        case 1:
            out[i] = values[i];
            break;
        case 4:
            stl_be_p(out + 4 * i, values[i]);
            break;
        case 8:
            stq_be_p(out + 8 * i, values[i]);
            break;
        }
    }
    *length = bytes;
    return true;
}

/* RGBA channels occupy the bytes from most to least significant. */
bool sgi_kona_gl_texture_transform(uint32_t flags, uint32_t scale,
                                   uint32_t bias, double scale_rgba[4],
                                   double bias_rgba[4])
{
    if (flags & ~0xfu) {
        return false;
    }
    for (unsigned k = 0; k < 4; k++) {
        unsigned shift = 24 - 8 * k;
        bool enabled = flags & (8u >> k);

        scale_rgba[k] = enabled ? ((scale >> shift) & 0xff) / 255.0 : 1.0;
        bias_rgba[k] = enabled ? ((bias >> shift) & 0xff) / 255.0 : 0.0;
    }
    return true;
}

void sgi_kona_gl_texture_dimensions(const uint32_t bind[5], uint32_t *width,
                                    uint32_t *height)
{
    *width = 1u << (bind[3] & 15);
    *height = 1u << ((bind[3] >> 4) & 15);
}

/* Decode the effective bound, rather than reconstructing the caller's LOD. */
static double sgi_kona_gl_texture_lod(unsigned code)
{
    if (code >= 64) {
        return code / 64.0 - 1;
    }
    /* The negative producer rounds a footprint ratio scaled by 64. */
    return code ? log2(code / 64.0) : -1000;
}

/* Native min selectors group spatial and mipmap interpolation separately. */
bool sgi_kona_gl_texture_sampler(const uint32_t bind[5],
                                 const uint32_t record[22],
                                 SGIKonaTextureParams *params)
{
    static const uint32_t min_filter[8] = {
        SGI_GL_NEAREST, SGI_GL_LINEAR, 0,
        SGI_GL_NEAREST_MIPMAP_NEAREST, SGI_GL_NEAREST_MIPMAP_LINEAR,
        SGI_GL_LINEAR_MIPMAP_NEAREST, SGI_GL_LINEAR_MIPMAP_LINEAR, 0,
    };
    SGIKonaTextureParams next = *params;
    unsigned min = (bind[2] >> 2) & 7;
    unsigned mag = (bind[2] >> 5) & 7;
    unsigned lo = record[2] & 0x3ff;
    unsigned hi = (record[2] >> 10) & 0x3ff;
    unsigned scale_s = record[1] & 0xff;
    unsigned scale_t = (record[1] >> 8) & 0xff;
    unsigned compare = (bind[2] >> 10) & 3;

    /*
     * Non-repeat S/T collapse three public clamp modes into the same bits.
     * Their border storage is not established by this sampler record.
     */
    if (params->base_level || !min_filter[min] ||
        (record[2] & 0xfff00000) || lo > hi ||
        (mag > 1 && (mag < 3 || mag > 5)) ||
        (bind[3] & 0x7000) ||
        !scale_s || !scale_t) {
        return false;
    }
    next.shadow_compare = 0;
    next.shadow_ambient = 0;
    if (compare) {
        unsigned target = bind[1] >> 28;

        /* The R exponent is a reference marker, not a volume dimension. */
        if (compare == 3 || bind[4] || (target != 1 && target != 2) ||
            ((bind[1] >> 12) & 0xfff) != 63 ||
            ((bind[2] >> 14) & 0x7f) != 0xd ||
            (bind[3] & 0xf00) != 0xf00 || mag > 1 ||
            record[4] || record[5] > 63) {
            return false;
        }
        next.shadow_compare = compare == 1 ? SGI_GL_TEXTURE_GEQUAL_R_SGIX :
                                            SGI_GL_TEXTURE_LEQUAL_R_SGIX;
        next.shadow_ambient = record[5] / 63.0;
    } else if (bind[4] != 1) {
        return false;
    }
    next.min_filter = min_filter[min];
    next.mag_filter = mag ? SGI_GL_LINEAR : SGI_GL_NEAREST;
    next.filter_curve_slot = 0;
    next.detail_level = 0;
    next.detail_mode = SGI_GL_MODULATE;
    if (mag >= 3) {
        switch (bind[2] & 0x0c000000) {
        case 0:
            next.mag_filter = mag == 5 ? SGI_GL_LINEAR_SHARPEN_SGIS :
                                        SGI_GL_LINEAR_DETAIL_SGIS;
            break;
        case 0x04000000:
            next.mag_filter = mag == 5 ? SGI_GL_LINEAR_SHARPEN_COLOR_SGIS :
                                        SGI_GL_LINEAR_DETAIL_COLOR_SGIS;
            break;
        case 0x08000000:
            next.mag_filter = mag == 5 ? SGI_GL_LINEAR_SHARPEN_ALPHA_SGIS :
                                        SGI_GL_LINEAR_DETAIL_ALPHA_SGIS;
            break;
        default:
            return false;
        }
        next.filter_curve_slot = bind[1] & 0xfff;
    }
    if (mag == 3 || mag == 4) {
        unsigned detail_level = (bind[2] >> 21) & 7;

        if (detail_level > 4) {
            return false;
        }
        next.detail_level = -(int)detail_level;
        next.detail_mode = mag == 3 ? SGI_GL_ADD : SGI_GL_MODULATE;
    }
    next.wrap_s = next.wrap_t = SGI_GL_REPEAT;
    next.lod_scale_s = scale_s / 16.0;
    next.lod_scale_t = scale_t / 16.0;
    next.min_lod = sgi_kona_gl_texture_lod(lo);
    next.max_lod = sgi_kona_gl_texture_lod(hi);
    *params = next;
    return true;
}

bool sgi_kona_gl_texture_native_layout(const uint32_t h[20])
{
    bool volume = h[0] == SGI_GL_TEXTURE_3D;
    uint32_t layout;

    if ((!volume && h[0] != SGI_GL_TEXTURE_1D &&
         h[0] != SGI_GL_TEXTURE_2D) || h[6] > 1) {
        return false;
    }
    switch (h[9]) {
    case 0xd:
    case 0xe:
    case 8:
        layout = volume ? 0x11c0 : 0x2200;
        break;
    case 0x30:
        layout = volume ? 0x1141 : 0x2181;
        break;
    default:
        return false;
    }
    /* Bit three selects a bank in ordinary mode and level parity in volume. */
    layout |= h[6] << 2;
    return (h[17] & ~8u) == layout;
}

bool sgi_kona_gl_texture_native_binding_layout(uint32_t base, uint32_t target,
                                               uint32_t record)
{
    bool volume = target == SGI_GL_TEXTURE_3D;
    uint32_t expected;

    if (!volume && target != SGI_GL_TEXTURE_1D && target != SGI_GL_TEXTURE_2D) {
        return false;
    }
    switch (base) {
    case SGI_GL_DEPTH_COMPONENT:
    case SGI_GL_LUMINANCE:
    case SGI_GL_ALPHA:
    case SGI_GL_LUMINANCE_ALPHA:
        expected = volume ? 0x23 : 0x44;
        break;
    case SGI_GL_RGB:
    case SGI_GL_RGBA:
        expected = volume ? 0x22 : 0x43;
        break;
    default:
        return false;
    }
    return (record & 0xff) == expected;
}

bool sgi_kona_gl_texture_sampler_1d(const uint32_t bind[5],
                                    const uint32_t record[22],
                                    SGIKonaTextureParams *params)
{
    SGIKonaTextureParams next = *params;
    uint32_t ordinary_bind[5], ordinary_record[22];

    /* Only the base-zero, repeat interior profile is established. */
    if ((bind[1] >> 28) != 1 || (bind[3] & 0xf0) ||
        (bind[3] & 0xf00) != ((bind[2] & 0xc00) ? 0xf00 : 0) ||
        params->base_level || ((bind[2] >> 5) & 7) > 1) {
        return false;
    }
    memcpy(ordinary_bind, bind, sizeof(ordinary_bind));
    memcpy(ordinary_record, record, sizeof(ordinary_record));
    /* A 1D sampler has no T/R footprint or wrapping decision. */
    ordinary_bind[3] &= ~0x6000u;
    ordinary_record[1] = (ordinary_record[1] & ~0xff00u) | 0x1000;
    if (!sgi_kona_gl_texture_sampler(ordinary_bind, ordinary_record, &next)) {
        return false;
    }
    next.lod_scale_t = next.lod_scale_r = 1;
    *params = next;
    return true;
}

void sgi_kona_gl_texture_volume_dimensions(const uint32_t bind[5],
                                            uint32_t *width, uint32_t *height,
                                            uint32_t *depth)
{
    sgi_kona_gl_texture_dimensions(bind, width, height);
    *depth = 1u << ((bind[3] >> 8) & 15);
}

bool sgi_kona_gl_texture_volume_sampler(const uint32_t bind[5],
                                         const uint32_t *record, bool mipmapped,
                                         SGIKonaTextureParams *params)
{
    SGIKonaTextureParams next = *params;
    uint32_t ordinary[22] = { 0, 0x00101010 };
    unsigned min = (bind[2] >> 2) & 7, mag = (bind[2] >> 5) & 7;

    if ((bind[1] >> 28) != 3 || params->base_level || mag > 1 ||
        (mipmapped ? min < 3 || min > 6 : min > 1 || min != mag)) {
        return false;
    }
    if (mipmapped) {
        memcpy(ordinary, record, 4 * sizeof(uint32_t));
        next.lod_scale_r = ((record[1] >> 16) & 255) / 16.0;
        if (next.lod_scale_r == 0) {
            return false;
        }
    } else {
        /* The short record omits LOD state; equal filters do not need it. */
        next.lod_scale_r = 1;
    }
    next.wrap_r = SGI_GL_REPEAT;
    if (!sgi_kona_gl_texture_sampler(bind, ordinary, &next)) {
        return false;
    }
    *params = next;
    return true;
}

bool sgi_kona_gl_texture_internal_format(uint32_t index, uint32_t *base,
                                         uint32_t *internal)
{
    static const struct {
        uint32_t base, internal;
    } formats[] = {
        [0] = { SGI_GL_LUMINANCE, SGI_GL_LUMINANCE },
        [1] = { SGI_GL_LUMINANCE_ALPHA, SGI_GL_LUMINANCE_ALPHA },
        [2] = { SGI_GL_RGBA, SGI_GL_RGBA },
        [3] = { SGI_GL_LUMINANCE, 0x8042 },
        [6] = { SGI_GL_LUMINANCE, 0x803f },
        [7] = { SGI_GL_ALPHA, 0x803b },
        [9] = { SGI_GL_LUMINANCE_ALPHA, 0x8043 },
        [10] = { SGI_GL_LUMINANCE, 0x8040 },
        [11] = { SGI_GL_ALPHA, 0x803c },
        [13] = { SGI_GL_LUMINANCE_ALPHA, 0x8044 },
        [14] = { SGI_GL_LUMINANCE_ALPHA, 0x8045 },
        [15] = { SGI_GL_LUMINANCE, SGI_GL_LUMINANCE },
        [16] = { SGI_GL_LUMINANCE, 0x8041 },
        [17] = { SGI_GL_ALPHA, SGI_GL_ALPHA },
        [18] = { SGI_GL_ALPHA, 0x803d },
        [21] = { SGI_GL_LUMINANCE_ALPHA, SGI_GL_LUMINANCE_ALPHA },
        [23] = { SGI_GL_ALPHA, 0x803e },
        [33] = { SGI_GL_RGB, 0x8051 },
        [34] = { SGI_GL_RGBA, 0x8058 },
        [37] = { SGI_GL_RGB, SGI_GL_RGB },
        [38] = { SGI_GL_RGB, SGI_GL_RGB },
        [41] = { SGI_GL_RGBA, SGI_GL_RGBA },
        [63] = { SGI_GL_DEPTH_COMPONENT, SGI_GL_DEPTH_COMPONENT },
    };

    if (index >= ARRAY_SIZE(formats) || !formats[index].base) {
        return false;
    }
    *base = formats[index].base;
    *internal = formats[index].internal;
    return true;
}

typedef struct SGIKonaGLPackedPixel {
    uint8_t bytes;
    uint8_t bits[4];
} SGIKonaGLPackedPixel;

static const SGIKonaGLPackedPixel *sgi_kona_gl_packed_pixel(uint32_t format,
                                                          uint32_t type)
{
    static const SGIKonaGLPackedPixel types[] = {
        { 1, { 3, 3, 2, 0 } },
        { 2, { 4, 4, 4, 4 } },
        { 2, { 5, 5, 5, 1 } },
        { 4, { 8, 8, 8, 8 } },
        { 4, { 10, 10, 10, 2 } },
    };

    if (type < 8 || type - 8 >= ARRAY_SIZE(types) ||
        (type == 8 ? format != 4 : (format != 5 && format != 10))) {
        return NULL;
    }
    return &types[type - 8];
}

uint32_t sgi_kona_gl_texture_pixel_bytes(uint32_t format, uint32_t type)
{
    static const uint8_t widths[] = { 1, 1, 2, 2, 4, 4, 4 };
    uint32_t components;
    const SGIKonaGLPackedPixel *packed = sgi_kona_gl_packed_pixel(format, type);

    if (packed) {
        return packed->bytes;
    }
    switch (format) {
    case 0:                     /* RED */
    case 1:                     /* GREEN */
    case 2:                     /* BLUE */
    case 3:                     /* ALPHA */
    case 6:                     /* LUMINANCE */
        components = 1;
        break;
    case 4:                     /* RGB */
    case 9:                     /* BGR */
        components = 3;
        break;
    case 5:                     /* RGBA */
    case 8:                     /* ABGR */
    case 10:                    /* BGRA */
        components = 4;
        break;
    case 7:                     /* LUMINANCE_ALPHA */
        components = 2;
        break;
    default:
        return 0;
    }
    return type < ARRAY_SIZE(widths) ? components * widths[type] : 0;
}

uint32_t sgi_kona_gl_native_pixel_bytes(uint32_t format, uint32_t type)
{
    /* Index and depth formats have one scalar element. */
    return (format >= 11 && format <= 13) && type <= 6 ?
           sgi_kona_gl_texture_pixel_bytes(0, type) :
           sgi_kona_gl_texture_pixel_bytes(format, type);
}

bool sgi_kona_gl_pixel_index(uint32_t type, const uint32_t *source,
                              bool swap_bytes, double *index)
{
    uint32_t raw;
    double value;

    if (!source || !index || type > 6) {
        return false;
    }
    if (type < 2) {
        raw = source[0] >> 24;
        value = type == 0 ? (double)(int8_t)raw : raw;
    } else if (type < 4) {
        raw = source[0] >> 16;
        raw = swap_bytes ? bswap16(raw) : raw;
        value = type == 2 ? (double)(int16_t)raw : raw;
    } else {
        raw = swap_bytes ? bswap32(source[0]) : source[0];
        if (type == 6) {
            float scalar;

            if ((raw & 0x7f800000) == 0x7f800000) {
                return false;
            }
            memcpy(&scalar, &raw, sizeof(scalar));
            value = scalar;
        } else {
            value = type == 4 ? (int32_t)raw : (double)raw;
        }
    }
    *index = value;
    return true;
}

static bool sgi_kona_gl_pixel_index_fixed(double index, int32_t shift,
                                         int32_t offset, uint64_t *out)
{
    double whole, fraction;
    uint64_t fixed;
    uint32_t integer, fractional;

    if (!out || !isfinite(index)) {
        return false;
    }
    /* Model fixed-point precision: 32 integer and 32 fractional bits. */
    whole = floor(index);
    integer = (int64_t)fmod(whole, 4294967296.0);
    fraction = (index - whole) * 4294967296.0;
    fractional = MIN(fraction, (double)UINT32_MAX);
    fixed = ((uint64_t)integer << 32) | fractional;
    if (shift >= 64 || shift <= -64) {
        fixed = 0;
    } else if (shift > 0) {
        fixed <<= shift;
    } else if (shift < 0) {
        fixed >>= -shift;
    }
    fixed += (uint64_t)(uint32_t)offset << 32;
    *out = fixed;
    return true;
}

bool sgi_kona_gl_pixel_index_transfer(double index, int32_t shift,
                                       int32_t offset, uint32_t *out)
{
    uint64_t fixed;

    if (!out || !sgi_kona_gl_pixel_index_fixed(index, shift, offset, &fixed)) {
        return false;
    }
    *out = fixed >> 32;
    return true;
}

bool sgi_kona_gl_pixel_index_round(double index, int32_t shift,
                                    int32_t offset, uint32_t *out)
{
    uint64_t fixed;

    if (!out || !sgi_kona_gl_pixel_index_fixed(index, shift, offset, &fixed)) {
        return false;
    }
    *out = (fixed + UINT64_C(0x80000000)) >> 32;
    return true;
}

bool sgi_kona_gl_pixel_index_pack(uint32_t type, uint32_t index,
                                   bool swap_bytes, uint8_t *out,
                                   size_t capacity)
{
    static const uint32_t masks[] = {
        0x7f, 0xff, 0x7fff, 0xffff, 0x7fffffff, 0xffffffff,
    };
    uint32_t bytes = sgi_kona_gl_texture_pixel_bytes(0, type), bits;
    uint8_t packed[4];

    if (!out || type > 6 || !bytes || capacity < bytes) {
        return false;
    }
    if (type == 6) {
        float scalar = index;

        memcpy(&bits, &scalar, sizeof(bits));
    } else {
        bits = index & masks[type];
    }
    if (bytes == 1) {
        packed[0] = bits;
    } else if (bytes == 2) {
        stw_be_p(packed, swap_bytes ? bswap16(bits) : bits);
    } else {
        stl_be_p(packed, swap_bytes ? bswap32(bits) : bits);
    }
    memcpy(out, packed, bytes);
    return true;
}

static bool sgi_kona_gl_pixel_values(uint32_t format, uint32_t type,
                                    const uint32_t *source, bool swap_bytes,
                                    double values[4])
{
    uint32_t components = sgi_kona_gl_texture_pixel_bytes(format, 1);
    const SGIKonaGLPackedPixel *packed = sgi_kona_gl_packed_pixel(format, type);
    uint32_t bits = 0, shift = 0;
    double r, g, b, a;

    if (!sgi_kona_gl_texture_pixel_bytes(format, type)) {
        return false;
    }
    values[0] = values[1] = values[2] = 0;
    values[3] = 1;
    if (packed) {
        bits = source[0] >> (8 * (4 - packed->bytes));
        if (swap_bytes && packed->bytes == 2) {
            bits = bswap16(bits);
        } else if (swap_bytes && packed->bytes == 4) {
            bits = bswap32(bits);
        }
        shift = 8 * packed->bytes;
    }
    for (uint32_t i = 0; i < components; i++) {
        double component;
        uint32_t raw;

        switch (type) {
        case 0:
        case 1:
            raw = (source[0] >> (24 - i * 8)) & 255;
            component = type == 1 ? raw / 255.0 :
                        (2.0 * (int8_t)raw + 1) / 255.0;
            break;
        case 2:
        case 3:
            raw = (source[i / 2] >> (16 - (i % 2) * 16)) & 65535;
            if (swap_bytes) {
                raw = bswap16(raw);
            }
            component = type == 3 ? raw / 65535.0 :
                        (2.0 * (int16_t)raw + 1) / 65535.0;
            break;
        case 4:
        case 5:
            raw = swap_bytes ? bswap32(source[i]) : source[i];
            component = type == 5 ? raw / 4294967295.0 :
                        (2.0 * (int32_t)raw + 1) / 4294967295.0;
            break;
        case 6: {
            float value;

            raw = swap_bytes ? bswap32(source[i]) : source[i];
            /* Reject both NaNs and infinities before host FP arithmetic. */
            if ((raw & 0x7f800000) == 0x7f800000) {
                return false;
            }
            memcpy(&value, &raw, sizeof(value));
            component = value;
            break;
        }
        default: {
            uint32_t maximum = (1u << packed->bits[i]) - 1;

            shift -= packed->bits[i];
            component = ((bits >> shift) & maximum) / (double)maximum;
            break;
        }
        }
        values[i] = component;
    }
    r = values[0];
    g = values[1];
    b = values[2];
    a = values[3];
    switch (format) {
    case 1:
        g = r;
        r = 0;
        break;
    case 2:
        b = r;
        r = 0;
        break;
    case 3:
        a = r;
        r = g = b = 0;
        break;
    case 4:
        a = 1;
        break;
    case 6:
        g = b = r;
        a = 1;
        break;
    case 7:
        a = g;
        g = b = r;
        break;
    case 8:
        r = values[3];
        g = values[2];
        b = values[1];
        a = values[0];
        break;
    case 9:
    case 10:
        r = values[2];
        b = values[0];
        break;
    }
    values[0] = r;
    values[1] = g;
    values[2] = b;
    values[3] = a;
    return true;
}

bool sgi_kona_gl_texture_pixel_transfer(uint32_t format, uint32_t type,
                                        const uint32_t *source, bool swap_bytes,
                                        const float scale[4],
                                        const float bias[4], uint32_t *rgba)
{
    double values[4];
    uint32_t pixel = 0;

    if (!source || !scale || !bias || !rgba ||
        !sgi_kona_gl_pixel_values(format, type, source, swap_bytes, values)) {
        return false;
    }
    for (unsigned i = 0; i < 4; i++) {
        double value;
        uint32_t component;

        if (!isfinite(scale[i]) || !isfinite(bias[i])) {
            return false;
        }
        value = values[i] * scale[i] + bias[i];
        component = CLAMP(value, 0.0, 1.0) * 255.0 + 0.5;
        pixel |= component << (24 - i * 8);
    }
    *rgba = pixel;
    return true;
}

bool sgi_kona_gl_texture_pixel_rgba(uint32_t format, uint32_t type,
                                    const uint32_t *source, bool swap_bytes,
                                    uint32_t *rgba)
{
    static const float scale[] = { 1, 1, 1, 1 };
    static const float bias[] = { 0, 0, 0, 0 };

    return sgi_kona_gl_texture_pixel_transfer(format, type, source, swap_bytes,
                                               scale, bias, rgba);
}

bool sgi_kona_gl_pixel_rgba_double(uint32_t format, uint32_t type,
                                   const uint32_t *source, bool swap_bytes,
                                   double rgba[4])
{
    double values[4];

    if (!source || !rgba ||
        !sgi_kona_gl_pixel_values(format, type, source, swap_bytes, values)) {
        return false;
    }
    memcpy(rgba, values, sizeof(values));
    return true;
}

bool sgi_kona_gl_texture_pixel_depth(uint32_t type, const uint32_t *source,
                                     bool swap_bytes, uint32_t scale,
                                     uint32_t bias, uint32_t *depth)
{
    double rgba[4], value;
    float zscale, zbias;

    memcpy(&zscale, &scale, sizeof(zscale));
    memcpy(&zbias, &bias, sizeof(zbias));

    if (!depth || type > 6 || !isfinite(zscale) || !isfinite(zbias) ||
        !sgi_kona_gl_pixel_rgba_double(0, type, source, swap_bytes, rgba)) {
        return false;
    }
    value = MIN(MAX(rgba[0] * zscale + zbias, 0), 1);
    *depth = (uint32_t)floor(value * UINT32_MAX + 0.5);
    return true;
}

bool sgi_kona_gl_pixel_rgba_float(uint32_t format, uint32_t type,
                                  const uint32_t *source, bool swap_bytes,
                                  float rgba[4])
{
    double values[4];

    if (!sgi_kona_gl_pixel_values(format, type, source, swap_bytes, values)) {
        return false;
    }
    for (unsigned i = 0; i < 4; i++) {
        rgba[i] = values[i];
    }
    return true;
}

bool sgi_kona_gl_texture_filter_curve(const uint32_t words[33],
                                      double values[64])
{
    double decoded[64];

    if (!words || !values) {
        return false;
    }
    for (unsigned i = 0; i < 16; i++) {
        uint32_t first = words[2 * i], last = words[2 * i + 1];
        uint32_t code[4] = {
            first & 0x3ff, (first >> 10) & 0x3ff,
            (first >> 20) & 0x3ff, (first >> 30) | (last << 2),
        };

        if (last & ~0xffu) {
            return false;
        }
        for (unsigned j = 0; j < 4; j++) {
            int mantissa = (int8_t)(code[j] >> 2);

            decoded[4 * i + j] = mantissa * (1 << (code[j] & 3)) / 128.0;
        }
    }
    memcpy(values, decoded, sizeof(decoded));
    return true;
}

static uint64_t sgi_kona_gl_bitmap_words_before(const SGIKonaGLBitmapLayout *l,
                                                uint32_t row)
{
    uint64_t cycle = 0, tail = 0;

    /* Byte phases repeat after at most four rows. */
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t phase = (l->phase + i * l->stride) & 3;
        uint64_t words = (phase + l->row_bytes + 3) / 4;

        cycle += words;
        if (i < row % 4) {
            tail += words;
        }
    }
    return cycle * (row / 4) + tail;
}

bool sgi_kona_gl_bitmap_pio_layout(const uint32_t header[9],
                                   uint32_t row_length, uint32_t skip_rows,
                                   uint32_t skip_pixels, uint32_t alignment,
                                   SGIKonaGLBitmapLayout *layout)
{
    SGIKonaGLBitmapLayout l = { 0 };
    uint64_t length, bytes, padding;

    if (!header || !layout || !header[0] || !header[1] ||
        header[2] > 3 || header[3] || header[4] > 1 ||
        (alignment != 1 && alignment != 2 &&
         alignment != 4 && alignment != 8)) {
        return false;
    }
    l.width = header[0];
    l.height = header[1];
    length = row_length ? row_length : l.width;
    bytes = (length + 7) / 8;
    l.stride = (bytes + alignment - 1) & ~(uint64_t)(alignment - 1);
    padding = l.stride - bytes;
    l.strided = padding >= 4 || length != l.width || skip_pixels;
    if (header[4] != l.strided) {
        return false;
    }
    l.phase = (header[2] + skip_rows * l.stride + skip_pixels / 8) & 3;
    l.bit_skip = skip_pixels & 7;
    l.row_bytes = ((uint64_t)l.width + l.bit_skip + 7) / 8;
    if (l.strided) {
        l.payload_words = sgi_kona_gl_bitmap_words_before(&l, l.height);
    } else {
        uint64_t span = l.height * l.stride - padding;

        l.payload_words = (l.phase + span + 3) / 4;
    }
    *layout = l;
    return true;
}

bool sgi_kona_gl_bitmap_dma_layout(const uint32_t header[9],
                                   uint32_t row_length, uint32_t skip_rows,
                                   uint32_t skip_pixels, uint32_t alignment,
                                   SGIKonaGLBitmapDMALayout *layout)
{
    SGIKonaGLBitmapDMALayout d = { 0 };
    SGIKonaGLBitmapLayout *l = &d.bitmap;
    uint32_t pio_header[9];

    if (!header || !layout || header[3] != 1) {
        return false;
    }
    memcpy(pio_header, header, sizeof(pio_header));
    pio_header[3] = 0;
    if (!sgi_kona_gl_bitmap_pio_layout(pio_header, row_length, skip_rows,
                                       skip_pixels, alignment, l) ||
        l->stride < l->row_bytes) {
        return false;
    }
    if (l->strided) {
        d.lines = l->height;
        d.line_bytes = l->row_bytes;
        d.pitch = l->stride;
        d.source_phase = l->phase;
        if (umul64_overflow(d.lines, d.line_bytes, &d.total_bytes) ||
            umul64_overflow(d.lines - 1, d.pitch, &d.span) ||
            uadd64_overflow(d.span, d.line_bytes, &d.span)) {
            return false;
        }
    } else {
        d.lines = 1;
        if (umul64_overflow(l->payload_words, 4, &d.total_bytes)) {
            return false;
        }
        d.line_bytes = d.pitch = d.span = d.total_bytes;
    }
    if (d.total_bytes > UINT32_MAX || d.line_bytes > UINT32_MAX ||
        d.pitch - d.line_bytes > UINT32_MAX) {
        return false;
    }
    *layout = d;
    return true;
}

bool sgi_kona_gl_bitmap_pio_offset(const SGIKonaGLBitmapLayout *l,
                                   uint32_t row, uint64_t byte,
                                   uint64_t *offset)
{
    uint64_t start, pos;

    if (!l || !offset || row >= l->height || byte >= l->row_bytes) {
        return false;
    }
    if (l->strided) {
        uint32_t phase = (l->phase + row * l->stride) & 3;

        if (umul64_overflow(sgi_kona_gl_bitmap_words_before(l, row),
                           4, &start) ||
            uadd64_overflow(start, phase, &start)) {
            return false;
        }
    } else if (umul64_overflow(row, l->stride, &start) ||
               uadd64_overflow(start, l->phase, &start)) {
        return false;
    }
    if (uadd64_overflow(start, byte, &pos)) {
        return false;
    }
    *offset = pos;
    return true;
}

bool sgi_kona_gl_bitmap_pio_byte(const SGIKonaGLBitmapLayout *l,
                                 const uint32_t *words, size_t nwords,
                                 bool lsb_first, uint32_t row, uint32_t byte,
                                 uint8_t *out)
{
    uint64_t first_bit = (uint64_t)byte * 8;
    uint64_t start;
    uint8_t value = 0;

    if (!l || !words || !out || row >= l->height || first_bit >= l->width) {
        return false;
    }
    if (!sgi_kona_gl_bitmap_pio_offset(l, row, 0, &start)) {
        return false;
    }
    for (uint32_t i = 0; i < 8 && first_bit + i < l->width; i++) {
        uint64_t source_bit = first_bit + i + l->bit_skip;
        uint64_t pos = start + source_bit / 8;
        uint32_t raw, bit = source_bit & 7;

        if (pos / 4 >= nwords) {
            return false;
        }
        raw = (words[pos / 4] >> (24 - 8 * (pos & 3))) & 0xff;
        value |= ((raw >> (lsb_first ? bit : 7 - bit)) & 1) << (7 - i);
    }
    *out = value;
    return true;
}

static bool sgi_kona_gl_pixels_words_before(const SGIKonaGLPixelLayout *l,
                                            uint32_t row, uint64_t *out)
{
    uint64_t cycle = 0, tail = 0, full;

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t phase = (l->phase + i * (l->stride & 3)) & 3;
        uint64_t words = (phase + l->row_bytes + 3) / 4;

        cycle += words;
        if (i < row % 4) {
            tail += words;
        }
    }
    if (umul64_overflow(cycle, row / 4, &full) ||
        uadd64_overflow(full, tail, out)) {
        return false;
    }
    return true;
}

bool sgi_kona_gl_pixels_pio_layout(const uint32_t header[8],
                                   uint32_t row_length, uint32_t skip_rows,
                                   uint32_t skip_pixels, uint32_t alignment,
                                   SGIKonaGLPixelLayout *layout)
{
    SGIKonaGLPixelLayout l = { 0 };
    uint64_t length, bytes, padding, span;

    if (!header || !layout || !header[0] || !header[1] || header[4] > 3 ||
        header[5] || header[6] > 1 ||
        (alignment != 1 && alignment != 2 &&
         alignment != 4 && alignment != 8)) {
        return false;
    }
    if (header[3] == 7 && (header[2] == 11 || header[2] == 12)) {
        uint32_t bitmap_header[9] = { header[0], header[1], header[4],
                                     0, header[6] };
        SGIKonaGLBitmapLayout bitmap;

        if (!sgi_kona_gl_bitmap_pio_layout(bitmap_header, row_length,
            skip_rows, skip_pixels, alignment, &bitmap)) {
            return false;
        }
        *layout = (SGIKonaGLPixelLayout) {
            .width = bitmap.width, .height = bitmap.height,
            .stride = bitmap.stride, .row_bytes = bitmap.row_bytes,
            .payload_words = bitmap.payload_words, .phase = bitmap.phase,
            .strided = bitmap.strided,
        };
        return true;
    }
    l.pixel_bytes = sgi_kona_gl_native_pixel_bytes(header[2], header[3]);
    if (!l.pixel_bytes) {
        return false;
    }
    l.width = header[0];
    l.height = header[1];
    length = row_length ? row_length : l.width;
    bytes = length * l.pixel_bytes;
    l.row_bytes = (uint64_t)l.width * l.pixel_bytes;
    l.stride = (bytes + alignment - 1) & ~(uint64_t)(alignment - 1);
    padding = l.stride - bytes;
    l.strided = padding >= 4 || length != l.width || skip_pixels;
    if (header[6] != l.strided) {
        return false;
    }
    l.phase = (header[4] + skip_rows * (l.stride & 3) +
               skip_pixels * (l.pixel_bytes & 3)) & 3;
    if (l.strided) {
        if (!sgi_kona_gl_pixels_words_before(&l, l.height, &l.payload_words)) {
            return false;
        }
    } else {
        if (umul64_overflow(l.height - 1, l.stride, &span) ||
            uadd64_overflow(span, l.row_bytes + l.phase + 3, &span)) {
            return false;
        }
        l.payload_words = span / 4;
    }
    *layout = l;
    return true;
}

bool sgi_kona_gl_pixels_dma_layout(const uint32_t header[8],
                                   uint32_t row_length, uint32_t skip_rows,
                                   uint32_t skip_pixels, uint32_t alignment,
                                   SGIKonaGLPixelDMALayout *layout)
{
    SGIKonaGLPixelDMALayout d = { 0 };
    SGIKonaGLPixelLayout *l = &d.pixels;
    uint32_t pio_header[8];

    if (!header || !layout || header[5] != 1 || header[7]) {
        return false;
    }
    memcpy(pio_header, header, sizeof(pio_header));
    pio_header[5] = 0;
    if (!sgi_kona_gl_pixels_pio_layout(pio_header, row_length, skip_rows,
                                       skip_pixels, alignment, l) ||
        l->stride < l->row_bytes ||
        umul64_overflow(l->height - 1, l->stride, &d.span) ||
        uadd64_overflow(d.span, l->row_bytes, &d.span)) {
        return false;
    }
    if (header[3] == 7 && (header[2] == 11 || header[2] == 12)) {
        uint32_t bitmap_header[9] = { header[0], header[1], header[4],
                                     1, header[6] };
        SGIKonaGLBitmapDMALayout bitmap;

        if (!sgi_kona_gl_bitmap_dma_layout(bitmap_header, row_length,
            skip_rows, skip_pixels, alignment, &bitmap)) {
            return false;
        }
        d.total_bytes = bitmap.total_bytes;
        d.line_bytes = bitmap.line_bytes;
        d.pitch = bitmap.pitch;
        d.span = bitmap.span;
        d.lines = bitmap.lines;
        d.source_phase = bitmap.source_phase;
        *layout = d;
        return true;
    }
    d.source_phase = l->phase;
    if (l->strided) {
        d.lines = l->height;
        d.line_bytes = l->row_bytes;
        d.pitch = l->stride;
        if (umul64_overflow(d.lines, d.line_bytes, &d.total_bytes)) {
            return false;
        }
    } else {
        d.lines = 1;
        d.total_bytes = d.line_bytes = d.pitch = d.span;
    }
    if (d.total_bytes > UINT32_MAX || d.line_bytes > UINT32_MAX ||
        d.pitch - d.line_bytes > UINT32_MAX) {
        return false;
    }
    *layout = d;
    return true;
}

bool sgi_kona_gl_pixels_pio_offset(const SGIKonaGLPixelLayout *l,
                                   uint32_t row, uint64_t byte,
                                   uint64_t *offset)
{
    uint64_t start, pos;

    if (!l || !offset || row >= l->height || byte >= l->row_bytes) {
        return false;
    }
    if (l->strided) {
        uint32_t phase = (l->phase + row * (l->stride & 3)) & 3;

        if (!sgi_kona_gl_pixels_words_before(l, row, &start) ||
            umul64_overflow(start, 4, &start) ||
            uadd64_overflow(start, phase, &start)) {
            return false;
        }
    } else if (umul64_overflow(row, l->stride, &start) ||
               uadd64_overflow(start, l->phase, &start)) {
        return false;
    }
    if (uadd64_overflow(start, byte, &pos)) {
        return false;
    }
    *offset = pos;
    return true;
}

bool sgi_kona_gl_pixels_pio_byte(const SGIKonaGLPixelLayout *l,
                                 const uint32_t *words, size_t nwords,
                                 uint32_t row, uint64_t byte, uint8_t *out)
{
    uint64_t pos;

    if (!words || !out ||
        !sgi_kona_gl_pixels_pio_offset(l, row, byte, &pos) ||
        pos / 4 >= nwords) {
        return false;
    }
    *out = words[pos / 4] >> (24 - 8 * (pos & 3));
    return true;
}

bool sgi_kona_gl_pixel_pack_double(uint32_t format, uint32_t type,
                                   const double rgba[4], bool swap_bytes,
                                   uint8_t *out, size_t capacity)
{
    uint8_t packed[16];
    double values[4];
    unsigned components = sgi_kona_gl_texture_pixel_bytes(format, 1);
    unsigned bytes = sgi_kona_gl_texture_pixel_bytes(format, type);
    unsigned size;
    const SGIKonaGLPackedPixel *pixel = sgi_kona_gl_packed_pixel(format, type);

    if (!rgba || !out || !bytes || capacity < bytes) {
        return false;
    }
    size = bytes / components;
    for (unsigned i = 0; i < 4; i++) {
        if (!isfinite(rgba[i])) {
            return false;
        }
        values[i] = rgba[i];
    }
    if (format < 3) {
        values[0] = values[format];
    } else if (format == 3) {
        values[0] = values[3];
    } else if (format == 6 || format == 7) {
        /* ReadPixels luminance sums channels before clamping. */
        values[0] = values[0] + values[1] + values[2];
        values[1] = values[3];
    } else if (format == 8) {
        for (unsigned k = 0; k < 4; k++) {
            values[k] = rgba[3 - k];
        }
    } else if (format == 9 || format == 10) {
        values[0] = rgba[2];
        values[2] = rgba[0];
    }
    if (pixel) {
        uint32_t bits = 0;
        unsigned shift = 8 * bytes;

        for (unsigned i = 0; i < components; i++) {
            uint32_t maximum = (1u << pixel->bits[i]) - 1;
            double value = CLAMP(values[i], 0.0, 1.0);
            uint32_t component = floor(value * maximum + 0.5);

            shift -= pixel->bits[i];
            bits |= component << shift;
        }
        if (bytes == 1) {
            packed[0] = bits;
        } else if (bytes == 2) {
            stw_be_p(packed, swap_bytes ? bswap16(bits) : bits);
        } else {
            stl_be_p(packed, swap_bytes ? bswap32(bits) : bits);
        }
        memcpy(out, packed, bytes);
        return true;
    }
    for (unsigned i = 0; i < components; i++) {
        double value = CLAMP(values[i], 0.0, 1.0);
        uint8_t *dest = packed + i * size;
        uint32_t bits;

        if (type == 6) {
            float component = value;

            memcpy(&bits, &component, sizeof(bits));
        } else {
            double maximum = size == 4 ? 4294967295.0 :
                                         (1u << (8 * size)) - 1;
            double converted = value * maximum;

            if (!(type & 1)) {
                converted = (converted - 1) / 2;
            }
            /* Nearest conversion with half ties toward positive infinity. */
            bits = floor(converted + 0.5);
        }
        if (size == 1) {
            dest[0] = bits;
        } else if (size == 2) {
            stw_be_p(dest, swap_bytes ? bswap16(bits) : bits);
        } else {
            stl_be_p(dest, swap_bytes ? bswap32(bits) : bits);
        }
    }
    memcpy(out, packed, bytes);
    return true;
}

bool sgi_kona_gl_pixel_pack(uint32_t format, uint32_t type,
                            const float rgba[4], bool swap_bytes,
                            uint8_t *out, size_t capacity)
{
    double values[4];

    if (!rgba) {
        return false;
    }
    for (unsigned i = 0; i < 4; i++) {
        values[i] = rgba[i];
    }
    return sgi_kona_gl_pixel_pack_double(format, type, values, swap_bytes,
                                          out, capacity);
}
